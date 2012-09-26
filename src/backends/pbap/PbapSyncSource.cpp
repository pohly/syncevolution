/*
 * Copyright (C) 2007-2009 Patrick Ohly <patrick.ohly@gmx.de>
 * Copyright (C) 2009 Intel Corporation
 * Copyright (C) 2012 BMW Car IT GmbH. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#include "config.h"

#ifdef ENABLE_PBAP

#include "PbapSyncSource.h"

// SyncEvolution includes a copy of Boost header files.
// They are safe to use without creating additional
// build dependencies. boost::filesystem requires a
// library and therefore is avoided here. Some
// utility functions from SyncEvolution are used
// instead, plus standard C/Posix functions.
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/tokenizer.hpp>

#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <pcrecpp.h>
#include <algorithm>

#include <syncevo/GLibSupport.h> // PBAP backend does not compile without GLib.
#include <syncevo/util.h>
#include <syncevo/BoostHelper.h>

#include "gdbus-cxx-bridge.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/bind.hpp>

#include <syncevo/SyncContext.h>
#include <syncevo/declarations.h>
SE_BEGIN_CXX

#define OBC_SERVICE "org.openobex.client"
#define OBC_SERVICE_NEW "org.bluez.obex.client"
#define OBC_CLIENT_INTERFACE "org.openobex.Client"
#define OBC_CLIENT_INTERFACE_NEW "org.bluez.obex.Client"
#define OBC_PBAP_INTERFACE "org.openobex.PhonebookAccess"
#define OBC_PBAP_INTERFACE_NEW "org.bluez.obex.PhonebookAccess"

class PbapSession : private boost::noncopyable {
public:
    static boost::shared_ptr<PbapSession> create(PbapSyncSource &parent);

    void initSession(const std::string &address, const std::string &format);

    typedef std::map<std::string, pcrecpp::StringPiece> Content;
    typedef std::map<std::string, boost::variant<std::string> > Params;

    void pullAll(Content &dst, std::string &buffer, pcrecpp::StringPiece &memRange);
    
    void shutdown(void);

private:
    PbapSession(PbapSyncSource &parent);

    PbapSyncSource &m_parent;
    boost::weak_ptr<PbapSession> m_self;
    std::auto_ptr<GDBusCXX::DBusRemoteObject> m_client;
    bool m_newobex;
    /**
     * m_transferComplete will be set to true when observing a
     * "Complete" signal on a transfer object path which has the
     * current session as prefix.
     *
     * It also gets set when an error occurred for such a transfer,
     * in which case m_error will also be set.
     *
     * This only works as long as the session is only used for a
     * single transfer. Otherwise a more complex tracking of
     * completion, for example per transfer object path, is needed.
     */
    bool m_transferComplete;
    std::string m_transferErrorCode;
    std::string m_transferErrorMsg;
    
    std::auto_ptr<GDBusCXX::SignalWatch1<GDBusCXX::Path_t> > m_completeSignal;
    std::auto_ptr<GDBusCXX::SignalWatch3<GDBusCXX::Path_t, std::string, std::string> >
        m_errorSignal;
    
    void completeCb(const GDBusCXX::Path_t &path);
    void errorCb(const GDBusCXX::Path_t &path, const std::string &error,
                 const std::string &msg);
        
    std::auto_ptr<GDBusCXX::DBusRemoteObject> m_session;
};

PbapSession::PbapSession(PbapSyncSource &parent) :
    m_parent(parent),
    m_transferComplete(false)
{
}

boost::shared_ptr<PbapSession> PbapSession::create(PbapSyncSource &parent)
{
    boost::shared_ptr<PbapSession> session(new PbapSession(parent));
    session->m_self = session;
    return session;
}

void PbapSession::completeCb(const GDBusCXX::Path_t &path)
{
    SE_LOG_DEBUG(NULL, NULL, "obexd transfer %s completed", path.c_str());
    m_transferComplete = true;
}

void PbapSession::errorCb(const GDBusCXX::Path_t &path,
                          const std::string &error,
                          const std::string &msg)
{
    SE_LOG_DEBUG(NULL, NULL, "obexd transfer %s failed: %s %s",
                 path.c_str(), error.c_str(), msg.c_str());
    m_transferComplete = true;
    m_transferErrorCode = error;
    m_transferErrorMsg = msg;
}

