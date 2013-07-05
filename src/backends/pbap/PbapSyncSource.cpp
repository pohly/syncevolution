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

#include <boost/assign/list_of.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/tokenizer.hpp>

#include <errno.h>
#include <unistd.h>

#include <pcrecpp.h>
#include <algorithm>

#include <syncevo/GLibSupport.h> // PBAP backend does not compile without GLib.
#include <syncevo/util.h>
#include <syncevo/BoostHelper.h>

#include "gdbus-cxx-bridge.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/bind.hpp>

#include <synthesis/SDK_util.h>

#include <syncevo/SyncContext.h>
#include <syncevo/declarations.h>
SE_BEGIN_CXX

#define OBC_SERVICE "org.openobex.client" // obexd < 0.47
#define OBC_SERVICE_NEW "org.bluez.obex.client" // obexd >= 0.47
#define OBC_SERVICE_NEW5 "org.bluez.obex" // obexd in Bluez 5.0
#define OBC_CLIENT_INTERFACE "org.openobex.Client"
#define OBC_CLIENT_INTERFACE_NEW "org.bluez.obex.Client"
#define OBC_CLIENT_INTERFACE_NEW5 "org.bluez.obex.Client1"
#define OBC_PBAP_INTERFACE "org.openobex.PhonebookAccess"
#define OBC_PBAP_INTERFACE_NEW "org.bluez.obex.PhonebookAccess"
#define OBC_PBAP_INTERFACE_NEW5 "org.bluez.obex.PhonebookAccess1"
#define OBC_TRANSFER_INTERFACE_NEW "org.bluez.obex.Transfer"
#define OBC_TRANSFER_INTERFACE_NEW5 "org.bluez.obex.Transfer1"

typedef std::map<int, pcrecpp::StringPiece> Content;

class PullAll
{
    std::string m_buffer; // vCards kept in memory when using old obexd.
    TmpFile m_tmpFile; // Stored in temporary file and mmapped with more recent obexd.
    Content m_content; // Refers to chunks of m_buffer or m_tmpFile without copying them.
    int m_numContacts; // Number of existing contacts, according to GetSize() or after downloading.
    int m_currentContact; // Numbered starting with zero according to discovery in addVCards.
    boost::shared_ptr<PbapSession> m_session; // Only set when there is a transfer ongoing.
    int m_tmpFileOffset; // Number of bytes already parsed.

    friend class PbapSession;
public:
    std::string getNextID();
    bool getContact(int contactNumber, pcrecpp::StringPiece &vcard);
    const char *addVCards(int startIndex, const pcrecpp::StringPiece &content);
};

enum PullData
{
    PULL_AS_CONFIGURED,
    PULL_WITHOUT_PHOTOS
};

class PbapSession : private boost::noncopyable {
public:
    static boost::shared_ptr<PbapSession> create(PbapSyncSource &parent);

    void initSession(const std::string &address, const std::string &format);

    typedef std::map<std::string, pcrecpp::StringPiece> Content;
    typedef std::map<std::string, boost::variant<std::string> > Params;

    boost::shared_ptr<PullAll> startPullAll(PullData pullata);
    void checkForError(); // Throws exception if transfer failed.
    Timespec transferComplete() const;
    void resetTransfer();
    void shutdown(void);

private:
    PbapSession(PbapSyncSource &parent);

    PbapSyncSource &m_parent;
    boost::weak_ptr<PbapSession> m_self;
    std::auto_ptr<GDBusCXX::DBusRemoteObject> m_client;
    enum {
        OBEXD_OLD, // obexd < 0.47
        OBEXD_NEW, // obexd == 0.47, file-based transfer
        // OBEXD_048 // obexd == 0.48, file-based transfer without SetFilter and with filter parameter to PullAll()
        BLUEZ5     // obexd in Bluez >= 5.0
    } m_obexAPI;

