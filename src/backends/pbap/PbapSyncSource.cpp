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

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/tokenizer.hpp>

#include <errno.h>
#include <unistd.h>
#include <stdint.h>

#include <algorithm>
#include <regex>

#include <syncevo/GLibSupport.h> // PBAP backend does not compile without GLib.
#include <syncevo/util.h>
#include <syncevo/BoostHelper.h>
#include <src/syncevo/SynthesisEngine.h>
#include <syncevo/SuspendFlags.h>

#include "gdbus-cxx-bridge.h"

#include <boost/algorithm/string/predicate.hpp>

#include <synthesis/SDK_util.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

#define OBC_SERVICE "org.openobex.client" // obexd < 0.47
#define OBC_SERVICE_NEW "org.bluez.obex.client" // obexd >= 0.47, including 0.48 (with yet another slight API change!)
#define OBC_SERVICE_NEW5 "org.bluez.obex" // obexd in Bluez 5.0
#define OBC_CLIENT_INTERFACE "org.openobex.Client"
#define OBC_CLIENT_INTERFACE_NEW "org.bluez.obex.Client"
#define OBC_CLIENT_INTERFACE_NEW5 "org.bluez.obex.Client1"
#define OBC_PBAP_INTERFACE "org.openobex.PhonebookAccess"
#define OBC_PBAP_INTERFACE_NEW "org.bluez.obex.PhonebookAccess"
#define OBC_PBAP_INTERFACE_NEW5 "org.bluez.obex.PhonebookAccess1"
#define OBC_TRANSFER_INTERFACE_NEW "org.bluez.obex.Transfer"
#define OBC_TRANSFER_INTERFACE_NEW5 "org.bluez.obex.Transfer1"

typedef std::map<int, StringPiece> Content;
typedef std::list<std::string> ContactQueue;
typedef std::list<std::string> Properties;
typedef boost::variant< std::string, Properties, uint16_t > Bluez5Values;
typedef std::map<std::string, Bluez5Values> Bluez5Filter;
typedef std::map<std::string, boost::variant<std::string> > Params;
typedef std::pair<GDBusCXX::DBusObject_t, Params> Bluez5PullAllResult;

enum PullData
{
    PULL_AS_CONFIGURED,
    PULL_WITHOUT_PHOTOS
};

struct PullParams
{
    /** Which data to pull. */
    PullData m_pullData;

    /**
     * How much time is meant to be used per chunk.
     */
    double m_timePerChunk;

    /**
     * The lambda factor used in exponential smoothing of the max
     * count per transfer to achieve the desired time per chunk.
     * 0 means "use latest observation only", 1 means "keep using
     * initial chunk size".
     */
    double m_timeLambda;

    /** Initial chunk size in number of contacts, without and with photo data. */
    uint16_t m_startMaxCount[2];

    /** Initial chunk offset, again in contacts. */
    uint16_t m_startOffset;

    // cppcheck-suppress memsetClassFloat
    PullParams() { memset(this, 0, sizeof(*this)); m_timePerChunk = m_timeLambda = 0; }
};

/**
 * This class is responsible for a) generating unique IDs for each
 * contact in a certain order (returned one-by-one via getNextID())
 * and b) returning the contact one-by-one in that order
 * (getContact()). This is done to match the way how the sync engine
 * handles items.
 *
 * Although the API of getContact() allows random access, we don't need
 * to support that for syncing.
 *
 * IDs are #0 to #n-1 where n = GetSize() at the time when the session
 * starts.
 *
 * A simple transfer then just does a PullAll() and returns the
 * incoming data one at a time. The downsides are a) if the transfer
 * always gets interrupted in the middle, we never cache contacts at
 * the end and b) the entire data must be stored temporarily, either in RAM
 * or on disk.
 *
 * Transfers have been reported to take half an hour for slow peers and
 * large address books. This is perhaps unusual, but it happens. More common
 * is the second downside.
 *
 * Transferring in chunks addresses both. Here's a potential (and not
 * 100% correct!) algorithm for transferring a complete address book in chunks:
 *
 * uint16 used = GetSize() # not the same as maximum offset!
 * uint16 start = choose_start()
 * uint16 chunksize = choose_chunk_size()
 *
 * uint16 i
 * for (i = start; i < used; i += chunksize) {
 *    PullAll( Offset = i, MaxCount = chunksize)
 * }
 * for (i = 0; i < start; i += chunksize) {
 *    PullAll( Offset = i, MaxCount = min(chunksize, start - 1)
 * }
 *
 * Note that GetSize() is specified as returning the number of entries in
 * the selected phonebook object that are actually used (i.e. indexes that
 * correspond to non-NULL entries). This is relevant if contacts get
 * deleted after starting the session. In that case, the algorithm above
 * will not necessarily read all contacts. Here's an example:
 *         offsets #0 till #99, with contacts #10 till #19 deleted
 *         chunksize = 10
 *         GetSize() = 90
 *
 * => this will request offsets #0 till #89, missing contacts #90 till #99
 *
 * This could be fixed with an additional PullAll, leading to:
 *
 * for (i = start; i < used; i += chunksize) {
 *    PullAll( Offset = i, MaxCount = chunksize)
 * }
 * PullAll(Offset = i) # no MaxCount!
 * for (i = 0; i < start; i += chunksize) {
 *    PullAll( Offset = i, MaxCount = min(chunksize, start - 1)
 * }
 *
 * The additional PullAll() is meant to read all contacts at the end which
 * would not be covered otherwise.
 *
 * Now the other problem: MaxCount means "read chunksize contacts
 * starting at #i". Therefore the algorithm above will end up reading contacts
 * multiple times occasionally. Example:
 *
 *         offsets #0 till #99, with contact #0 deleted
 *         chunksize = 10
 *         GetSize() = 98
 *
 * PullAll(Offset = 0, MaxCount = 10) => returns 10 contacts #1 till #10 (inclusive)
 * PullAll(Offset = 10, MaxCount = 10) => returns 10 contacts #10 till #19
 * => contact #10 appears twice in the result
 *
 * The duplicate cannot be filtered out easily because the UID is not
 * reliable. This could be addressed by keeping a hash of each contact and
 * discarding those who are exact matches for already seen contacts. It's easier
 * to accept the duplicate and remove it during the next sync.
 *
 * When combining these two problems (some contacts read twice, plus
 * the additional PullAll() at the end), we can get more contacts than
 * originally anticipated based on GetSize(). The sync engine will not
 * ask for more contacts than we originally announced. Therefore the
 * current implementation does *not* do the additional PullAll(); this
 * is unlikely to cause any real problems because it should be rare
 * that the number of contacts changes in the short period of time
 * between establishing the session and asking for the size.
 *
 * There are two more aspects that I chose to ignore above: how to
 * implement the choice of start offset and chunk size.
 *
 * Start offset could be random (no persistent state needed) or could
 * continue where the last sync left off. The latter will require a write
 * after each PullAll() (in case of unexpected shutdowns), even if nothing
 * ever changes. Is that acceptable? Probably not. The current implementation
 * chooses randomly by default.
 *
 * The chunk size in bytes depends on the size of the average contact,
 * which is unknown. Make it too small, and we end up generating lots
 * of individual transfers. Make it too large, and we still have
 * chunks that never transfer completely. The current implementation
 * uses self-tuning to achieve a certain desired transfer time per
 * chunk.
 *
 * This algorithm can be tuned by env variables. See the README for
 * details.
 */