void PbapSession::initSession(const std::string &address, const std::string &format)
{
    if (m_session.get()) {
        return;
    }

    // format string uses:
    // [(2.1|3.0):][^]propname,propname,...
    //
    // 3.0:^PHOTO = download in vCard 3.0 format, excluding PHOTO
    // 2.1:PHOTO = download in vCard 2.1 format, only the PHOTO

    std::string version;
    std::string tmp;
    std::string properties;
    const pcrecpp::RE re("(?:(2\\.1|3\\.0):?)?(\\^?)([-a-zA-Z,]*)");
    if (!re.FullMatch(format, &version, &tmp, &properties)) {
        m_parent.throwError(StringPrintf("invalid specification of PBAP vCard format (databaseFormat): %s",
                                         format.c_str()));
    }
    char negated = tmp.c_str()[0];
    if (version.empty()) {
        // same default as in obexd
        version = "2.1";
    }
    if (version != "2.1" && version != "3.0") {
        m_parent.throwError(StringPrintf("invalid vCard version prefix in PBAP vCard format specification (databaseFormat): %s",
                                         format.c_str()));
    }
    std::set<std::string> keywords;
    boost::split(keywords, properties, boost::is_from_range(',', ','));
    typedef std::map<std::string, boost::variant<std::string> > Params;

    Params params;
    params["Target"] = std::string("PBAP");

    std::string session;
    GDBusCXX::DBusConnectionPtr conn = GDBusCXX::dbus_get_bus_connection("SESSION", NULL, true, NULL);

    // We must attempt to use the new interface, otherwise we won't know whether
    // the daemon exists or can be started.
    m_newobex = true;
    m_client.reset(new GDBusCXX::DBusRemoteObject(conn, "/", OBC_CLIENT_INTERFACE_NEW, 
                                                  OBC_SERVICE_NEW, true));
    try {
        SE_LOG_DEBUG(NULL, NULL, "trying to use new obexd service %s", OBC_SERVICE_NEW);
        session =
            GDBusCXX::DBusClientCall1<GDBusCXX::DBusObject_t>(*m_client, "CreateSession")(address, params);
    } catch (const std::exception &error) {
        if (!strstr(error.what(), "org.freedesktop.DBus.Error.ServiceUnknown")) {
            throw;
        }
        // Fall back to old interface.
        SE_LOG_DEBUG(NULL, NULL, "new obexd service not available (%s), falling back to old one %s",
                     error.what(),
                     OBC_SERVICE);
        m_newobex = false;
    }

    if (!m_newobex) {
        m_client.reset(new GDBusCXX::DBusRemoteObject(conn, "/", OBC_CLIENT_INTERFACE,
                                                      OBC_SERVICE, true));
        params["Destination"] = std::string(address);
        session = GDBusCXX::DBusClientCall1<GDBusCXX::DBusObject_t>(*m_client, "CreateSession")(params);
    }

    if (session.empty()) {
        m_parent.throwError("PBAP: got empty session from CreateSession()");
    }

    if (m_newobex) {
        m_session.reset(new GDBusCXX::DBusRemoteObject(m_client->getConnection(),
                        session,
                        OBC_PBAP_INTERFACE_NEW,
                        OBC_SERVICE_NEW,
                        true));
        
        // Filter Transfer signals via path prefix. Discussions on Bluez
        // list showed that this is meant to be possible, even though the
        // client-api.txt documentation itself didn't (and still doesn't)
        // make it clear:
        // "[PATCH obexd v0] client-doc: Guarantee prefix in transfer paths"
        // http://www.spinics.net/lists/linux-bluetooth/msg28409.html
        m_completeSignal.reset(new GDBusCXX::SignalWatch1<GDBusCXX::Path_t>
                               (GDBusCXX::SignalFilter(m_client->getConnection(),
                               session,
                               "org.bluez.obex.Transfer",
                               "Complete",
                               GDBusCXX::SignalFilter::SIGNAL_FILTER_PATH_PREFIX)));
        // Be extra careful with asynchronous callbacks: bind to weak
        // pointer and ignore callback when the instance is already gone.
        // Should not happen with signals (destructing the class unregisters
        // the watch), but very well may happen in asynchronous method
        // calls. Therefore maintain m_self and show how to use it here.
        m_completeSignal->activate(boost::bind(&PbapSession::completeCb, m_self, _1));

        // same for error
        m_errorSignal.reset(new GDBusCXX::SignalWatch3<GDBusCXX::Path_t, std::string, std::string>
                            (GDBusCXX::SignalFilter(m_client->getConnection(),
                            session,
                            "org.bluez.obex.Transfer",
                            "Error",
                            GDBusCXX::SignalFilter::SIGNAL_FILTER_PATH_PREFIX)));
        m_errorSignal->activate(boost::bind(&PbapSession::errorCb, m_self, _1, _2, _3));

    } else {
        m_session.reset(new GDBusCXX::DBusRemoteObject(m_client->getConnection(),
                                                   session,
                                                   OBC_PBAP_INTERFACE,
                                                   OBC_SERVICE,
                                                   true));
    }

    SE_LOG_DEBUG(NULL, NULL, "PBAP session created: %s", m_session->getPath());

    // get filter list so that we can continue validating our format specifier
    std::vector<std::string> filterFields =
        GDBusCXX::DBusClientCall1< std::vector<std::string> >(*m_session, "ListFilterFields")();

    std::list<std::string> filter;
    if (negated) {
        // negated, start with everything set
        filter.insert(filter.begin(), filterFields.begin(), filterFields.end());
    }

    // validate parameters and update filter
    BOOST_FOREACH (const std::string &prop, keywords) {
        if (prop.empty()) {
            continue;
        }

        std::vector<std::string>::const_iterator entry =
            std::find_if(filterFields.begin(),
                         filterFields.end(),
                         boost::bind(&boost::iequals<std::string,std::string>, _1, prop, std::locale()));

        if (entry == filterFields.end()) {
            m_parent.throwError(StringPrintf("invalid property name in PBAP vCard format specification (databaseFormat): %s",
                                             prop.c_str()));
        }

        if (negated) {
            filter.remove(*entry);
        } else {
            filter.push_back(*entry);
        }
    }

    GDBusCXX::DBusClientCall0(*m_session, "Select")(std::string("int"), std::string("PB"));
    GDBusCXX::DBusClientCall0(*m_session, "SetFilter")(std::vector<std::string>(filter.begin(), filter.end()));
    GDBusCXX::DBusClientCall0(*m_session, "SetFormat")(std::string(version == "2.1" ? "vcard21" : "vcard30"));

    SE_LOG_DEBUG(NULL, NULL, "PBAP session initialized");
}