    /** filter parameters for BLUEZ5 PullAll */
    typedef std::list<std::string> Properties;
    typedef boost::variant< std::string, Properties > Bluez5Values;
    std::map<std::string, Bluez5Values> m_filter5;
    Properties m_filterFields;
    Properties supportedProperties() const;

    /**
     * m_transferComplete will be set to the current monotonic time when observing a
     * "Complete" signal on a transfer object path which has the
     * current session as prefix. There may be more than one such transfer,
     * so record all completions that we see and then pick the right one.
     *
     * It also gets set when an error occurred for such a transfer,
     * in which case m_error will also be set.
     *
     * This only works as long as the session is only used for a
     * single transfer. Otherwise a more complex tracking of
     * completion, for example per transfer object path, is needed.
     */
    class Completion {
    public:
        Timespec m_transferComplete;
        std::string m_transferErrorCode;
        std::string m_transferErrorMsg;

        static Completion now() {
            Completion res;
            res.m_transferComplete = Timespec::monotonic();
            return res;
        }
    };
    typedef std::map<std::string, Completion> Transfers;
    Transfers m_transfers;
    std::string m_currentTransfer;

    std::auto_ptr<GDBusCXX::SignalWatch3<GDBusCXX::Path_t, std::string, std::string> >
        m_errorSignal;
    void errorCb(const GDBusCXX::Path_t &path, const std::string &error,
                 const std::string &msg);

    // Bluez 5
    typedef GDBusCXX::SignalWatch4<GDBusCXX::Path_t, std::string, Params, std::vector<std::string> > PropChangedSignal_t;
    std::auto_ptr<PropChangedSignal_t> m_propChangedSignal;
    void propChangedCb(const GDBusCXX::Path_t &path,
                       const std::string &interface,
                       const Params &changed,
                       const std::vector<std::string> &invalidated);

    // new obexd API
    typedef GDBusCXX::SignalWatch1<GDBusCXX::Path_t> CompleteSignal_t;
    std::auto_ptr<CompleteSignal_t> m_completeSignal;
    void completeCb(const GDBusCXX::Path_t &path);
    typedef GDBusCXX::SignalWatch3<GDBusCXX::Path_t, std::string, boost::variant<int64_t> > PropertyChangedSignal_t;
    std::auto_ptr<PropertyChangedSignal_t> m_propertyChangedSignal;
    void propertyChangedCb(const GDBusCXX::Path_t &path, const std::string &name, const boost::variant<int64_t> &value);

    std::auto_ptr<GDBusCXX::DBusRemoteObject> m_session;
};

PbapSession::PbapSession(PbapSyncSource &parent) :
    m_parent(parent)
{
}

boost::shared_ptr<PbapSession> PbapSession::create(PbapSyncSource &parent)
{
    boost::shared_ptr<PbapSession> session(new PbapSession(parent));
    session->m_self = session;
    return session;
}

void PbapSession::propChangedCb(const GDBusCXX::Path_t &path,
                                const std::string &interface,
                                const Params &changed,
                                const std::vector<std::string> &invalidated)
{
    // Called for a path which matches the current session, so we know
    // that the signal is for our transfer. Only need to check the status.
    Params::const_iterator it = changed.find("Status");
    if (it != changed.end()) {
        std::string status = boost::get<std::string>(it->second);
        SE_LOG_DEBUG(NULL, "OBEXD transfer %s: %s",
                     path.c_str(), status.c_str());
        Completion completion = Completion::now();
        SE_LOG_DEBUG(NULL, "obexd transfer %s: %s", path.c_str(), status.c_str());
        if (status == "error") {
            // We have to make up some error descriptions. The Bluez
            // 5 API no longer seems to provide that.
            completion.m_transferErrorCode = "transfer failed";
            completion.m_transferErrorMsg = "reason unknown";
        }
        m_transfers[path] = completion;
    }
}

void PbapSession::completeCb(const GDBusCXX::Path_t &path)
{
    SE_LOG_DEBUG(NULL, "obexd transfer %s completed", path.c_str());
    m_transfers[path] = Completion::now();
}

