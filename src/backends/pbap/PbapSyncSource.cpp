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

#include <pcrecpp.h>
#include <algorithm>

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
    static boost::shared_ptr<PbapSession> create();

    void initSession(const std::string &address, const std::string &format);

    typedef std::map<std::string, std::string> Content;
    typedef std::map<std::string, boost::variant<std::string> > Params;

    void pullAll(Content &dst);
    
    void shutdown(void);

private:
    PbapSession(void);
    
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

PbapSession::PbapSession(void) :
    m_transferComplete(false)
{
    GDBusCXX::DBusConnectionPtr session = GDBusCXX::dbus_get_bus_connection("SESSION", NULL, true, NULL);
    m_client.reset(new GDBusCXX::DBusRemoteObject(session, "/", OBC_CLIENT_INTERFACE_NEW, 
                                          OBC_SERVICE_NEW, true));
    if (!m_client.get()) {
        m_client.reset(new GDBusCXX::DBusRemoteObject(session, "/", OBC_CLIENT_INTERFACE,
                                              OBC_SERVICE, true));
        m_newobex = false;
        SE_LOG_DEBUG(NULL, NULL, "Using old obex %s", OBC_CLIENT_INTERFACE);
    } else {
        m_newobex = true;
        SE_LOG_DEBUG(NULL, NULL, "Using new obex %s", OBC_CLIENT_INTERFACE_NEW);
    }
}