void PbapSession::pullAll(Content &dst, std::string &buffer, pcrecpp::StringPiece &memRange)
{
    pcrecpp::StringPiece content;
    if (m_newobex) {
        char *addr;
        class TmpFileGuard {
            int m_fd;
            PlainGStr m_filename;
        public:
            /**
             * Create temporary file. Will be closed and unlinked when
             * this instance destructs.
             */
            TmpFileGuard(PbapSyncSource &parent) {
                gchar *filename;
                GErrorCXX gerror;
                m_fd = g_file_open_tmp (NULL, &filename, gerror);
                m_filename = filename;
                if (m_fd == -1) {
                    parent.throwError(std::string("opening temporary file for PBAP: ") +
                                      (gerror ? gerror->message : "unknown failure"));
                }
            }
            ~TmpFileGuard() {
                // Unlink before closing to avoid race condition
                // (close, someone else opens file, we remove it).
                if (remove(m_filename) == -1) {
                    // Continue despite error.
                    SE_LOG_ERROR(NULL, NULL, "Unable to remove temporary file %s: %s",
                                 m_filename.get(), strerror(errno));
                }
                close (m_fd);
            }

            const char *getFilename() const { return m_filename; }
            int getFD() const { return m_fd; }
        } tmpfile(m_parent);

        SE_LOG_DEBUG(NULL, NULL, "Created temporary file for PullAll %s", tmpfile.getFilename());
        GDBusCXX::DBusClientCall1<std::pair<GDBusCXX::DBusObject_t, Params> > pullall(*m_session, "PullAll");
        std::pair<GDBusCXX::DBusObject_t, Params> tuple = pullall(std::string(tmpfile.getFilename()));
        const GDBusCXX::DBusObject_t &transfer = tuple.first;
        const Params &properties = tuple.second;
        
        SE_LOG_DEBUG(NULL, NULL, "pullall transfer path %s, %ld properties", transfer.c_str(), (long)properties.size());

        while (!m_transferComplete) {
            g_main_context_iteration(NULL, true);
        }
        if (!m_transferErrorCode.empty()) {
            m_parent.throwError(StringPrintf("%s: %s",
                                             m_transferErrorCode.c_str(),
                                             m_transferErrorMsg.c_str()));
        }

        struct stat sb;
        if (fstat(tmpfile.getFD(), &sb) == -1) {
            m_parent.throwError("stat on PBAP temp file", errno);
        }
        SE_LOG_DEBUG(NULL, NULL, "Temporary file size is %ld", (long)sb.st_size);

        addr = (char*)mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, tmpfile.getFD(), 0);
        if (addr == MAP_FAILED) {
            m_parent.throwError("mmap temporary file", errno);
        }
        memRange.set(addr, sb.st_size);
        content = memRange;

    } else {
        GDBusCXX::DBusClientCall1<std::string> pullall(*m_session, "PullAll");
        buffer = pullall();
        content = buffer;
    }

    pcrecpp::StringPiece vcarddata;
    int count = 0;
    pcrecpp::RE re("[\\r\\n]*(^BEGIN:VCARD.*?^END:VCARD)",
                   pcrecpp::RE_Options().set_dotall(true).set_multiline(true));
    while (re.Consume(&content, &vcarddata)) {
        std::string id = StringPrintf("%d", count);
        dst[id] = vcarddata;
        ++count;
    }

    SE_LOG_DEBUG(NULL, NULL, "PBAP content pulled: %d entries", (int) dst.size());
}