class PullAll
{
    PullParams m_pullParams;

    std::string m_buffer; // vCards kept in memory when using old obexd.
    TmpFile m_tmpFile; // Stored in temporary file and mmapped with more recent obexd.

    // Maps contact number to chunks of m_buffer or m_tmpFile.
    Content m_content;
    int m_contentStartIndex;

    uint16_t m_numContacts; // Number of existing contacts, according to GetSize() or after downloading.
    uint16_t m_currentContact; // Numbered starting with zero according to discovery in addVCards.
    std::shared_ptr<PbapSession> m_session; // Only set when there is a transfer ongoing.
    size_t m_tmpFileOffset; // Number of bytes already parsed.
    uint16_t m_transferOffset; // First contact requested as part of current transfer.
    uint16_t m_initialOffset; // First contact request by first transfer.
    uint16_t m_transferMaxCount; // Number of contacts requested as part of current transfer, 0 when not doing chunked transfers.
    uint16_t m_desiredMaxCount; // Number of contacts supposed to be transfered, may be more than m_transferMaxCount when reading at the end of the enumerated contacts.
    Bluez5Filter m_filter;  // Current filter for a Bluez5-like transfer (includes obexd 0.48 case).
    Timespec m_transferStart; // Start time of current transfer.

    // Observed results from the last transfer.
    double m_lastTransferRate;
    double m_lastContactSizeAverage;
    bool m_wasSuspended;

    friend class PbapSession;
    friend class PbapSyncSource;
public:
    PullAll();
    ~PullAll();

    std::string getNextID();
    bool getContact(const char *id, StringPiece &vcard);
    const char *addVCards(int startIndex, const StringPiece &content, bool eof);
};

PullAll::PullAll() :
    m_contentStartIndex(0),
    m_numContacts(0),
    m_currentContact(0),
    m_tmpFileOffset(0),
    m_transferOffset(0),
    m_initialOffset(0),
    m_transferMaxCount(0),
    m_desiredMaxCount(0),
    m_lastTransferRate(0),
    m_lastContactSizeAverage(0),
    m_wasSuspended(false)
{}

PullAll::~PullAll()
{
}

class PbapSession : private boost::noncopyable, public enable_weak_from_this<PbapSession> {
public:
    // Construct via make_weak_shared.
    friend make_weak_shared;

    void initSession(const std::string &address, const std::string &format);

    typedef std::map<std::string, StringPiece> Content;

    std::shared_ptr<PullAll> startPullAll(const PullParams &pullParams);
    void continuePullAll(PullAll &state);
    void checkForError(); // Throws exception if transfer failed.
    Timespec transferComplete() const;
    void resetTransfer();
    void shutdown(void);
    void setFreeze(bool freeze);
    void blockOnFreeze();

private:
    PbapSession(PbapSyncSource &parent);

    PbapSyncSource &m_parent;
    std::unique_ptr<GDBusCXX::DBusRemoteObject> m_client;
    bool m_frozen;
    enum {
        OBEXD_OLD, // obexd < 0.47
        OBEXD_NEW, // obexd == 0.47, file-based transfer
        // OBEXD_048 // obexd == 0.48, file-based transfer without SetFilter and with filter parameter to PullAll()
        BLUEZ5     // obexd in Bluez >= 5.0
    } m_obexAPI;

    Bluez5Filter m_filter5;
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

    std::unique_ptr<GDBusCXX::SignalWatch<GDBusCXX::Path_t, std::string, std::string> >
        m_errorSignal;

    // Bluez 5
    typedef GDBusCXX::SignalWatch<GDBusCXX::Path_t, std::string, Params, std::vector<std::string> > PropChangedSignal_t;
    std::unique_ptr<PropChangedSignal_t> m_propChangedSignal;
    void propChangedCb(const GDBusCXX::Path_t &path,
                       const std::string &interface,
                       const Params &changed,
                       const std::vector<std::string> &invalidated);