boost::shared_ptr<PbapSession> PbapSession::create()
{
    boost::shared_ptr<PbapSession> session(new PbapSession());
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
        SE_THROW(StringPrintf("invalid specification of PBAP vCard format (databaseFormat): %s",
                              format.c_str()));
    }
    char negated = tmp.c_str()[0];
    if (version.empty()) {
        // same default as in obexd
        version = "2.1";
    }
    if (version != "2.1" && version != "3.0") {
        SE_THROW(StringPrintf("invalid vCard version prefix in PBAP vCard format specification (databaseFormat): %s",
                              format.c_str()));
    }
    std::set<std::string> keywords;
    boost::split(keywords, properties, boost::is_from_range(',', ','));
    typedef std::map<std::string, boost::variant<std::string> > Params;

    GDBusCXX::DBusClientCall1<GDBusCXX::DBusObject_t>
        createSession(*m_client, "CreateSession");

    Params params;
    params["Target"] = std::string("PBAP");

    std::string session;
    if (m_newobex) {
        session = createSession(address, params);
    } else {
        params["Destination"] = std::string(address);
        session = createSession(params);
    }
        
    if (session.empty()) {
        SE_THROW("PBAP: got empty session from CreateSession()");
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
            SE_THROW(StringPrintf("invalid property name in PBAP vCard format specification (databaseFormat): %s",
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

void vcardParse(const std::string &content, std::size_t begin, std::size_t end, std::map<std::string, std::string> &dst)
{
    static const boost::char_separator<char> lineSep("\n\r");

    typedef boost::tokenizer<boost::char_separator<char> > Tokenizer;
    Tokenizer tok(content.begin() + begin, content.begin() + end, lineSep);

    for(Tokenizer::iterator it = tok.begin(); it != tok.end(); it ++) {
        const std::string &line = *it;
        size_t i = line.find(':');
        if(i != std::string::npos) {
            std::size_t j = line.find(';');
            j = (j == std::string::npos)? i : std::min(i, j);
            std::string key = line.substr(0, j);
            std::string value = line.substr(i + 1);
            dst[key] = value;
        }
    }
}

void PbapSession::pullAll(Content &dst)
{
    std::string content;
    if (m_newobex) {
        char *filename;
        GError *error = NULL;
        char *addr;
        gint fd = g_file_open_tmp (NULL, &filename, &error);
        if (error != NULL) {
            SE_LOG_ERROR(NULL, NULL, "Unable to create temporary file for transfer");
            return;
        }
        SE_LOG_DEBUG(NULL, NULL, "Created temporary file for PullAll %s", filename);
        GDBusCXX::DBusClientCall1<std::pair<GDBusCXX::DBusObject_t, Params> > pullall(*m_session, "PullAll");
        std::pair<GDBusCXX::DBusObject_t, Params> tuple = pullall(std::string(filename));
        const GDBusCXX::DBusObject_t &transfer = tuple.first;
        const Params &properties = tuple.second;
        
        SE_LOG_DEBUG(NULL, NULL, "pullall transfer path %s, %ld properties", transfer.c_str(), (long)properties.size());

        while (!m_transferComplete) {
            g_main_context_iteration(NULL, true);
        }

        struct stat sb;
        if (fstat(fd, &sb) == -1) {
            SE_LOG_ERROR(NULL, NULL, "Stat on file failed");
            return;
        }
        SE_LOG_DEBUG(NULL, NULL, "Temporary file size is %ld", (long)sb.st_size);

        addr = (char*)mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (addr == MAP_FAILED) {
            SE_LOG_ERROR(NULL, NULL, "Unable to mmap temporary file");
            return;
        }
        
        pcrecpp::StringPiece content(addr, sb.st_size);
        
        pcrecpp::StringPiece vcarddata;
        int count = 0;
        pcrecpp::RE re("[\\r\\n]*(^BEGIN:VCARD.*?^END:VCARD)",
                       pcrecpp::RE_Options().set_dotall(true).set_multiline(true));
        while (re.Consume(&content, &vcarddata)) {
            std::string id = StringPrintf("%d", count);
            dst[id] = vcarddata.as_string();

            ++count;
        }
        
        close (fd);
        
        if (remove(filename) == -1)
        {
            SE_LOG_ERROR(NULL, NULL, "Unable to remove temporary file");
        }
    } else {
        GDBusCXX::DBusClientCall1<std::string> pullall(*m_session, "PullAll");
        content = pullall();

        // empty content not treated as an error

        typedef std::map<std::string, int> CounterMap;
        CounterMap counterMap;

        std::size_t pos = 0;
        while(pos < content.size()) {
            static const std::string beginStr("BEGIN:VCARD");
            static const std::string endStr("END:VCARD");

            pos = content.find(beginStr, pos);
            if(pos == std::string::npos) {
                break;
            }

            std::size_t endPos = content.find(endStr, pos + beginStr.size());
            if(endPos == std::string::npos) {
                break;
            }

            endPos += endStr.size();

            typedef std::map<std::string, std::string> VcardMap;
            VcardMap vcard;
            vcardParse(content, pos, endPos, vcard);

            VcardMap::const_iterator it = vcard.find("N");
            if(it != vcard.end() && !it->second.empty()) {
                const std::string &fn = it->second;

                const std::pair<CounterMap::iterator, bool> &r =
                    counterMap.insert(CounterMap::value_type(fn, 0));
                if(!r.second) {
                    r.first->second ++;
                }

                char suffix[8];
                sprintf(suffix, "%07d", r.first->second);

                std::string id = fn + std::string(suffix);
                dst[id] = content.substr(pos, endPos - pos);
            }

            pos = endPos;
        }
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
    m_session = PbapSession::create();
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
    m_session->pullAll(m_content);
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
    typedef std::pair<std::string, std::string> Entry;
    BOOST_FOREACH(const Entry &entry, m_content) {
        revisions[entry.first] = "0";
    }
}

void PbapSyncSource::readItem(const string &uid, std::string &item, bool raw)
{
    Content::iterator it = m_content.find(uid);
    if(it != m_content.end()) {
        item = it->second;
    }
}

TrackingSyncSource::InsertItemResult PbapSyncSource::insertItem(const string &uid, const std::string &item, bool raw)
{
    throw std::runtime_error("Operation not supported");
}

void PbapSyncSource::removeItem(const string &uid)
{
    throw std::runtime_error("Operation not supported");
}

SE_END_CXX

#endif /* ENABLE_PBAP */

#ifdef ENABLE_MODULES
# include "PbapSyncSourceRegister.cpp"
#endif