void PbapSession::shutdown(void)
{
    GDBusCXX::DBusClientCall0 removeSession(*m_client, "RemoveSession");

    // always clear pointer, even if method call fails
    GDBusCXX::DBusObject_t path(m_session->getPath());
    //m_session.reset();
    SE_LOG_DEBUG(NULL, NULL, "removed session: %s", path.c_str());

    removeSession(path);

    SE_LOG_DEBUG(NULL, NULL, "PBAP session closed");
}

PbapSyncSource::PbapSyncSource(const SyncSourceParams &params) :
    TrackingSyncSource(params)
{
    m_session = PbapSession::create(*this);
}

PbapSyncSource::~PbapSyncSource()
{
    if (m_memRange.data()) {
        munmap(const_cast<char *>(m_memRange.data()), m_memRange.size());
    }
}

std::string PbapSyncSource::getMimeType() const
{
    return "text/x-vcard";
}

std::string PbapSyncSource::getMimeVersion() const
{
    return "2.1";
}

void PbapSyncSource::open()
{
    string database = getDatabaseID();
    const string prefix("obex-bt://");

    if (!boost::starts_with(database, prefix)) {
        throwError("database should specifiy device address (obex-bt://<bt-addr>)");
    }

    std::string address = database.substr(prefix.size());

    m_session->initSession(address, getDatabaseFormat());
    m_session->pullAll(m_content, m_buffer, m_memRange);
    m_session->shutdown();
}

bool PbapSyncSource::isEmpty()
{
    return m_content.empty();
}

void PbapSyncSource::close()
{
}

PbapSyncSource::Databases PbapSyncSource::getDatabases()
{
    Databases result;

    result.push_back(Database("select database via bluetooth address",
                              "[obex-bt://]<bt-addr>"));
    return result;
}

void PbapSyncSource::listAllItems(RevisionMap_t &revisions)
{
    typedef std::pair<std::string, pcrecpp::StringPiece> Entry;
    BOOST_FOREACH(const Entry &entry, m_content) {
        revisions[entry.first] = "0";
    }
}

void PbapSyncSource::readItem(const string &uid, std::string &item, bool raw)
{
    Content::iterator it = m_content.find(uid);
    if(it != m_content.end()) {
        item = it->second.as_string();
    }
}

TrackingSyncSource::InsertItemResult PbapSyncSource::insertItem(const string &uid, const std::string &item, bool raw)
{
    throwError("Operation not supported");
    return InsertItemResult();
}

void PbapSyncSource::removeItem(const string &uid)
{
    throwError("Operation not supported");
}

SE_END_CXX

#endif /* ENABLE_PBAP */

#ifdef ENABLE_MODULES
# include "PbapSyncSourceRegister.cpp"
#endif