    // new obexd API
    typedef GDBusCXX::SignalWatch<GDBusCXX::Path_t> CompleteSignal_t;
    std::unique_ptr<CompleteSignal_t> m_completeSignal;
    typedef GDBusCXX::SignalWatch<GDBusCXX::Path_t, std::string, boost::variant<int64_t> > PropertyChangedSignal_t;
    std::unique_ptr<PropertyChangedSignal_t> m_propertyChangedSignal;
    void propertyChangedCb(const GDBusCXX::Path_t &path, const std::string &name, const boost::variant<int64_t> &value);

    std::unique_ptr<GDBusCXX::DBusRemoteObject> m_session;
};

PbapSession::PbapSession(PbapSyncSource &parent) :
    m_parent(parent),
    m_frozen(false)
{
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
        if (status == "complete" || status == "error") {
            Completion completion = Completion::now();
            if (status == "error") {
                // We have to make up some error descriptions. The Bluez
                // 5 API no longer seems to provide that.
                completion.m_transferErrorCode = "transfer failed";
                completion.m_transferErrorMsg = "reason unknown";
            }
            m_transfers[path] = completion;
        } else if (status == "active" && m_currentTransfer == path && m_frozen) {
            // Retry Suspend() which must have failed earlier.
            try {
                GDBusCXX::DBusRemoteObject transfer(m_client->getConnection(),
                                                    m_currentTransfer,
                                                    OBC_TRANSFER_INTERFACE_NEW5,
                                                    OBC_SERVICE_NEW5,
                                                    true);
                GDBusCXX::DBusClientCall<>(transfer, "Suspend")();
                SE_LOG_DEBUG(NULL, "successfully suspended transfer when it became active");
            } catch (...) {
                // Ignore all errors here. The worst that can happen is that
                // the transfer continues to run. Once Bluez supports suspending
                // queued transfers we shouldn't get here at all.
                std::string explanation;
                Exception::handle(explanation, HANDLE_EXCEPTION_NO_ERROR);
                SE_LOG_DEBUG(NULL, "ignoring failure of delayed suspend: %s", explanation.c_str());
            }
        }
    }
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