void PbapSession::errorCb(const GDBusCXX::Path_t &path,
                          const std::string &error,
                          const std::string &msg)
{
    SE_LOG_DEBUG(NULL, "obexd transfer %s failed: %s %s",
                 path.c_str(), error.c_str(), msg.c_str());
    Completion &completion = m_transfers[path];
    completion.m_transferComplete = Timespec::monotonic();
    completion.m_transferErrorCode = error;
    completion.m_transferErrorMsg = msg;
}

void PbapSession::propertyChangedCb(const GDBusCXX::Path_t &path,
                                    const std::string &name,
                                    const boost::variant<int64_t> &value)
{
    const int64_t *tmp = boost::get<int64_t>(&value);
    if (tmp) {
        SE_LOG_DEBUG(NULL, "obexd transfer %s property change: %s = %ld",
                     path.c_str(), name.c_str(), (long signed)*tmp);
    } else {
        SE_LOG_DEBUG(NULL, "obexd transfer %s property change: %s",
                     path.c_str(), name.c_str());
    }
}

PbapSession::Properties PbapSession::supportedProperties() const
{
    Properties props;
    static const std::set<std::string> supported =
        boost::assign::list_of("VERSION")
        ("FN")
        ("N")
        ("PHOTO")
        ("BDAY")
        ("ADR")
        ("LABEL")
        ("TEL")
        ("EMAIL")
        ("MAILER")
        ("TZ")
        ("GEO")
        ("TITLE")
        ("ROLE")
        ("LOGO")
        ("AGENT")
        ("ORG")
        ("NOTE")
        ("REV")
        ("SOUND")
        ("URL")
        ("UID")
        ("KEY")
        ("NICKNAME")
        ("CATEGORIES")
        ("CLASS");

    BOOST_FOREACH (const std::string &prop, m_filterFields) {
        // Be conservative and only ask for properties that we
        // really know how to use. obexd also lists the bit field
        // strings ("BIT01") but phones have been seen to reject
        // queries when those were enabled.
        if (supported.find(prop) != supported.end()) {
            props.push_back(prop);
        }
    }
    return props;
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

    // We must attempt to use the new interface(s), otherwise we won't know whether
    // the daemon exists or can be started.
    m_obexAPI = BLUEZ5;
    m_client.reset(new GDBusCXX::DBusRemoteObject(conn,
                                                  "/org/bluez/obex",
                                                  OBC_CLIENT_INTERFACE_NEW5,
                                                  OBC_SERVICE_NEW5, true));
    try {
        SE_LOG_DEBUG(NULL, "trying to use bluez 5 obexd service %s", OBC_SERVICE_NEW5);
        session =
            GDBusCXX::DBusClientCall1<GDBusCXX::DBusObject_t>(*m_client, "CreateSession")(address, params);
    } catch (const std::exception &error) {
        if (!strstr(error.what(), "org.freedesktop.DBus.Error.ServiceUnknown") &&
            !strstr(error.what(), "org.freedesktop.DBus.Error.UnknownObject")) {
            throw;
        }
        // Fall back to old interface.
        SE_LOG_DEBUG(NULL, "bluez obex service not available (%s), falling back to previous obexd one %s",
                     error.what(),
                     OBC_SERVICE_NEW);
        m_obexAPI = OBEXD_NEW;
    }

    if (session.empty()) {
        m_client.reset(new GDBusCXX::DBusRemoteObject(conn, "/", OBC_CLIENT_INTERFACE_NEW,
                                                      OBC_SERVICE_NEW, true));
        try {
            SE_LOG_DEBUG(NULL, "trying to use new obexd service %s", OBC_SERVICE_NEW);
            session =
                GDBusCXX::DBusClientCall1<GDBusCXX::DBusObject_t>(*m_client, "CreateSession")(address, params);
        } catch (const std::exception &error) {
            if (!strstr(error.what(), "org.freedesktop.DBus.Error.ServiceUnknown")) {
                throw;
            }
            // Fall back to old interface.
            SE_LOG_DEBUG(NULL, "new obexd service(s) not available (%s), falling back to old one %s",
                         error.what(),
                         OBC_SERVICE);
            m_obexAPI = OBEXD_OLD;
        }
    }

    if (session.empty()) {
        m_client.reset(new GDBusCXX::DBusRemoteObject(conn, "/", OBC_CLIENT_INTERFACE,
                                                      OBC_SERVICE, true));
        params["Destination"] = std::string(address);
        session = GDBusCXX::DBusClientCall1<GDBusCXX::DBusObject_t>(*m_client, "CreateSession")(params);
    }

    if (session.empty()) {
        m_parent.throwError("PBAP: failed to create session");
    }

    if (m_obexAPI != OBEXD_OLD) {
        m_session.reset(new GDBusCXX::DBusRemoteObject(m_client->getConnection(),
                                                       session,
                                                       m_obexAPI == BLUEZ5 ? OBC_PBAP_INTERFACE_NEW5 : OBC_PBAP_INTERFACE_NEW,
                                                       m_obexAPI == BLUEZ5 ? OBC_SERVICE_NEW5 : OBC_SERVICE_NEW,
                                                       true));
        
        // Filter Transfer signals via path prefix. Discussions on Bluez
        // list showed that this is meant to be possible, even though the
        // client-api.txt documentation itself didn't (and still doesn't)
        // make it clear:
        // "[PATCH obexd v0] client-doc: Guarantee prefix in transfer paths"
        // http://www.spinics.net/lists/linux-bluetooth/msg28409.html
        //
        // Be extra careful with asynchronous callbacks: bind to weak
        // pointer and ignore callback when the instance is already gone.
        // Should not happen with signals (destructing the class unregisters
        // the watch), but very well may happen in asynchronous method
        // calls. Therefore maintain m_self and show how to use it here.
        if (m_obexAPI == BLUEZ5) {
            // Bluez 5
            m_propChangedSignal.reset(new PropChangedSignal_t
                                      (GDBusCXX::SignalFilter(m_client->getConnection(),
                                                              session,
                                                              "org.freedesktop.DBus.Properties",
                                                              "PropertiesChanged",
                                                              GDBusCXX::SignalFilter::SIGNAL_FILTER_PATH_PREFIX)));
            m_propChangedSignal->activate(boost::bind(&PbapSession::propChangedCb, m_self, _1, _2, _3, _4));
        } else {
            // obexd >= 0.47
            m_completeSignal.reset(new CompleteSignal_t
                                   (GDBusCXX::SignalFilter(m_client->getConnection(),
                                                           session,
                                                           OBC_TRANSFER_INTERFACE_NEW,
                                                           "Complete",
                                                           GDBusCXX::SignalFilter::SIGNAL_FILTER_PATH_PREFIX)));
            m_completeSignal->activate(boost::bind(&PbapSession::completeCb, m_self, _1));

            // same for error
            m_errorSignal.reset(new GDBusCXX::SignalWatch3<GDBusCXX::Path_t, std::string, std::string>
                                (GDBusCXX::SignalFilter(m_client->getConnection(),
                                                        session,
                                                        OBC_TRANSFER_INTERFACE_NEW,
                                                        "Error",
                                                        GDBusCXX::SignalFilter::SIGNAL_FILTER_PATH_PREFIX)));
            m_errorSignal->activate(boost::bind(&PbapSession::errorCb, m_self, _1, _2, _3));

            // and property changes
            m_propertyChangedSignal.reset(new PropertyChangedSignal_t(GDBusCXX::SignalFilter(m_client->getConnection(),
                                                                                             session,
                                                                                             OBC_TRANSFER_INTERFACE_NEW,
                                                                                             "PropertyChanged",
                                                                                             GDBusCXX::SignalFilter::SIGNAL_FILTER_PATH_PREFIX)));
            m_propertyChangedSignal->activate(boost::bind(&PbapSession::propertyChangedCb, m_self, _1, _2, _3));
        }
    } else {
        // obexd < 0.47
        m_session.reset(new GDBusCXX::DBusRemoteObject(m_client->getConnection(),
                                                   session,
                                                   OBC_PBAP_INTERFACE,
                                                   OBC_SERVICE,
                                                   true));
    }

    SE_LOG_DEBUG(NULL, "PBAP session created: %s", m_session->getPath());

    // get filter list so that we can continue validating our format specifier
    m_filterFields = GDBusCXX::DBusClientCall1< Properties >(*m_session, "ListFilterFields")();
    SE_LOG_DEBUG(NULL, "supported PBAP filter fields:\n    %s",
                 boost::join(m_filterFields, "\n    ").c_str());

    Properties filter;
    if (negated) {
        // negated, start with everything set
        filter = supportedProperties();
    }

    // validate parameters and update filter
    BOOST_FOREACH (const std::string &prop, keywords) {
        if (prop.empty()) {
            continue;
        }

        Properties::const_iterator entry =
            std::find_if(m_filterFields.begin(),
                         m_filterFields.end(),
                         boost::bind(&boost::iequals<std::string,std::string>, _1, prop, std::locale()));

        if (entry == m_filterFields.end()) {
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
    m_filter5["Format"] = version == "2.1" ? "vcard21" : "vcard30";
    m_filter5["Fields"] = filter;

    SE_LOG_DEBUG(NULL, "PBAP session initialized");
}

boost::shared_ptr<PullAll> PbapSession::startPullAll(PullData pullData)
{
    resetTransfer();

    // Update prepared filter to match pullData.
    std::map<std::string, Bluez5Values> currentFilter = m_filter5;
    std::string &format = boost::get<std::string>(currentFilter["Format"]);
    std::list<std::string> &filter = boost::get< std::list<std::string> >(currentFilter["Fields"]);
    switch (pullData) {
    case PULL_AS_CONFIGURED:
        SE_LOG_DEBUG(NULL, "pull all with configured filter: '%s'",
                     boost::join(filter, " ").c_str());
        break;
    case PULL_WITHOUT_PHOTOS:
        // Remove PHOTO from list or create list with the other properties.
        if (filter.empty()) {
            filter = supportedProperties();
        }
        for (Properties::iterator it = filter.begin();
             it != filter.end();
             ++it) {
            if (*it == "PHOTO") {
                filter.erase(it);
                break;
            }
        }
        SE_LOG_DEBUG(NULL, "pull all without photos: '%s'",
                     boost::join(filter, " ").c_str());
        break;
    }

    if (m_obexAPI == OBEXD_OLD ||
        m_obexAPI == OBEXD_NEW) {
        GDBusCXX::DBusClientCall0(*m_session, "SetFilter")(filter);
        GDBusCXX::DBusClientCall0(*m_session, "SetFormat")(format);
    }

    boost::shared_ptr<PullAll> state(new PullAll);
    state->m_currentContact = 0;
    if (m_obexAPI != OBEXD_OLD) {
        // Beware, this will lead to a "Complete" signal in obexd
        // 0.47. We need to be careful with looking at the right
        // transfer to determine whether PullAll completed.
        state->m_numContacts = GDBusCXX::DBusClientCall1<uint16>(*m_session, "GetSize")();
        SE_LOG_DEBUG(NULL, "Expecting %d contacts.", state->m_numContacts);

        state->m_tmpFile.create();
        SE_LOG_DEBUG(NULL, "Created temporary file for PullAll %s", state->m_tmpFile.filename().c_str());
        GDBusCXX::DBusClientCall1<std::pair<GDBusCXX::DBusObject_t, Params> > pullall(*m_session, "PullAll");
        std::pair<GDBusCXX::DBusObject_t, Params> tuple =
            m_obexAPI == OBEXD_NEW ?
            GDBusCXX::DBusClientCall1<std::pair<GDBusCXX::DBusObject_t, Params> >(*m_session, "PullAll")(state->m_tmpFile.filename()) :
            GDBusCXX::DBusClientCall2<GDBusCXX::DBusObject_t, Params>(*m_session, "PullAll")(state->m_tmpFile.filename(), currentFilter);
        const GDBusCXX::DBusObject_t &transfer = tuple.first;
        const Params &properties = tuple.second;
        m_currentTransfer = transfer;
        SE_LOG_DEBUG(NULL, "pullall transfer path %s, %ld properties", transfer.c_str(), (long)properties.size());
        // Work will be finished incrementally in PullAll::getContact().
        //
        // In the meantime we return IDs by simply enumerating the expected ones.
        // If we don't get as many contacts as expected, we return 404 in getContact()
        // and the Synthesis engine will ignore the ID (src/sysync/binfileimplds.cpp:
        // "Record does not exist any more in database%s -> ignore").
        state->m_tmpFileOffset = 0;
        state->m_session = m_self.lock();
    } else {
        GDBusCXX::DBusClientCall1<std::string> pullall(*m_session, "PullAll");
        state->m_buffer = pullall();
        state->addVCards(0, state->m_buffer);
        state->m_numContacts = state->m_content.size();
    }
    return state;
}

const char *PullAll::addVCards(int startIndex, const pcrecpp::StringPiece &vcards)
{
    pcrecpp::StringPiece vcarddata;
    pcrecpp::StringPiece tmp = vcards;
    int count = startIndex;
    pcrecpp::RE re("[\\r\\n]*(^BEGIN:VCARD.*?^END:VCARD)",
                   pcrecpp::RE_Options().set_dotall(true).set_multiline(true));
    while (re.Consume(&tmp, &vcarddata)) {
        m_content[count] = vcarddata;
        ++count;
    }

    SE_LOG_DEBUG(NULL, "PBAP content parsed: %d entries starting at %d", count - startIndex, startIndex);
    return tmp.data();
}

void PbapSession::checkForError()
{
    Transfers::const_iterator it = m_transfers.find(m_currentTransfer);
    if (it != m_transfers.end()) {
        if (!it->second.m_transferErrorCode.empty()) {
            m_parent.throwError(StringPrintf("%s: %s",
                                             it->second.m_transferErrorCode.c_str(),
                                             it->second.m_transferErrorMsg.c_str()));
        }
    }
}

Timespec PbapSession::transferComplete() const
{
    Timespec res;
    Transfers::const_iterator it = m_transfers.find(m_currentTransfer);
    if (it != m_transfers.end()) {
        res = it->second.m_transferComplete;
    }
    return res;
}

void PbapSession::resetTransfer()
{
    m_transfers.clear();
}

std::string PullAll::getNextID()
{
    std::string id;
    if (m_currentContact < m_numContacts) {
        id = StringPrintf("%d", m_currentContact);
        m_currentContact++;
    }
    return id;
}

bool PullAll::getContact(int contactNumber, pcrecpp::StringPiece &vcard)
{
    SE_LOG_DEBUG(NULL, "get PBAP contact #%d", contactNumber);
    if (contactNumber < 0 ||
        contactNumber >= m_numContacts) {
        SE_LOG_DEBUG(NULL, "invalid contact number");
        return false;
    }

    Content::iterator it;
    while ((it = m_content.find(contactNumber)) == m_content.end() &&
           m_session &&
           (!m_session->transferComplete() ||
            m_tmpFile.moreData())) {
        // Wait? We rely on regular propgress signals to wake us up.
        // obex 0.47 sends them every 64KB, at least in combination
        // with a Samsung Galaxy SIII. This may depend on both obexd
        // and the phone, so better check ourselves and perhaps do it
        // less often - unmap/map can be expensive and invalidates
        // some of the unread data (at least how it is implemented
        // now).
        while (!m_session->transferComplete() && m_tmpFile.moreData() < 128 * 1024) {
            g_main_context_iteration(NULL, true);
        }
        m_session->checkForError();
        if (m_tmpFile.moreData()) {
            // Remap. This shifts all addresses already stored in
            // m_content, so beware and update those.
            pcrecpp::StringPiece oldMem = m_tmpFile.stringPiece();
            m_tmpFile.unmap();
            m_tmpFile.map();
            pcrecpp::StringPiece newMem = m_tmpFile.stringPiece();
            ssize_t delta = newMem.data() - oldMem.data();
            BOOST_FOREACH (Content::value_type &entry, m_content) {
                pcrecpp::StringPiece &vcard = entry.second;
                vcard.set(vcard.data() + delta, vcard.size());
            }

            // File exists and obexd has written into it, so now we
            // can unlink it to avoid leaking it if we crash.
            m_tmpFile.remove();

            // Continue parsing where we stopped before.
            pcrecpp::StringPiece next(newMem.data() + m_tmpFileOffset,
                                      newMem.size() - m_tmpFileOffset);
            const char *end = addVCards(m_content.size(), next);
            int newTmpFileOffset = end - newMem.data();
            SE_LOG_DEBUG(NULL, "PBAP content parsed: %d out of %d (total), %d out of %d (last update)",
                         newTmpFileOffset,
                         newMem.size(),
                         (int)(end - next.data()),
                         next.size());
            m_tmpFileOffset = newTmpFileOffset;
        }
    }

    if (it == m_content.end()) {
        SE_LOG_DEBUG(NULL, "did not get the expected contact #%d, perhaps some contacts were deleted?", contactNumber);
        return false;
    }
    vcard = it->second;
    return true;
}

void PbapSession::shutdown(void)
{
    GDBusCXX::DBusClientCall0 removeSession(*m_client, "RemoveSession");

    // always clear pointer, even if method call fails
    GDBusCXX::DBusObject_t path(m_session->getPath());
    //m_session.reset();
    SE_LOG_DEBUG(NULL, "removed session: %s", path.c_str());

    removeSession(path);

    SE_LOG_DEBUG(NULL, "PBAP session closed");
}

PbapSyncSource::PbapSyncSource(const SyncSourceParams &params) :
    SyncSource(params)
{
    SyncSourceSession::init(m_operations);
    m_operations.m_readNextItem = boost::bind(&PbapSyncSource::readNextItem, this, _1, _2, _3);
    m_operations.m_readItemAsKey = boost::bind(&PbapSyncSource::readItemAsKey,
                                               this, _1, _2);
    m_session = PbapSession::create(*this);
    const char *PBAPSyncMode = getenv("SYNCEVOLUTION_PBAP_SYNC");
    m_PBAPSyncMode = !PBAPSyncMode ? PBAP_SYNC_NORMAL :
        boost::iequals(PBAPSyncMode, "incremental") ? PBAP_SYNC_INCREMENTAL :
        boost::iequals(PBAPSyncMode, "text") ? PBAP_SYNC_TEXT :
        boost::iequals(PBAPSyncMode, "all") ? PBAP_SYNC_NORMAL :
        (throwError(StringPrintf("invalid value for SYNCEVOLUTION_PBAP_SYNC: %s", PBAPSyncMode)), PBAP_SYNC_NORMAL);
    m_isFirstCycle = true;
    m_hadContacts = false;
}

PbapSyncSource::~PbapSyncSource()
{
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
}

void PbapSyncSource::beginSync(const std::string &lastToken, const std::string &resumeToken)
{
    if (!lastToken.empty()) {
        throwError(STATUS_SLOW_SYNC_508, std::string("PBAP cannot do change detection"));
    }
}

std::string PbapSyncSource::endSync(bool success)
{
    m_pullAll.reset();
    // Non-empty so that beginSync() can detect non-slow syncs and ask
    // for one.
    return "1";
}

bool PbapSyncSource::isEmpty()
{
    return false; // We don't know for sure. Doesn't matter, so pretend to not be empty.
}

void PbapSyncSource::close()
{
    m_session->shutdown();
}

PbapSyncSource::Databases PbapSyncSource::getDatabases()
{
    Databases result;

    result.push_back(Database("select database via bluetooth address",
                              "[obex-bt://]<bt-addr>"));
    return result;
}

void PbapSyncSource::enableServerMode()
{
    SE_THROW("PbapSyncSource does not implement server mode.");
}

bool PbapSyncSource::serverModeEnabled() const
{
    return false;
}

std::string PbapSyncSource::getPeerMimeType() const
{
    return "text/vcard";
}

void PbapSyncSource::getSynthesisInfo(SynthesisInfo &info,
                                      XMLConfigFragments &fragments)
{
    // We send vCards in either 2.1 or 3.0. The Synthesis engine
    // handles both when parsing, so we don't have to be exact here.
    info.m_native = "vCard21";
    info.m_fieldlist = "contacts";
    info.m_profile = "\"vCard\", 1";

    // vCard 3.0 is the more sane format for exchanging with the
    // Synthesis peer in a local sync, so use that by default.
    std::string type = "text/vcard";
    SourceType sourceType = getSourceType();
    if (!sourceType.m_format.empty()) {
        type = sourceType.m_format;
    }
    info.m_datatypes = getDataTypeSupport(type, sourceType.m_forceFormat);

    /**
     * Access to data must be done early so that a slow sync can be
     * enforced.
     */
    info.m_earlyStartDataRead = true;
}

// TODO: return IDs based on GetSize(), read only when engine needs data.

sysync::TSyError PbapSyncSource::readNextItem(sysync::ItemID aID,
                                  sysync::sInt32 *aStatus,
                                  bool aFirst)
{
    if (aFirst) {
        m_pullAll = m_session->startPullAll((m_PBAPSyncMode == PBAP_SYNC_TEXT ||
                                             (m_PBAPSyncMode == PBAP_SYNC_INCREMENTAL && m_isFirstCycle)) ? PULL_WITHOUT_PHOTOS :
                                            PULL_AS_CONFIGURED);
    }
    if (!m_pullAll) {
        throwError("logic error: readNextItem without aFirst=true before");
    }
    std::string id = m_pullAll->getNextID();
    if (id.empty()) {
        *aStatus = sysync::ReadNextItem_EOF;
        if (m_PBAPSyncMode == PBAP_SYNC_INCREMENTAL &&
            m_hadContacts &&
            m_isFirstCycle) {
            requestAnotherSync();
            m_isFirstCycle = false;
        }
    } else {
        *aStatus = sysync::ReadNextItem_Unchanged;
        aID->item = StrAlloc(id.c_str());
        aID->parent = NULL;
        m_hadContacts = true;
    }
    return sysync::LOCERR_OK;
}

sysync::TSyError PbapSyncSource::readItemAsKey(sysync::cItemID aID, sysync::KeyH aItemKey)
{
    if (!m_pullAll) {
        throwError("logic error: readItemAsKey() without preceeding readNextItem()");
    }
    pcrecpp::StringPiece vcard;
    if (m_pullAll->getContact(atoi(aID->item), vcard)) {
        return getSynthesisAPI()->setValue(aItemKey, "data", vcard.data(), vcard.size());
    } else {
        return sysync::DB_NotFound;
    }
}

SyncSourceRaw::InsertItemResult PbapSyncSource::insertItemRaw(const std::string &luid, const std::string &item)
{
    throwError("writing via PBAP is not supported");
    return InsertItemResult();
}

void PbapSyncSource::readItemRaw(const std::string &luid, std::string &item)
{
    if (!m_pullAll) {
        throwError("logic error: readItemRaw() without preceeding readNextItem()");
    }
    pcrecpp::StringPiece vcard;
    if (m_pullAll->getContact(atoi(luid.c_str()), vcard)) {
        item.assign(vcard.data(), vcard.size());
    } else {
        throwError(STATUS_NOT_FOUND, string("retrieving item: ") + luid);
    }
}

SE_END_CXX

#endif /* ENABLE_PBAP */

#ifdef ENABLE_MODULES
# include "PbapSyncSourceRegister.cpp"
#endif