Properties PbapSession::supportedProperties() const
{
    Properties props;
    static const std::set<std::string> supported = {
        "VERSION",
        "FN",
        "N",
        "PHOTO",
        "BDAY",
        "ADR",
        "LABEL",
        "TEL",
        "EMAIL",
        "MAILER",
        "TZ",
        "GEO",
        "TITLE",
        "ROLE",
        "LOGO",
        "AGENT",
        "ORG",
        "NOTE",
        "REV",
        "SOUND",
        "URL",
        "UID",
        "KEY",
        "NICKNAME",
        "CATEGORIES",
        "CLASS"
    };

    for (const std::string &prop: m_filterFields) {
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

    const static std::regex re(R"del((?:(2\.1|3\.0):?)?(\^?)([-a-zA-Z,]*))del");
    std::smatch match;
    if (!std::regex_match(format, match, re)) {
        m_parent.throwError(SE_HERE, StringPrintf("invalid specification of PBAP vCard format (databaseFormat): %s",
                                         format.c_str()));
    }
    std::string version = match[1];
    std::string tmp = match[2];
    std::string properties = match[3];
    char negated = tmp.c_str()[0];
    if (version.empty()) {
        // same default as in obexd
        version = "2.1";
    }
    if (version != "2.1" && version != "3.0") {
        m_parent.throwError(SE_HERE, StringPrintf("invalid vCard version prefix in PBAP vCard format specification (databaseFormat): %s",
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
            GDBusCXX::DBusClientCall<GDBusCXX::DBusObject_t>(*m_client, "CreateSession")(address, params);
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
                GDBusCXX::DBusClientCall<GDBusCXX::DBusObject_t>(*m_client, "CreateSession")(address, params);
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
        session = GDBusCXX::DBusClientCall<GDBusCXX::DBusObject_t>(*m_client, "CreateSession")(params);
    }

    if (session.empty()) {
        m_parent.throwError(SE_HERE, "PBAP: failed to create session");
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
        // calls.
        if (m_obexAPI == BLUEZ5) {
            // Bluez 5
            m_propChangedSignal.reset(new PropChangedSignal_t
                                      (GDBusCXX::SignalFilter(m_client->getConnection(),
                                                              session,
                                                              "org.freedesktop.DBus.Properties",
                                                              "PropertiesChanged",
                                                              GDBusCXX::SignalFilter::SIGNAL_FILTER_PATH_PREFIX)));
            m_propChangedSignal->activate([self=weak_from_this()] (const GDBusCXX::Path_t &path, const std::string &interface, const Params &changed, const std::vector<std::string> &invalidated) {
                    auto lock = self.lock();
                    if (lock) {
                        lock->propChangedCb(path, interface, changed, invalidated);
                    }
                });
        } else {
            // obexd >= 0.47
            m_completeSignal.reset(new CompleteSignal_t
                                   (GDBusCXX::SignalFilter(m_client->getConnection(),
                                                           session,
                                                           OBC_TRANSFER_INTERFACE_NEW,
                                                           "Complete",
                                                           GDBusCXX::SignalFilter::SIGNAL_FILTER_PATH_PREFIX)));
            m_completeSignal->activate([self=weak_from_this()] (const GDBusCXX::Path_t &path) {
                    auto lock = self.lock();
                    SE_LOG_DEBUG(NULL, "obexd transfer %s completed", path.c_str());
                    if (lock) {
                        lock->m_transfers[path] = Completion::now();
                    }
                });

            // same for error
            m_errorSignal.reset(new GDBusCXX::SignalWatch<GDBusCXX::Path_t, std::string, std::string>
                                (GDBusCXX::SignalFilter(m_client->getConnection(),
                                                        session,
                                                        OBC_TRANSFER_INTERFACE_NEW,
                                                        "Error",
                                                        GDBusCXX::SignalFilter::SIGNAL_FILTER_PATH_PREFIX)));
            m_errorSignal->activate([self=weak_from_this()] (const GDBusCXX::Path_t &path, const std::string &error, const std::string &msg) {
                    auto lock = self.lock();
                    SE_LOG_DEBUG(NULL, "obexd transfer %s failed: %s %s",
                                 path.c_str(), error.c_str(), msg.c_str());
                    if (lock) {
                        Completion &completion = lock->m_transfers[path];
                        completion.m_transferComplete = Timespec::monotonic();
                        completion.m_transferErrorCode = error;
                        completion.m_transferErrorMsg = msg;
                    }
                });

            // and property changes
            m_propertyChangedSignal.reset(new PropertyChangedSignal_t(GDBusCXX::SignalFilter(m_client->getConnection(),
                                                                                             session,
                                                                                             OBC_TRANSFER_INTERFACE_NEW,
                                                                                             "PropertyChanged",
                                                                                             GDBusCXX::SignalFilter::SIGNAL_FILTER_PATH_PREFIX)));
            m_propertyChangedSignal->activate([self=weak_from_this()] (const GDBusCXX::Path_t &path, const std::string &interface , const boost::variant<int64_t> &value) {
                    auto lock = self.lock();
                    if (lock) {
                        lock->propertyChangedCb(path, interface, value);
                    }
                });
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
    m_filterFields = GDBusCXX::DBusClientCall< Properties >(*m_session, "ListFilterFields")();
    SE_LOG_DEBUG(NULL, "supported PBAP filter fields:\n    %s",
                 boost::join(m_filterFields, "\n    ").c_str());

    Properties filter;
    if (negated) {
        // negated, start with everything set
        filter = supportedProperties();
    }

    // validate parameters and update filter
    for (const std::string &prop: keywords) {
        if (prop.empty()) {
            continue;
        }

        Properties::const_iterator entry =
            std::find_if(m_filterFields.begin(),
                         m_filterFields.end(),
                         [&prop] (const std::string &other) { return boost::iequals(other, prop, std::locale()); });

        if (entry == m_filterFields.end()) {
            m_parent.throwError(SE_HERE, StringPrintf("invalid property name in PBAP vCard format specification (databaseFormat): %s",
                                             prop.c_str()));
        }

        if (negated) {
            filter.remove(*entry);
        } else {
            filter.push_back(*entry);
        }
    }

    GDBusCXX::DBusClientCall<>(*m_session, "Select")(std::string("int"), std::string("PB"));
    m_filter5["Format"] = version == "2.1" ? "vcard21" : "vcard30";
    m_filter5["Fields"] = filter;

    SE_LOG_DEBUG(NULL, "PBAP session initialized");
}

std::shared_ptr<PullAll> PbapSession::startPullAll(const PullParams &pullParams)
{
    resetTransfer();
    blockOnFreeze();

    // Update prepared filter to match pullData.
    Bluez5Filter currentFilter = m_filter5;
    std::string &format = boost::get<std::string>(currentFilter["Format"]);
    std::list<std::string> &filter = boost::get< std::list<std::string> >(currentFilter["Fields"]);
    switch (pullParams.m_pullData) {
    case PULL_AS_CONFIGURED:
        // Avoid empty filter. Android 4.3 on Samsung Galaxy S3
        // only returns the mandatory FN, N, TEL fields when no
        // filter is set.
        const char *filterSource;
        if (filter.empty()) {
            filterSource = "default properties";
            filter = supportedProperties();
        } else {
            filterSource = "configured";
        }
        SE_LOG_DEBUG(NULL, "pull all with %s filter: '%s'",
                     filterSource,
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

    bool pullAllWithFiltersFallback = false;
    if (m_obexAPI == OBEXD_OLD ||
        m_obexAPI == OBEXD_NEW) {
        try {
            GDBusCXX::DBusClientCall<>(*m_session, "SetFilter")(filter);
            GDBusCXX::DBusClientCall<>(*m_session, "SetFormat")(format);
        } catch (...) {
            // Ignore failure, can happen with 0.48. Instead send filter together
            // with PullAll method call.
            Exception::handle(HANDLE_EXCEPTION_NO_ERROR);
            pullAllWithFiltersFallback = true;
        }
    }

    auto state = std::make_shared<PullAll>();
    state->m_pullParams = pullParams;
    state->m_contentStartIndex = 0;
    state->m_currentContact = 0;
    state->m_transferOffset = 0;
    state->m_desiredMaxCount = 0;
    state->m_initialOffset = 0;
    state->m_transferMaxCount = 0;
    state->m_lastTransferRate = 0;
    state->m_lastContactSizeAverage = 0;
    state->m_wasSuspended = false;
    if (m_obexAPI != OBEXD_OLD) {
        // Beware, this will lead to a "Complete" signal in obexd
        // 0.47. We need to be careful with looking at the right
        // transfer to determine whether PullAll completed.
        state->m_numContacts = GDBusCXX::DBusClientCall<uint16_t>(*m_session, "GetSize")();
        SE_LOG_DEBUG(NULL, "Expecting %d contacts.", state->m_numContacts);

        state->m_tmpFile.create(TmpFile::FILE);
        SE_LOG_DEBUG(NULL, "Created temporary file for PullAll %s", state->m_tmpFile.filename().c_str());

        // Start chunk size depends on whether we pull PHOTOs.
        bool pullPhotos = std::find(filter.begin(), filter.end(), "PHOTO") != filter.end();
        state->m_transferMaxCount = pullParams.m_startMaxCount[pullPhotos];
        if (state->m_transferMaxCount > 0 &&
            (pullAllWithFiltersFallback || m_obexAPI == BLUEZ5)) {
            // Enable transfering in chunks.
            state->m_desiredMaxCount = state->m_transferMaxCount;

            state->m_initialOffset =
                state->m_transferOffset = pullParams.m_startOffset < 0 ?
                0 :
                (pullParams.m_startOffset % state->m_numContacts);
            uint16_t available = state->m_numContacts - state->m_transferOffset;
            if (available < state->m_transferMaxCount) {
                // Don't read past end of contacts.
                state->m_transferMaxCount = available;
            }
            currentFilter["Offset"] = state->m_transferOffset;
            currentFilter["MaxCount"] = state->m_transferMaxCount;
            state->m_filter = currentFilter;
        }

        state->m_transferStart.resetMonotonic();
        Bluez5PullAllResult tuple =
            pullAllWithFiltersFallback ?
            // 0.48
            GDBusCXX::DBusClientCall<std::pair<GDBusCXX::DBusObject_t, Params> >(*m_session, "PullAll")(state->m_tmpFile.filename(), currentFilter) :
            m_obexAPI == OBEXD_NEW ?
            // 0.47
            GDBusCXX::DBusClientCall<std::pair<GDBusCXX::DBusObject_t, Params> >(*m_session, "PullAll")(state->m_tmpFile.filename()) :
            // 5.x
            GDBusCXX::DBusClientCall<GDBusCXX::DBusObject_t, Params>(*m_session, "PullAll")(state->m_tmpFile.filename(), currentFilter);
        const GDBusCXX::DBusObject_t &transfer = tuple.first;
        const Params &properties = tuple.second;
        m_currentTransfer = transfer;
        SE_LOG_DEBUG(NULL, "start pullall offset #%u count %u, transfer path %s, %ld properties",
                     state->m_transferOffset,
                     state->m_transferMaxCount,
                     transfer.c_str(),
                     (long)properties.size());
        // Work will be finished incrementally in PullAll::getContact().
        //
        // In the meantime we return IDs by simply enumerating the expected ones.
        // If we don't get as many contacts as expected, we return 404 in getContact()
        // and the Synthesis engine will ignore the ID (src/sysync/binfileimplds.cpp:
        // "Record does not exist any more in database%s -> ignore").
        state->m_tmpFileOffset = 0;
        state->m_session = shared_from_this();
        state->m_filter = currentFilter;
    } else {
        // < 0.47
        //
        // This only works once. Incremental syncing with the same
        // session leads to a "PullAll method with no arguments not
        // found" error from obex-client. Looks like a bug/limitation
        // of obex-client < 0.47. Not sure what we should do about
        // this: disable incremental sync for old obex-client?  Reject
        // it?  Catch the error and add a better exlanation?
        GDBusCXX::DBusClientCall<std::string> pullall(*m_session, "PullAll");
        state->m_buffer = pullall();
        state->addVCards(0, state->m_buffer, true);
        state->m_numContacts = state->m_content.size();
    }
    return state;
}

const char *findLine(const StringPiece &hay, const StringPiece &needle, bool eof)
{
    const char *current = hay.begin();
    const char *end = hay.end();
    size_t size = needle.size();
    while (current < end) {
        // Skip line break(s).
        while (current < end && (*current == '\n' || *current == '\r')) {
            current++;
        }
        const char *next = current + size;
        if (next <= end &&
            !memcmp(current, needle.begin(), size) &&
            ((eof && next == end) ||
             (next + 1 < end && (*next == '\n' || *next == '\r')))) {
            // Found a matching line.
            return current;
        }
        // Skip line.
        while (current < end && *current != '\n' && *current != '\r') {
            current++;
        }
    }
    return nullptr;
}

const char *PullAll::addVCards(int startIndex, const StringPiece &vcards, bool eof)
{
    const char *current = vcards.begin();
    const char *end = vcards.end();
    const static StringPiece BEGIN_VCARD("BEGIN:VCARD");
    const static StringPiece END_VCARD("END:VCARD");
    int count = startIndex;
    while (true) {
        StringPiece remaining(current, end - current);
        const char *begin_vcard = findLine(remaining, BEGIN_VCARD, eof);
        if (begin_vcard) {
            const char *end_vcard = findLine(StringPiece(remaining), END_VCARD, eof);
            if (end_vcard) {
                const char *next = end_vcard + END_VCARD.size();
                StringPiece vcarddata(begin_vcard, next - begin_vcard);
                m_content[count] = vcarddata;
                ++count;
                current = next;
                continue;
            }
        }
        // No further vcard found, try again when we have more data.
        break;
    }
    SE_LOG_DEBUG(NULL, "PBAP content parsed: %d contacts starting at ID %d", count - startIndex, startIndex);
    return current;
}

void PbapSession::continuePullAll(PullAll &state)
{
    m_transfers.clear();
    state.m_transferStart.resetMonotonic();
    blockOnFreeze();

    Bluez5PullAllResult tuple =
        m_obexAPI == BLUEZ5 ?
        GDBusCXX::DBusClientCall<GDBusCXX::DBusObject_t, Params>(*m_session, "PullAll")(state.m_tmpFile.filename(), state.m_filter) :
        // must be 0.48
        GDBusCXX::DBusClientCall<std::pair<GDBusCXX::DBusObject_t, Params> >(*m_session, "PullAll")(state.m_tmpFile.filename(), state.m_filter);

    const GDBusCXX::DBusObject_t &transfer = tuple.first;
    const Params &properties = tuple.second;
    m_currentTransfer = transfer;
    SE_LOG_DEBUG(NULL, "continue pullall offset #%u count %u, transfer path %s, %ld properties",
                 state.m_transferOffset,
                 state.m_transferMaxCount,
                 transfer.c_str(),
                 (long)properties.size());
}

void PbapSession::checkForError()
{
    Transfers::const_iterator it = m_transfers.find(m_currentTransfer);
    if (it != m_transfers.end()) {
        if (!it->second.m_transferErrorCode.empty()) {
            m_parent.throwError(SE_HERE, StringPrintf("%s: %s",
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

bool PullAll::getContact(const char *id, StringPiece &vcard)
{
    int contactNumber = atoi(id);
    SE_LOG_DEBUG(NULL, "get PBAP contact ID %s", id);
    if (contactNumber < 0 ||
        contactNumber >= m_numContacts) {
        SE_LOG_DEBUG(NULL, "invalid contact number");
        return false;
    }

    Content::iterator it;
    SuspendFlags &s = SuspendFlags::getSuspendFlags();
    while ((it = m_content.find(contactNumber)) == m_content.end() &&
           m_session &&
           (!m_session->transferComplete() ||
            m_tmpFile.moreData() ||
            m_transferMaxCount)) {
        // Wait? We rely on regular propgress signals to wake us up.
        // obex 0.47 sends them every 64KB, at least in combination
        // with a Samsung Galaxy SIII. This may depend on both obexd
        // and the phone, so better check ourselves and perhaps do it
        // less often - unmap/map can be expensive and invalidates
        // some of the unread data (at least how it is implemented
        // now).
        while (!m_session->transferComplete() && m_tmpFile.moreData() < 128 * 1024) {
            s.checkForNormal();
            g_main_context_iteration(NULL, true);
        }
        m_session->checkForError();

        Timespec completed = m_session->transferComplete();
        if (m_tmpFile.moreData()) {
            // Remap. This shifts all addresses already stored in
            // m_content, so beware and update those.
            StringPiece oldMem = m_tmpFile.stringPiece();
            m_tmpFile.unmap();
            m_tmpFile.map();
            StringPiece newMem = m_tmpFile.stringPiece();
            ssize_t delta = newMem.data() - oldMem.data();
            for (auto &entry: m_content) {
                StringPiece &vcard = entry.second;
                vcard.set(vcard.data() + delta, vcard.size());
            }

            // File exists and obexd has written into it, so now we
            // can unlink it to avoid leaking it if we crash.
            m_tmpFile.remove();

            // Continue parsing where we stopped before.
            StringPiece next(newMem.data() + m_tmpFileOffset,
                             newMem.size() - m_tmpFileOffset);
            const char *end = addVCards(m_contentStartIndex + m_content.size(), next, completed);
            size_t newTmpFileOffset = end - newMem.data();
            SE_LOG_DEBUG(NULL, "PBAP content parsed: %ld out of %ld (total), %d out of %ld (last update)",
                         (long)newTmpFileOffset,
                         (long)newMem.size(),
                         (int)(end - next.data()),
                         (long)next.size());
            m_tmpFileOffset = newTmpFileOffset;

            if (completed) {
                double duration = (completed - m_transferStart).duration();
                m_lastTransferRate = duration > 0 ? m_tmpFile.size() / duration : 0;
                m_lastContactSizeAverage = m_content.size() ? (double)m_tmpFile.size() / (double)m_content.size() : 0;

                SE_LOG_DEBUG(NULL, "transferred %ldKB and %ld contacts in %.1fs -> transfer rate %.1fKB/s and %.1fcontacts/s, average contact size %.0fB",
                             (long)m_tmpFile.size() / 1024,
                             (long)m_content.size(),
                             duration,
                             m_lastTransferRate / 1024,
                             m_content.size() / duration,
                             m_lastContactSizeAverage);
            }
        } else if (completed && m_transferMaxCount > 0) {
            // Tune m_desiredMaxCount to achieve the intended transfer
            // time. Ignore clipped or suspended transfers, they are
            // not representative. Also avoid completely bogus
            // observations.
            if (m_pullParams.m_timePerChunk > 0 &&
                !m_wasSuspended &&
                m_transferMaxCount == m_desiredMaxCount &&
                m_lastTransferRate > 0 &&
                m_lastContactSizeAverage > 0) {
                // Use exponential moving average.
                double count = m_lastTransferRate * m_pullParams.m_timePerChunk / m_lastContactSizeAverage;
                double newcount = m_desiredMaxCount * m_pullParams.m_timeLambda +
                    count * (1 - m_pullParams.m_timeLambda);
                uint16_t nextcount = (newcount < 0 || newcount > 0xFFFF) ? 0xFFFF : (uint16_t)newcount;
                SE_LOG_DEBUG(NULL, "old max count %u, measured max count for last transfer %.1f, lambda %f, next max count %u",
                             m_desiredMaxCount, count, m_pullParams.m_timeLambda, nextcount);
                m_desiredMaxCount = nextcount;
            }
            m_wasSuspended = false;
            if (m_transferOffset + m_transferMaxCount < m_numContacts) {
                // Move one chunk forward.
                m_transferOffset += m_transferMaxCount;
                m_transferMaxCount = std::min((uint16_t)(((m_transferOffset < m_initialOffset) ? m_initialOffset : m_numContacts) - m_transferOffset),
                                              m_desiredMaxCount);
            } else {
                // Wrap around to offset #0.
                m_transferOffset = 0;
                m_transferMaxCount = std::min(m_initialOffset, m_desiredMaxCount);
            }

            if (m_transferMaxCount > 0) {
                m_filter["Offset"] = m_transferOffset;
                m_filter["MaxCount"] = m_transferMaxCount;

                m_tmpFileOffset = 0;
                m_tmpFile.close();
                m_tmpFile.unmap();
                m_tmpFile.create(TmpFile::FILE);
                SE_LOG_DEBUG(NULL, "Created next temporary file for PullAll %s", m_tmpFile.filename().c_str());
                m_contentStartIndex += m_content.size();
                m_content.clear();
                m_session->continuePullAll(*this);
            }
        }
    }

    if (it == m_content.end()) {
        SE_LOG_DEBUG(NULL, "did not get the expected contact #%d, perhaps some contacts were deleted?",
                     contactNumber);
        return false;
    }

    vcard = it->second;

    return true;
}

void PbapSession::shutdown(void)
{
    GDBusCXX::DBusClientCall<> removeSession(*m_client, "RemoveSession");

    // always clear pointer, even if method call fails
    GDBusCXX::DBusObject_t path(m_session->getPath());
    //m_session.reset();
    SE_LOG_DEBUG(NULL, "removed session: %s", path.c_str());

    removeSession(path);

    SE_LOG_DEBUG(NULL, "PBAP session closed");
}

void PbapSession::setFreeze(bool freeze)
{
    SE_LOG_DEBUG(NULL, "PbapSession::setFreeze(%s, %s)",
                 m_currentTransfer.c_str(),
                 freeze ? "freeze" : "thaw");
    if (freeze == m_frozen) {
        SE_LOG_DEBUG(NULL, "no change in freeze state");
        return;
    }
    if (m_client.get()) {
        if (m_obexAPI == OBEXD_OLD) {
            SE_THROW("freezing OBEX transfer not possible with old obexd");
        }
        if (!m_currentTransfer.empty()) {
            // Suspend/Resume implemented since Bluez 5.15. If not
            // implemented, we will get a D-Bus exception that is returned
            // to the caller as error, which will abort the sync.
            GDBusCXX::DBusRemoteObject transfer(m_client->getConnection(),
                                                m_currentTransfer,
                                                OBC_TRANSFER_INTERFACE_NEW5,
                                                OBC_SERVICE_NEW5,
                                                true);
            try {
                if (freeze) {
                    GDBusCXX::DBusClientCall<>(transfer, "Suspend")();
                } else {
                    GDBusCXX::DBusClientCall<>(transfer, "Resume")();
                }
            } catch (...) {
                std::string explanation;
                Exception::handle(explanation, HANDLE_EXCEPTION_NO_ERROR);

                if (m_currentTransfer.empty() || transferComplete()) {
                    // Transfer already finished. This causes obexd to report
                    // "GDBus.Error:org.freedesktop.DBus.Error.UnknownObject: Method "xxx" with signature "" on interface "org.bluez.obex.Transfer1" doesn't exist."
                    //
                    // We can ignore any error for suspend/resume when
                    // there is no active transfer. The sync engine will
                    // handle suspending/resuming the processing of the data.
                    SE_LOG_DEBUG(NULL, "ignore error after transfer completed: %s", explanation.c_str());
                } else if (freeze && explanation.find("org.bluez.obex.Error.NotInProgress") != explanation.npos) {
                    // Suspending failed because the transfer had not
                    // started yet (still queuing), see
                    // "org.bluez.obex.Transfer1 Suspend/Resume in
                    // queued state" on linux-bluetooth.
                    // Ignore this and retry the Suspend when the transfer
                    // becomes active.
                    SE_LOG_DEBUG(NULL, "must retry Suspend(), got error at the moment: %s", explanation.c_str());
                } else {
                    // Have to abort.
                    GDBusCXX::DBusClientCall<>(transfer, "Cancel")();

                    // Bluez does not change the transfer status when cancelling it,
                    // so our propChangedCb() doesn't get called. We need to record
                    // the end of the transfer directly to stop the syncing.
                    Completion completion = Completion::now();
                    completion.m_transferErrorCode = "cancelled";
                    completion.m_transferErrorMsg = "transfer cancelled because suspending not possible";
                    m_transfers[m_currentTransfer] = completion;

                    Exception::tryRethrow(explanation, true);
                }
            }
        }
    }
    // Handle setFreeze() before and after we have a running
    // transfer by setting a flag and checking that flag before
    // initiating a new transfer.
    m_frozen = freeze;
}

void PbapSession::blockOnFreeze()
{
    SuspendFlags &s = SuspendFlags::getSuspendFlags();
    while (m_frozen) {
        s.checkForNormal();
        g_main_context_iteration(NULL, true);
    }
}

PbapSyncSource::PbapSyncSource(const SyncSourceParams &params) :
    SyncSource(params)
{
    SyncSourceSession::init(m_operations);
    m_operations.m_readNextItem = [this] (sysync::ItemID aID,
                                          sysync::sInt32 *aStatus,
                                          bool aFirst) {
        return readNextItem(aID, aStatus, aFirst);
    };
    m_operations.m_readItemAsKey = [this] (sysync::cItemID aID, sysync::KeyH aItemKey) {
        return readItemAsKey(aID, aItemKey);
    };
    m_session = make_weak_shared::make<PbapSession>(*this);
    const char *PBAPSyncMode = getenv("SYNCEVOLUTION_PBAP_SYNC");
    m_PBAPSyncMode = !PBAPSyncMode ? PBAP_SYNC_INCREMENTAL :
        boost::iequals(PBAPSyncMode, "incremental") ? PBAP_SYNC_INCREMENTAL :
        boost::iequals(PBAPSyncMode, "text") ? PBAP_SYNC_TEXT :
        boost::iequals(PBAPSyncMode, "all") ? PBAP_SYNC_NORMAL :
        (throwError(SE_HERE, StringPrintf("invalid value for SYNCEVOLUTION_PBAP_SYNC: %s", PBAPSyncMode)), PBAP_SYNC_NORMAL);
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
        throwError(SE_HERE, "database should specifiy device address (obex-bt://<bt-addr>)");
    }

    std::string address = database.substr(prefix.size());

    m_session->initSession(address, getDatabaseFormat());
}

void PbapSyncSource::beginSync(const std::string &lastToken, const std::string &resumeToken)
{
    if (!lastToken.empty()) {
        throwError(SE_HERE, STATUS_SLOW_SYNC_508, std::string("PBAP cannot do change detection"));
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

void PbapSyncSource::setFreeze(bool freeze)
{
    if (m_session) {
        m_session->setFreeze(freeze);
    }
    if (m_pullAll) {
        m_pullAll->m_wasSuspended = true;
    }
}


PbapSyncSource::Databases PbapSyncSource::getDatabases()
{
    Databases result;

    result.push_back(Database("select database via bluetooth address",
                              "[obex-bt://]<bt-addr>",
                              false,
                              true));
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
    // Use vCard 3.0 with minimal conversion by default.
    std::string type = "raw/text/vcard";
    SourceType sourceType = getSourceType();
    if (!sourceType.m_format.empty()) {
        type = sourceType.m_format;
    }
    if (type == "raw/text/vcard") {
        // Raw mode.
        info.m_native = "vCard30";
        info.m_fieldlist = "Raw";
        info.m_profile = "";
    } else {
        // Assume that it's something more traditional requiring parsing.
        info.m_native = "vCard21";
        info.m_fieldlist = "contacts";
        info.m_profile = "\"vCard\", 1";
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
        PullParams params;

        params.m_pullData = (m_PBAPSyncMode == PBAP_SYNC_TEXT ||
                             (m_PBAPSyncMode == PBAP_SYNC_INCREMENTAL && m_isFirstCycle)) ?
            PULL_WITHOUT_PHOTOS :
            PULL_AS_CONFIGURED;

        const char *env;
        if ((env = getenv("SYNCEVOLUTION_PBAP_CHUNK_TRANSFER_TIME")) != NULL) {
            params.m_timePerChunk = atof(env);
        } else {
            params.m_timePerChunk = 30;
        }
        static const double LAMBDA_DEF = 0.1;
        if ((env = getenv("SYNCEVOLUTION_PBAP_CHUNK_TIME_LAMBDA")) != NULL) {
            params.m_timeLambda = atof(env);
        } else {
            params.m_timeLambda = LAMBDA_DEF;
        }
        if (params.m_timeLambda < 0 ||
            params.m_timeLambda > 1) {
            params.m_timeLambda = LAMBDA_DEF;
        }
        if ((env = getenv("SYNCEVOLUTION_PBAP_CHUNK_MAX_COUNT_PHOTO")) != NULL) {
            params.m_startMaxCount[true] = atoi(env);
        }
        if ((env = getenv("SYNCEVOLUTION_PBAP_CHUNK_MAX_COUNT_NO_PHOTO")) != NULL) {
            params.m_startMaxCount[false] = atoi(env);
        }
        if ((env = getenv("SYNCEVOLUTION_PBAP_CHUNK_OFFSET")) != NULL) {
            params.m_startOffset = atoi(env);
        } else {
            unsigned int seed = (unsigned int)Timespec::system().seconds();
            // Clip it such that it is >= 0 and < 0x10000.
            params.m_startOffset = rand_r(&seed) % 0x10000;
        }

        m_pullAll = m_session->startPullAll(params);
    }
    if (!m_pullAll) {
        throwError(SE_HERE, "logic error: readNextItem without aFirst=true before");
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
        throwError(SE_HERE, "logic error: readItemAsKey() without preceeding readNextItem()");
    }
    StringPiece vcard;
    if (m_pullAll->getContact(aID->item, vcard)) {
        return getSynthesisAPI()->setValue(aItemKey, "itemdata", vcard.data(), vcard.size());
    } else {
        return sysync::DB_NotFound;
    }
}

SyncSourceRaw::InsertItemResult PbapSyncSource::insertItemRaw(const std::string &luid, const std::string &item)
{
    throwError(SE_HERE, "writing via PBAP is not supported");
    return InsertItemResult();
}

void PbapSyncSource::readItemRaw(const std::string &luid, std::string &item)
{
    if (!m_pullAll) {
        throwError(SE_HERE, "logic error: readItemRaw() without preceeding readNextItem()");
    }
    StringPiece vcard;
    if (m_pullAll->getContact(luid.c_str(), vcard)) {
        item.assign(vcard.data(), vcard.size());
    } else {
        throwError(SE_HERE, STATUS_NOT_FOUND, string("retrieving item: ") + luid);
    }
}

SE_END_CXX

#endif /* ENABLE_PBAP */

#ifdef ENABLE_MODULES
# include "PbapSyncSourceRegister.cpp"
#endif
