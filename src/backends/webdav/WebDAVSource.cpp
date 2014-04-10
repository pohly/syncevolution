/*
 * Copyright (C) 2010 Intel Corporation
 */

#include "WebDAVSource.h"

#include <boost/bind.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/find.hpp>
#include <boost/scoped_ptr.hpp>

#include <syncevo/LogRedirect.h>
#include <syncevo/IdentityProvider.h>

#include <boost/assign.hpp>

#include <stdio.h>
#include <errno.h>

SE_BEGIN_CXX

BoolConfigProperty &WebDAVCredentialsOkay()
{
    static BoolConfigProperty okay("webDAVCredentialsOkay", "credentials were accepted before");
    return okay;
}

#ifdef ENABLE_DAV

/**
 * Retrieve settings from SyncConfig.
 * NULL pointer for config is allowed.
 */
class ContextSettings : public Neon::Settings {
    boost::shared_ptr<SyncConfig> m_context;
    SyncSourceConfig *m_sourceConfig;
    std::string m_url;
    /** do change tracking without relying on CTag */
    bool m_noCTag;
    bool m_googleUpdateHack;
    bool m_googleChildHack;
    bool m_googleAlarmHack;
    // credentials were valid in the past: stored persistently in tracking node
    bool m_credentialsOkay;

public:
    ContextSettings(const boost::shared_ptr<SyncConfig> &context,
                    SyncSourceConfig *sourceConfig) :
        m_context(context),
        m_sourceConfig(sourceConfig),
        m_noCTag(false),
        m_googleUpdateHack(false),
        m_googleChildHack(false),
        m_googleAlarmHack(false),
        m_credentialsOkay(false)
    {
        std::string url;

        // check source config first
        if (m_sourceConfig) {
            url = m_sourceConfig->getDatabaseID();
            if (url.find("%u") != url.npos) {
                std::string username = getUsername();
                boost::replace_all(url, "%u", Neon::URI::escape(username));
            }
        }

        // fall back to sync context
        if (url.empty() && m_context) {
            vector<string> urls = m_context->getSyncURL();

            if (!urls.empty()) {
                url = urls.front();
                if (url.find("%u") != url.npos) {
                    std::string username = getUsername();
                    boost::replace_all(url, "%u", Neon::URI::escape(username));
                }
            }
        }

        // remember result and set flags
        setURL(url);

        // m_credentialsOkay: no corresponding setting when using
        // credentials + URL from source config, in which case we
        // never know that credentials should work (bad for Google,
        // with its temporary authentication errors)
        if (m_context) {
            boost::shared_ptr<FilterConfigNode> node = m_context->getNode(WebDAVCredentialsOkay());
            m_credentialsOkay = WebDAVCredentialsOkay().getPropertyValue(*node);
        }
    }

    void setURL(const std::string &url) { initializeFlags(url); m_url = url; }
    virtual std::string getURL() { return m_url; }

    virtual bool verifySSLHost()
    {
        return !m_context || m_context->getSSLVerifyHost();
    }

    virtual bool verifySSLCertificate()
    {
        return !m_context || m_context->getSSLVerifyServer();
    }

    virtual std::string proxy()
    {
        if (!m_context ||
            !m_context->getUseProxy()) {
            return "";
        } else {
            return m_context->getProxyHost();
        }
    }

    bool noCTag() const { return m_noCTag; }
    virtual bool googleUpdateHack() const { return m_googleUpdateHack; }
    virtual bool googleChildHack() const { return m_googleChildHack; }
    virtual bool googleAlarmHack() const { return m_googleChildHack; }

    virtual int timeoutSeconds() const { return m_context->getRetryDuration(); }
    virtual int retrySeconds() const {
        int seconds = m_context->getRetryInterval();
        if (seconds >= 0) {
            seconds /= (120 / 5); // default: 2min => 5s
        }
        return seconds;
    }

    virtual void getCredentials(const std::string &realm,
                                std::string &username,
                                std::string &password);

    virtual boost::shared_ptr<AuthProvider> getAuthProvider();

    std::string getUsername()
    {
        lookupAuthProvider();
        return m_authProvider->getUsername();
    }

    virtual bool getCredentialsOkay() { return m_credentialsOkay; }
    virtual void setCredentialsOkay(bool okay) {
        if (m_credentialsOkay != okay && m_context) {
            boost::shared_ptr<FilterConfigNode> node = m_context->getNode(WebDAVCredentialsOkay());
            if (!node->isReadOnly()) {
                WebDAVCredentialsOkay().setProperty(*node, okay);
                node->flush();
            }
            m_credentialsOkay = okay;
        }
    }

    virtual int logLevel()
    {
        return m_context ?
            m_context->getLogLevel().get() :
            Logger::instance().getLevel();
    }

private:
    void initializeFlags(const std::string &url);
    boost::shared_ptr<AuthProvider> m_authProvider;
    void lookupAuthProvider();
};


void ContextSettings::getCredentials(const std::string &realm,
                                     std::string &username,
                                     std::string &password)
{
    lookupAuthProvider();
    Credentials creds = m_authProvider->getCredentials();
    username = creds.m_username;
    password = creds.m_password;
}

boost::shared_ptr<AuthProvider> ContextSettings::getAuthProvider()
{
    lookupAuthProvider();
    return m_authProvider;
}

void ContextSettings::lookupAuthProvider()
{
    if (m_authProvider) {
        return;
    }

    UserIdentity identity;
    InitStateString password;

    // prefer source config if anything is set there
    const char *credentialsFrom = "undefined";
    if (m_sourceConfig) {
        identity = m_sourceConfig->getUser();
        password = m_sourceConfig->getPassword();
        credentialsFrom = "source config";
    }

    // fall back to context
    if (m_context && !identity.wasSet() && !password.wasSet()) {
        identity = m_context->getSyncUser();
        password = m_context->getSyncPassword();
        credentialsFrom = "source context";
    }
    SE_LOG_DEBUG(NULL, "using username '%s' from %s for WebDAV, password %s",
                 identity.toString().c_str(),
                 credentialsFrom,
                 password.wasSet() ? "was set" : "not set");

    // lookup actual authentication method instead of assuming username/password
    m_authProvider = AuthProvider::create(identity, password);
}

void ContextSettings::initializeFlags(const std::string &url)
{
    bool googleUpdate = false,
        googleChild = false,
        googleAlarm = false,
        noCTag = false;

    Neon::URI uri = Neon::URI::parse(url);
    typedef boost::split_iterator<string::iterator> string_split_iterator;
    for (string_split_iterator arg =
             boost::make_split_iterator(uri.m_query, boost::first_finder("&", boost::is_iequal()));
         arg != string_split_iterator();
         ++arg) {
        static const std::string keyword = "SyncEvolution=";
        if (boost::istarts_with(*arg, keyword)) {
            std::string params(arg->begin() + keyword.size(), arg->end());
            for (string_split_iterator flag =
                     boost::make_split_iterator(params,
                                                boost::first_finder(",", boost::is_iequal()));
                 flag != string_split_iterator();
                 ++flag) {
                if (boost::iequals(*flag, "UpdateHack")) {
                    googleUpdate = true;
                } else if (boost::iequals(*flag, "ChildHack")) {
                    googleChild = true;
                } else if (boost::iequals(*flag, "AlarmHack")) {
                    googleAlarm = true;
                } else if (boost::iequals(*flag, "Google")) {
                    googleUpdate =
                        googleChild =
                        googleAlarm = true;
                } else if (boost::iequals(*flag, "NoCTag")) {
                    noCTag = true;
                } else {
                    SE_THROW(StringPrintf("unknown SyncEvolution flag %s in URL %s",
                                          std::string(flag->begin(), flag->end()).c_str(),
                                          url.c_str()));
                }
            }
        } else if (arg->end() != arg->begin()) {
            SE_THROW(StringPrintf("unknown parameter %s in URL %s",
                                  std::string(arg->begin(), arg->end()).c_str(),
                                  url.c_str()));
        }
    }

    // store final result
    m_googleUpdateHack = googleUpdate;
    m_googleChildHack = googleChild;
    m_googleAlarmHack = googleAlarm;
    m_noCTag = noCTag;
}


WebDAVSource::WebDAVSource(const SyncSourceParams &params,
                           const boost::shared_ptr<Neon::Settings> &settings) :
    TrackingSyncSource(params),
    m_settings(settings)
{
    if (!m_settings) {
        m_contextSettings.reset(new ContextSettings(params.m_context, this));
        m_settings = m_contextSettings;
    }

    /* insert contactServer() into BackupData_t and RestoreData_t (implemented by SyncSourceRevisions) */
    m_operations.m_backupData = boost::bind(&WebDAVSource::backupData,
                                            this, m_operations.m_backupData, _1, _2, _3);
    m_operations.m_restoreData = boost::bind(&WebDAVSource::restoreData,
                                             this, m_operations.m_restoreData, _1, _2, _3);

    // ignore the "Request ends, status 207 class 2xx, error line:" printed by neon
    LogRedirect::addIgnoreError(", error line:");
    // ignore error messages in returned data
    LogRedirect::addIgnoreError("Read block (");
}

static const std::string UID("\nUID:");

const std::string *WebDAVSource::createResourceName(const std::string &item, std::string &buffer, std::string &luid)
{
    luid = extractUID(item);
    std::string suffix = getSuffix();
    if (luid.empty()) {
        // must modify item
        luid = UUID();
        buffer = item;
        size_t start = buffer.find("\nEND:" + getContent());
        if (start != buffer.npos) {
            start++;
            buffer.insert(start, StringPrintf("UID:%s\r\n", luid.c_str()));
        }
        luid += suffix;
        return &buffer;
    } else {
        luid += suffix;
        return &item;
    }
}

const std::string *WebDAVSource::setResourceName(const std::string &item, std::string &buffer, const std::string &luid)
{
    std::string olduid = luid;
    std::string suffix = getSuffix();
    if (boost::ends_with(olduid, suffix)) {
        olduid.resize(olduid.size() - suffix.size());
    }

    // First check if the item already contains the right UID
    // or at least some UID. If there is a UID, we trust it to be correct,
    // because our guess here (resource name == UID) can be wrong, for
    // example for items created by other clients or by us when using
    // POST and letting the server choose the resource name.
    //
    // This relies on our peer doing the right thing.
    size_t start, end;
    std::string uid = extractUID(item, &start, &end);
    if (uid == olduid || !uid.empty()) {
        return &item;
    }

    // insert or overwrite
    buffer = item;
    if (start != std::string::npos) {
        // overwrite
        buffer.replace(start, end - start, olduid);
    } else {
        // insert
        start = buffer.find("\nEND:" + getContent());
        if (start != buffer.npos) {
            start++;
            buffer.insert(start, StringPrintf("UID:%s\n", olduid.c_str()));
        }
    }
    return &buffer;
}



std::string WebDAVSource::extractUID(const std::string &item, size_t *startp, size_t *endp)
{
    std::string luid;
    if (startp) {
        *startp = std::string::npos;
    }
    if (endp) {
        *endp = std::string::npos;
    }
    // find UID, use that plus ".vcf" as resource name (expected by Yahoo Contacts)
    size_t start = item.find(UID);
    if (start != item.npos) {
        start += UID.size();
        size_t end = item.find("\n", start);
        if (end != item.npos) {
            if (startp) {
                *startp = start;
            }
            luid = item.substr(start, end - start);
            if (boost::ends_with(luid, "\r")) {
                luid.resize(luid.size() - 1);
            }
            // keep checking for more lines because of folding
            while (end + 1 < item.size() &&
                   item[end + 1] == ' ') {
                start = end + 1;
                end = item.find("\n", start);
                if (end == item.npos) {
                    // incomplete, abort
                    luid = "";
                    if (startp) {
                        *startp = std::string::npos;
                    }
                    break;
                }
                luid += item.substr(start, end - start);
                if (boost::ends_with(luid, "\r")) {
                    luid.resize(luid.size() - 1);
                }
            }
            // success, return all information
            if (endp) {
                // don't include \r or \n
                *endp = item[end - 1] == '\r' ?
                    end - 1 :
                    end;
            }
        }
    }
    return luid;
}

std::string WebDAVSource::getSuffix() const
{
    return getContent() == "VCARD" ?
        ".vcf" :
        ".ics";
}

void WebDAVSource::replaceHTMLEntities(std::string &item)
{
    while (true) {
        bool found = false;

        std::string decoded;
        size_t last = 0; // last character copied
        size_t next = 0; // next character to be looked at
        while (true) {
            next = item.find('&', next);
            size_t start = next;
            if (next == item.npos) {
                // finish decoding
                if (found) {
                    decoded.append(item, last, item.size() - last);
                }
                break;
            }
            next++;
            size_t end = next;
            while (end != item.size()) {
                char c = item[end];
                if ((c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') ||
                    (c == '#')) {
                    end++;
                } else {
                    break;
                }
            }
            if (end == item.size() || item[end] != ';') {
                // Invalid character between & and ; or no
                // proper termination? No entity, continue
                // decoding in next loop iteration.
                next = end;
                continue;
            }
            unsigned char c = 0;
            if (next < end) {
                if (item[next] == '#') {
                    // decimal or hexadecimal number
                    next++;
                    if (next < end) {
                        int base;
                        if (item[next] == 'x') {
                            // hex
                            base = 16;
                            next++;
                        } else {
                            base = 10;
                        }
                        while (next < end) {
                            unsigned char v = tolower(item[next]);
                            if (v >= '0' && v <= '9') {
                                next++;
                                c = c * base + (v - '0');
                            } else if (base == 16 && v >= 'a' && v <= 'f') {
                                next++;
                                c = c * base + (v - 'a') + 10;
                            } else {
                                // invalid character, abort scanning of this entity
                                break;
                            }
                        }
                    }
                } else {
                    // check for entities
                    struct {
                        const char *m_name;
                        unsigned char m_character;
                    } entities[] = {
                        // core entries, extend as needed...
                        { "quot", '"' },
                        { "amp", '&' },
                        { "apos", '\'' },
                        { "lt", '<' },
                        { "gt", '>' },
                        { NULL, 0 }
                    };
                    int i = 0;
                    while (true) {
                        const char *name = entities[i].m_name;
                        if (!name) {
                            break;
                        }
                        if (!item.compare(next, end - next, name)) {
                            c = entities[i].m_character;
                            next += strlen(name);
                            break;
                        }
                        i++;
                    }
                }
                if (next == end) {
                    // swallowed all characters in entity, must be valid:
                    // copy all uncopied characters plus the new one
                    found = true;
                    decoded.reserve(item.size());
                    decoded.append(item, last, start - last);
                    decoded.append(1, c);
                    last = end + 1;
                }
            }
            next = end + 1;
        }
        if (found) {
            item = decoded;
        } else {
            break;
        }
    }
}

void WebDAVSource::open()
{
    // Nothing to do here, expensive initialization is in contactServer().
}

static bool setFirstURL(Neon::URI &result,
                        const std::string &name,
                        const Neon::URI &uri)
{
    result = uri;
    // stop
    return false;
}

void WebDAVSource::contactServer()
{
    if (!m_calendar.empty() &&
        m_session) {
        // we have done this work before, no need to repeat it
    }

    SE_LOG_DEBUG(NULL, "using libneon %s with %s",
                 ne_version_string(), Neon::features().c_str());

    // Can we skip auto-detection because a full resource URL is set?
    std::string database = getDatabaseID();
    if (!database.empty() &&
        m_contextSettings) {
        m_calendar = Neon::URI::parse(database, true);
        // m_contextSettings = m_settings, so this sets m_settings->getURL()
        m_contextSettings->setURL(database);
        // start talking to host defined by m_settings->getURL()
        m_session = Neon::Session::create(m_settings);
        // force authentication via username/password or OAuth2
        m_session->forceAuthorization(m_settings->getAuthProvider());
        return;
    }

    // Create session and find first collection (the default).
    m_calendar = Neon::URI();
    findCollections(boost::bind(setFirstURL,
                                boost::ref(m_calendar),
                                _1, _2));
    if (m_calendar.empty()) {
        throwError("no database found");
    }
    SE_LOG_DEBUG(NULL, "picked final path %s", m_calendar.m_path.c_str());

    // Check some server capabilities. Purely informational at this
    // point, doesn't have to succeed either (Google 401 throttling
    // workaround not active here, so it may really fail!).
#ifdef HAVE_LIBNEON_OPTIONS
    if (Logger::instance().getLevel() >= Logger::DEV) {
        try {
            SE_LOG_DEBUG(NULL, "read capabilities of %s", m_calendar.toURL().c_str());
            m_session->startOperation("OPTIONS", Timespec());
            int caps = m_session->options(m_calendar.m_path);
            static const Flag descr[] = {
                { NE_CAP_DAV_CLASS1, "Class 1 WebDAV (RFC 2518)" },
                { NE_CAP_DAV_CLASS2, "Class 2 WebDAV (RFC 2518)" },
                { NE_CAP_DAV_CLASS3, "Class 3 WebDAV (RFC 4918)" },
                { NE_CAP_MODDAV_EXEC, "mod_dav 'executable' property" },
                { NE_CAP_DAV_ACL, "WebDAV ACL (RFC 3744)" },
                { NE_CAP_VER_CONTROL, "DeltaV version-control" },
                { NE_CAP_CO_IN_PLACE, "DeltaV checkout-in-place" },
                { NE_CAP_VER_HISTORY, "DeltaV version-history" },
                { NE_CAP_WORKSPACE, "DeltaV workspace" },
                { NE_CAP_UPDATE, "DeltaV update" },
                { NE_CAP_LABEL, "DeltaV label" },
                { NE_CAP_WORK_RESOURCE, "DeltaV working-resouce" },
                { NE_CAP_MERGE, "DeltaV merge" },
                { NE_CAP_BASELINE, "DeltaV baseline" },
                { NE_CAP_ACTIVITY, "DeltaV activity" },
                { NE_CAP_VC_COLLECTION, "DeltaV version-controlled-collection" },
                { 0, NULL }
            };
            SE_LOG_DEBUG(NULL, "%s WebDAV capabilities: %s",
                         m_session->getURL().c_str(),
                         Flags2String(caps, descr).c_str());
        } catch (const Neon::FatalException &ex) {
            throw;
        } catch (...) {
            Exception::handle();
        }
    }
#endif // HAVE_LIBNEON_OPTIONS
}

bool WebDAVSource::findCollections(const boost::function<bool (const std::string &,
                                                               const Neon::URI &)> &storeResult)
{
    bool res = true; // completed
    int timeoutSeconds = m_settings->timeoutSeconds();
    int retrySeconds = m_settings->retrySeconds();
    SE_LOG_DEBUG(getDisplayName(), "timout %ds, retry %ds => %s",
                 timeoutSeconds, retrySeconds,
                 (timeoutSeconds <= 0 ||
                  retrySeconds <= 0) ? "resending disabled" : "resending allowed");

    boost::shared_ptr<AuthProvider> authProvider = m_contextSettings->getAuthProvider();
    std::string username = authProvider->getUsername();

    // If no URL was configured, then try DNS SRV lookup.
    // syncevo-webdav-lookup and at least one of the tools
    // it depends on (host, nslookup, adnshost, ...) must
    // be in the shell search path.
    //
    // Only our own m_contextSettings allows overriding the
    // URL. Not an issue, in practice it is always used.
    std::string url = m_settings->getURL();
    if (url.empty() && m_contextSettings) {
        size_t pos = username.find('@');
        if (pos == username.npos) {
            // throw authentication error to indicate that the credentials are wrong
            throwError(STATUS_UNAUTHORIZED, StringPrintf("syncURL not configured and username %s does not contain a domain", username.c_str()));
        }
        std::string domain = username.substr(pos + 1);

        FILE *in = NULL;
        try {
            Timespec startTime = Timespec::monotonic();

        retry:
            in = popen(StringPrintf("syncevo-webdav-lookup '%s' '%s'",
                                    serviceType().c_str(),
                                    domain.c_str()).c_str(),
                       "r");
            if (!in) {
                throwError("syncURL not configured and starting syncevo-webdav-lookup for DNS SRV lookup failed", errno);
            }
            // ridicuously long URLs are truncated...
            char buffer[1024];
            size_t read = fread(buffer, 1, sizeof(buffer) - 1, in);
            buffer[read] = 0;
            if (read > 0 && buffer[read - 1] == '\n') {
                read--;
            }
            buffer[read] = 0;
            m_contextSettings->setURL(buffer);
            SE_LOG_DEBUG(getDisplayName(), "found syncURL '%s' via DNS SRV", buffer);
            int res = pclose(in);
            in = NULL;
            switch (res) {
            case 0:
                break;
            case 2:
                throwError(StringPrintf("syncURL not configured and syncevo-webdav-lookup did not find a DNS utility to search for %s in %s", serviceType().c_str(), domain.c_str()));
                break;
            case 3:
                throwError(StringPrintf("syncURL not configured and DNS SRV search for %s in %s did not find the service", serviceType().c_str(), domain.c_str()));
                break;
            default: {
                Timespec now = Timespec::monotonic();
                if (retrySeconds > 0 &&
                    timeoutSeconds > 0) {
                    if (now < startTime + timeoutSeconds) {
                        SE_LOG_DEBUG(getDisplayName(), "DNS SRV search failed due to network issues, retry in %d seconds",
                                     retrySeconds);
                        Sleep(retrySeconds);
                        goto retry;
                    } else {
                        SE_LOG_INFO(getDisplayName(), "DNS SRV search timed out after %d seconds", timeoutSeconds);
                    }
                }

                // probably network problem
                throwError(STATUS_TRANSPORT_FAILURE, StringPrintf("syncURL not configured and DNS SRV search for %s in %s failed", serviceType().c_str(), domain.c_str()));
                break;
            }
            }
        } catch (...) {
            if (in) {
                pclose(in);
            }
            throw;
        }
    }

    // start talking to host defined by m_settings->getURL()
    m_session = Neon::Session::create(m_settings);

    // Find default calendar. Same for address book, with slightly
    // different parameters.
    //
    // Stops when:
    // - current path is calendar collection (= contains VEVENTs)
    // Gives up:
    // - when running in circles
    // - nothing else to try out
    // - tried 10 times
    // Follows:
    // - current-user-principal
    // - CalDAV calendar-home-set
    // - collections
    //
    // TODO: hrefs and redirects are assumed to be on the same host - support switching host
    // TODO: support more than one calendar. Instead of stopping at the first one,
    // scan more throroughly, then decide deterministically.
    int counter = 0;
    const int limit = 1000;
    // Keeps track of paths to look at and those
    // which were already tested.
    class Tried : public std::set<std::string> {
        std::list<std::string> m_candidates;
        bool m_found;
    public:
        Tried() : m_found(false) {}

        /** Was path not tested yet? */
        bool isNew(const std::string &path) {
            return find(Neon::URI::normalizePath(path, true)) == end();
        }

        /** Hand over next candidate to caller, empty if none available. */
        std::string getNextCandidate() {
            if (!m_candidates.empty() ) {
                std::string candidate = m_candidates.front();
                m_candidates.pop_front();
                return candidate;
            } else {
                return "";
            }
        }

        /** remember that path was tested */
        std::string insert(const std::string &path) {
            std::string normal = Neon::URI::normalizePath(path, true);
            std::set<std::string>::insert(normal);
            m_candidates.remove(normal);
            return normal;
        }
        enum Position {
            FRONT,
            BACK
        };
        void addCandidate(const std::string &path, Position position) {
            std::string normal = Neon::URI::normalizePath(path, true);
            if (isNew(normal)) {
                if (position == FRONT) {
                    m_candidates.push_front(normal);
                } else {
                    m_candidates.push_back(normal);
                }
            }
        }

        void foundResult() { m_found = true; }

        /** Nothing left to try and nothing found => bail out with error for last candidate. */
        bool errorIsFatal() { return m_candidates.empty() && !m_found; }
    } tried;
    std::string path = m_session->getURI().m_path;
    Props_t davProps;
    Neon::Session::PropfindPropCallback_t callback =
        boost::bind(&WebDAVSource::openPropCallback,
                    this, boost::ref(davProps), _1, _2, _3, _4);

    // With Yahoo! the initial connection often failed with 50x
    // errors.  Retrying individual requests is error prone because at
    // least one (asking for .well-known/[caldav|carddav]) always
    // results in 502. Let the PROPFIND requests be resent, but in
    // such a way that the overall discovery will never take longer
    // than the total configured timeout period.
    //
    // The PROPFIND with openPropCallback is idempotent, because it
    // will just overwrite previously found information in davProps.
    // Therefore resending is okay.
    Timespec finalDeadline = createDeadline(); // no resending if left empty

    // Add well-known URL as fallback to be tried if configured
    // path was empty. eGroupware also replies with a redirect for the
    // empty path, but relying on that alone is risky because it isn't
    // specified.
    if (path.empty() || path == "/") {
        std::string wellknown = wellKnownURL();
        if (!wellknown.empty()) {
            tried.addCandidate(wellknown, Tried::BACK);
        }
    }

    while (true) {
        bool usernameInserted = false;
        std::string next;

        // Replace %u with the username, if the %u is found. Also, keep track
        // of this event happening, because if we later on get a 404 error,
        // we will convert it to 401 only if the path contains the username
        // and it was indeed us who put the username there (not the server).
        if (boost::find_first(path, "%u")) {
            boost::replace_all(path, "%u", Neon::URI::escape(username));
            usernameInserted = true;
        }

        // must normalize so that we can compare against results from server
        path = tried.insert(path);
        SE_LOG_DEBUG(NULL, "testing %s", path.c_str());

        // Accessing the well-known URIs should lead to a redirect, but
        // with Yahoo! Calendar all I got was a 502 "connection refused".
        // Yahoo! Contacts also doesn't redirect. Instead on ends with
        // a Principal resource - perhaps reading that would lead further.
        //
        // So anyway, let's try the well-known URI first, but also add
        // the root path as fallback.
        if (path == "/.well-known/caldav/" ||
            path == "/.well-known/carddav/") {
            // remove trailing slash added by normalization, to be aligned with draft-daboo-srv-caldav-10
            path.resize(path.size() - 1);

            // Yahoo! Calendar returns no redirect. According to rfc4918 appendix-E,
            // a client may simply try the root path in case of such a failure,
            // which happens to work for Yahoo.
            tried.addCandidate("/", Tried::BACK);
            // TODO: Google Calendar, with workarounds
            // candidates.push_back(StringPrintf("/calendar/dav/%s/user/", Neon::URI::escape(username).c_str()));
        }

        bool success = false;
        try {
            // disable resending for some known cases where it never succeeds
            Timespec deadline = finalDeadline;
            if (boost::starts_with(path, "/.well-known") &&
                m_settings->getURL().find("yahoo.com") != string::npos) {
                deadline = Timespec();
            }

            if (Logger::instance().getLevel() >= Logger::DEV) {
                // First dump WebDAV "allprops" properties (does not contain
                // properties which must be asked for explicitly!). Only
                // relevant for debugging.
                try {
                    SE_LOG_DEBUG(NULL, "debugging: read all WebDAV properties of %s", path.c_str());
                    // Use OAuth2, if available.
                    boost::shared_ptr<AuthProvider> authProvider = m_settings->getAuthProvider();
                    if (authProvider->methodIsSupported(AuthProvider::AUTH_METHOD_OAUTH2)) {
                        m_session->forceAuthorization(authProvider);
                    }
                    Neon::Session::PropfindPropCallback_t callback =
                        boost::bind(&WebDAVSource::openPropCallback,
                                    this, boost::ref(davProps), _1, _2, _3, _4);
                    m_session->propfindProp(path, 0, NULL, callback, Timespec());
                } catch (const Neon::FatalException &ex) {
                    throw;
                } catch (...) {
                    handleException(HANDLE_EXCEPTION_NO_ERROR);
                }
            }

            // Now ask for some specific properties of interest for us.
            // Using CALDAV:allprop would be nice, but doesn't seem to
            // be possible with Neon.
            //
            // The "current-user-principal" is particularly relevant,
            // because it leads us from
            // "/.well-known/[carddav/caldav]" (or whatever that
            // redirected to) to the current user and its
            // "[calendar/addressbook]-home-set".
            //
            // Apple Calendar Server only returns that information if
            // we force authorization to be used. Otherwise it returns
            // <current-user-principal>
            //    <unauthenticated/>
            // </current-user-principal>
            //
            // We send valid credentials here, using Basic authorization,
            // if configured to use credentials instead of something like OAuth2.
            // The rationale is that this cuts down on the number of
            // requests for https while still being secure. For
            // http, our Neon wrapper is smart enough to ignore our request.
            //
            // See also:
            // http://tools.ietf.org/html/rfc4918#appendix-E
            // http://lists.w3.org/Archives/Public/w3c-dist-auth/2005OctDec/0243.html
            // http://thread.gmane.org/gmane.comp.web.webdav.neon.general/717/focus=719
            m_session->forceAuthorization(m_settings->getAuthProvider());
            davProps.clear();
            // Avoid asking for CardDAV properties when only using CalDAV
            // and vice versa, to avoid breaking both when the server is only
            // broken for one of them (like Google, which (temporarily?) sent
            // invalid CardDAV properties).
            static const ne_propname caldav[] = {
                // WebDAV ACL
                { "DAV:", "alternate-URI-set" },
                { "DAV:", "principal-URL" },
                { "DAV:", "current-user-principal" },
                { "DAV:", "group-member-set" },
                { "DAV:", "group-membership" },
                { "DAV:", "displayname" },
                { "DAV:", "resourcetype" },
                // CalDAV
                { "urn:ietf:params:xml:ns:caldav", "calendar-home-set" },
                { "urn:ietf:params:xml:ns:caldav", "calendar-description" },
                { "urn:ietf:params:xml:ns:caldav", "calendar-timezone" },
                { "urn:ietf:params:xml:ns:caldav", "supported-calendar-component-set" },
                { "urn:ietf:params:xml:ns:caldav", "supported-calendar-data" },
                { "urn:ietf:params:xml:ns:caldav", "max-resource-size" },
                { "urn:ietf:params:xml:ns:caldav", "min-date-time" },
                { "urn:ietf:params:xml:ns:caldav", "max-date-time" },
                { "urn:ietf:params:xml:ns:caldav", "max-instances" },
                { "urn:ietf:params:xml:ns:caldav", "max-attendees-per-instance" },
                { NULL, NULL }
            };
            static const ne_propname carddav[] = {
                // WebDAV ACL
                { "DAV:", "alternate-URI-set" },
                { "DAV:", "principal-URL" },
                { "DAV:", "current-user-principal" },
                { "DAV:", "group-member-set" },
                { "DAV:", "group-membership" },
                { "DAV:", "displayname" },
                { "DAV:", "resourcetype" },
                // CardDAV
                { "urn:ietf:params:xml:ns:carddav", "addressbook-home-set" },
                { "urn:ietf:params:xml:ns:carddav", "principal-address" },
                { "urn:ietf:params:xml:ns:carddav", "addressbook-description" },
                { "urn:ietf:params:xml:ns:carddav", "supported-address-data" },
                { "urn:ietf:params:xml:ns:carddav", "max-resource-size" },
                { NULL, NULL }
            };
            SE_LOG_DEBUG(NULL, "read relevant properties of %s", path.c_str());
            m_session->propfindProp(path, 0,
                                    getContent() == "VCARD" ? carddav : caldav,
                                    callback, deadline);
            success = true;
        } catch (const Neon::FatalException &ex) {
            throw;
        } catch (const Neon::RedirectException &ex) {
            // follow to new location
            Neon::URI next = Neon::URI::parse(ex.getLocation(), true);
            Neon::URI old = m_session->getURI();
            // keep old host + scheme + port if not set in next location
            if (next.m_scheme.empty()) {
                next.m_scheme = old.m_scheme;
            }
            if (next.m_host.empty()) {
                next.m_host = old.m_host;
            }
            if (!next.m_port) {
                next.m_port = old.m_port;
            }
            if (next.m_scheme != old.m_scheme ||
                next.m_host != old.m_host ||
                next.m_port != old.m_port) {
                SE_LOG_DEBUG(NULL, "ignore redirection to different server (not implemented): %s",
                             ex.getLocation().c_str());
                if (tried.errorIsFatal()) {
                    throw;
                }
            } else if (tried.isNew(next.m_path)) {
                SE_LOG_DEBUG(NULL, "new candidate from %s -> %s redirect",
                             old.m_path.c_str(),
                             next.m_path.c_str());
                tried.addCandidate(next.m_path, Tried::FRONT);
            } else {
                SE_LOG_DEBUG(NULL, "already known candidate from %s -> %s redirect",
                             old.m_path.c_str(),
                             next.m_path.c_str());
            }
        } catch (const TransportStatusException &ex) {
            SE_LOG_DEBUG(NULL, "TransportStatusException: %s", ex.what());
            if (ex.syncMLStatus() == 404 && boost::find_first(path, username) && usernameInserted) {
                // We're actually looking at an authentication error: the path to the calendar has
                // not been found, so the username was wrong. Let's hijack the error message and
                // code of the exception by throwing a new one.
                string descr = StringPrintf("Path not found: %s. Is the username '%s' correct?",
                                            path.c_str(), username.c_str());
                int code = 401;
                SE_THROW_EXCEPTION_STATUS(TransportStatusException, descr, SyncMLStatus(code));
            } else {
                if (tried.errorIsFatal()) {
                    throw;
                } else {
                    // ignore the error (whatever it was!), try next
                    // candidate; needed to handle 502 "Connection
                    // refused" for /.well-known/caldav/ from Yahoo!
                    // Calendar
                    SE_LOG_DEBUG(NULL, "ignore error for URI candidate: %s", ex.what());
                }
            }
        } catch (const Exception &ex) {
            if (tried.errorIsFatal()) {
                throw;
            } else {
                // ignore the error (whatever it was!), try next
                // candidate; needed to handle 502 "Connection
                // refused" for /.well-known/caldav/ from Yahoo!
                // Calendar
                SE_LOG_DEBUG(NULL, "ignore error for URI candidate: %s", ex.what());
            }
        }

        if (success) {
            Props_t::iterator pathProps = davProps.find(path);
            if (pathProps == davProps.end()) {
                // No reply for requested path? Happens with Yahoo Calendar server,
                // which returns information about "/dav" when asked about "/".
                // Move to that path.
                if (!davProps.empty()) {
                    pathProps = davProps.begin();
                    string newpath = pathProps->first;
                    SE_LOG_DEBUG(NULL, "use properties for '%s' instead of '%s'",
                                 newpath.c_str(), path.c_str());
                    path = newpath;
                }
            }
            StringMap *props = pathProps == davProps.end() ? NULL : &pathProps->second;
            bool isResult = false;
            if (props && typeMatches(*props)) {
                isResult = true;
                StringMap::const_iterator it;

                // TODO: filter out CalDAV collections which do
                // not contain the right components
                // (urn:ietf:params:xml:ns:caldav:supported-calendar-component-set)

                // found something
                tried.foundResult();
                it = props->find("DAV::displayname");
                Neon::URI uri = m_session->getURI();
                uri.m_path = path;
                std::string name;
                if (it != props->end()) {
                    name = it->second;
                }
                SE_LOG_DEBUG(NULL, "found %s = %s",
                             name.c_str(),
                             uri.toURL().c_str());
                res = storeResult(name,
                                  uri);
                if (!res) {
                    // done
                    break;
                }
            }

            // find next path:
            // prefer CardDAV:calendar-home-set or CalDAV:addressbook-home-set
            std::list<std::string> homes;
            if (props) {
                homes = extractHREFs((*props)[homeSetProp()]);
            }
            BOOST_FOREACH(const std::string &home, homes) {
                if (!home.empty() &&
                    tried.isNew(home)) {
                    if (next.empty()) {
                        SE_LOG_DEBUG(NULL, "follow home-set property to %s", home.c_str());
                        next = home;
                    } else {
                        SE_LOG_DEBUG(NULL, "new candidate from home-set property %s", home.c_str());
                        tried.addCandidate(home, Tried::FRONT);
                    }
                }
            }
            // alternatively, follow principal URL
            if (next.empty()) {
                std::string principal;
                if (props) {
                    principal = extractHREF((*props)["DAV::current-user-principal"]);
                }

                // TODO:
                // xmlns:d="DAV:"
                // <d:current-user-principal><d:href>/m8/carddav/principals/__uids__/patrick.ohly@googlemail.com/</d:href></d:current-user-principal>
                if (!principal.empty() &&
                    tried.isNew(principal)) {
                    next = principal;
                    SE_LOG_DEBUG(NULL, "follow current-user-prinicipal to %s", next.c_str());
                }
            }
            // finally, recursively descend into collections,
            // unless we identified it as a result (because those
            // cannot be recursive)
            if (next.empty() && !isResult) {
                std::string type;
                if (props) {
                    type = (*props)["DAV::resourcetype"];
                }
                if (type.find("<DAV:collection></DAV:collection>") != type.npos) {
                    // List members and find new candidates.
                    // Yahoo! Calendar does not return resources contained in /dav/<user>/Calendar/
                    // if <allprops> is used. Properties must be requested explicitly.
                    SE_LOG_DEBUG(NULL, "list items in %s", path.c_str());
                    // See findCollections() for the reason why we are not mixing CalDAV and CardDAV
                    // properties.
                    static const ne_propname caldav[] = {
                        { "DAV:", "displayname" },
                        { "DAV:", "resourcetype" },
                        { "urn:ietf:params:xml:ns:caldav", "calendar-home-set" },
                        { "urn:ietf:params:xml:ns:caldav", "calendar-description" },
                        { "urn:ietf:params:xml:ns:caldav", "calendar-timezone" },
                        { "urn:ietf:params:xml:ns:caldav", "supported-calendar-component-set" },
                        { NULL, NULL }
                    };
                    static const ne_propname carddav[] = {
                        { "DAV:", "displayname" },
                        { "DAV:", "resourcetype" },
                        { "urn:ietf:params:xml:ns:carddav", "addressbook-home-set" },
                        { "urn:ietf:params:xml:ns:carddav", "addressbook-description" },
                        { "urn:ietf:params:xml:ns:carddav", "supported-address-data" },
                        { NULL, NULL }
                    };
                    davProps.clear();
                    m_session->propfindProp(path, 1,
                                            getContent() == "VCARD" ? carddav : caldav,
                                            callback, finalDeadline);
                    std::set<std::string> subs;
                    BOOST_FOREACH(Props_t::value_type &entry, davProps) {
                        const std::string &sub = entry.first;
                        const std::string &subType = entry.second["DAV::resourcetype"];
                        // new candidates are:
                        // - untested
                        // - not already a candidate
                        // - a resource, but not the CalDAV schedule-inbox/outbox
                        // - not shared ("global-addressbook" in Apple Calendar Server),
                        //   because these are unlikely to be the right "default" collection
                        //
                        // Trying to prune away collections here which are not of the
                        // right type *and* cannot contain collections of the right
                        // type (example: Apple Calendar Server "inbox" under
                        // calendar-home-set URL with type "CALDAV:schedule-inbox") requires
                        // knowledge not current provided by derived classes. TODO (?).
                        if (tried.isNew(sub) &&
                            subType.find("<DAV:collection></DAV:collection>") != subType.npos &&
                            subType.find("<urn:ietf:params:xml:ns:caldavschedule-") == subType.npos &&
                            subType.find("<http://calendarserver.org/ns/shared") == subType.npos &&
                            (typeMatches(entry.second) || !ignoreCollection(entry.second))) {
                            subs.insert(sub);
                            SE_LOG_DEBUG(NULL, "new candidate: %s", sub.c_str());
                        } else {
                            SE_LOG_DEBUG(NULL, "skipping: %s", sub.c_str());
                        }
                    }

                    // insert before other candidates, sorted
                    // alphabetically
                    BOOST_REVERSE_FOREACH (const std::string &path, subs) {
                        tried.addCandidate(path, Tried::FRONT);
                    }
                }
            }
        }

        if (next.empty()) {
            // use next untried candidate
            next = tried.getNextCandidate();
            if (next.empty()) {
                // done searching
                break;
            }
            SE_LOG_DEBUG(NULL, "follow candidate %s", next.c_str());
        }

        counter++;
        if (counter > limit) {
            throwError(StringPrintf("giving up search for collection after %d attempts", limit));
        }
        path = next;
    }

    return res;
}

std::string WebDAVSource::extractHREF(const std::string &propval)
{
    // all additional parameters after opening resp. closing tag
    static const std::string hrefStart = "<DAV:href";
    static const std::string hrefEnd = "</DAV:href";
    size_t start = propval.find(hrefStart);
    start = propval.find('>', start);
    if (start != propval.npos) {
        start++;
        size_t end = propval.find(hrefEnd, start);
        if (end != propval.npos) {
            return propval.substr(start, end - start);
        }
    }
    return "";
}

std::list<std::string> WebDAVSource::extractHREFs(const std::string &propval)
{
    std::list<std::string> res;

    // all additional parameters after opening resp. closing tag
    static const std::string hrefStart = "<DAV:href";
    static const std::string hrefEnd = "</DAV:href";
    size_t current = 0;
    while (current < propval.size()) {
        size_t start = propval.find(hrefStart, current);
        start = propval.find('>', start);
        if (start != propval.npos) {
            start++;
            size_t end = propval.find(hrefEnd, start);
            if (end != propval.npos) {
                res.push_back(propval.substr(start, end - start));
                current = end;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    return res;
}

void WebDAVSource::openPropCallback(Props_t &davProps,
                                    const Neon::URI &uri,
                                    const ne_propname *prop,
                                    const char *value,
                                    const ne_status *status)
{
    // TODO: recognize CALDAV:calendar-timezone and use it for local time conversion of events
    std::string name;
    if (prop->nspace) {
        name = prop->nspace;
    }
    name += ":";
    name += prop->name;
    if (value) {
        davProps[uri.m_path][name] = value;
        boost::trim_if(davProps[uri.m_path][name],
                       boost::is_space());
    }
}

bool WebDAVSource::isEmpty()
{
    contactServer();

    // listing all items is relatively efficient, let's use that
    // TODO: use truncated result search
    RevisionMap_t revisions;
    listAllItems(revisions);
    return revisions.empty();
}

void WebDAVSource::close()
{
    m_session.reset();
}

static bool storeCollection(SyncSource::Databases &result,
                            const std::string &name,
                            const Neon::URI &uri)
{
    std::string url = uri.toURL();

    // avoid duplicates
    BOOST_FOREACH(const SyncSource::Database &entry, result) {
        if (entry.m_uri == url) {
            // already found before
            return true;
        }
    }

    result.push_back(SyncSource::Database(name, url));
    return true;
}

WebDAVSource::Databases WebDAVSource::getDatabases()
{
    Databases result;

    // do a scan if some kind of credentials were set
    if (m_contextSettings->getAuthProvider()->wasConfigured()) {
        findCollections(boost::bind(storeCollection,
                                    boost::ref(result),
                                    _1, _2));
        if (!result.empty()) {
            result.front().m_isDefault = true;
        }
    } else {
        result.push_back(Database("select database via absolute URL, set username/password to scan, set syncURL to base URL if server does not support auto-discovery",
                                  "<path>"));
    }
    return result;
}

void WebDAVSource::getSynthesisInfo(SynthesisInfo &info,
                                    XMLConfigFragments &fragments)
{
    TrackingSyncSource::getSynthesisInfo(info, fragments);

    // only CalDAV enforces unique UID
    std::string content = getContent();
    if (content == "VEVENT" || content == "VTODO" || content == "VJOURNAL") {
        info.m_globalIDs = true;
    }
    if (content == "VEVENT") {
        info.m_backendRule = "HAVE-SYNCEVOLUTION-EXDATE-DETACHED";
    }

    // TODO: instead of identifying the peer based on the
    // session URI, use some information gathered about
    // it during open()
    if (m_session) {
        string host = m_session->getURI().m_host;
        if (host.find("google") != host.npos) {
            info.m_backendRule = "GOOGLE";
            fragments.m_remoterules["GOOGLE"] =
                "      <remoterule name='GOOGLE'>\n"
                "          <deviceid>none</deviceid>\n"
                // enable extensions, just in case (not relevant yet for calendar)
                "          <include rule=\"ALL\"/>\n"
                "      </remoterule>";
        } else if (host.find("yahoo") != host.npos) {
            info.m_backendRule = "YAHOO";
            fragments.m_remoterules["YAHOO"] =
                "      <remoterule name='YAHOO'>\n"
                "          <deviceid>none</deviceid>\n"
                // Yahoo! Contacts reacts with a "500 - internal server error"
                // to an empty X-GENDER property. In general, empty properties
                // should never be necessary in CardDAV and CalDAV, because
                // sent items conceptually replace the one on the server, so
                // disable them all.
                "          <noemptyproperties>yes</noemptyproperties>\n"
                // BDAY is ignored if it has the compact 19991231 instead of
                // 1999-12-31, although both are valid.
                "          <include rule='EXTENDED-DATE-FORMAT'/>\n"
                // Yahoo accepts extensions, so send them. However, it
                // doesn't seem to store the X-EVOLUTION-UI-SLOT parameter
                // extensions.
                "          <include rule=\"ALL\"/>\n"
                "      </remoterule>";
        } else {
            // fallback: generic CalDAV/CardDAV, with all properties
            // enabled (for example, X-EVOLUTION-UI-SLOT)
            info.m_backendRule = "WEBDAV";
            fragments.m_remoterules["WEBDAV"] =
                "      <remoterule name='WEBDAV'>\n"
                "          <deviceid>none</deviceid>\n"
                "          <include rule=\"ALL\"/>\n"
                "      </remoterule>";
        }
        SE_LOG_DEBUG(getDisplayName(), "using data conversion rules for '%s'", info.m_backendRule.c_str());
    }
}

void WebDAVSource::storeServerInfos()
{
    if (getDatabaseID().empty()) {
        // user did not select resource, remember the one used for the
        // next sync
        setDatabaseID(m_calendar.toURL());
        getProperties()->flush();
    }
}

void WebDAVSource::checkPostSupport()
{
    if (m_postPath.wasSet()) {
        return;
    }

    static const ne_propname getaddmember[] = {
        { "DAV:", "add-member" },
        { NULL, NULL }
    };
    Timespec deadline = createDeadline();
    Props_t davProps;
    Neon::Session::PropfindPropCallback_t callback =
        boost::bind(&WebDAVSource::openPropCallback,
                    this, boost::ref(davProps), _1, _2, _3, _4);
    SE_LOG_DEBUG(NULL, "check POST support of %s", m_calendar.m_path.c_str());
    m_session->propfindProp(m_calendar.m_path, 0, getaddmember, callback, deadline);
    // Fatal communication problems will be reported via exceptions.
    // Once we get here, invalid or incomplete results can be
    // treated as "don't have revision string".
    m_postPath = extractHREF(davProps[m_calendar.m_path]["DAV::add-member"]);
    SE_LOG_DEBUG(NULL, "%s POST support: %s",
                 m_calendar.m_path.c_str(),
                 m_postPath.empty() ? "<none>" : m_postPath.get().c_str());
}

/**
 * See https://trac.calendarserver.org/browser/CalendarServer/trunk/doc/Extensions/caldav-ctag.txt
 */
static const ne_propname getctag[] = {
    { "http://calendarserver.org/ns/", "getctag" },
    { NULL, NULL }
};

std::string WebDAVSource::databaseRevision()
{
    if (m_contextSettings && m_contextSettings->noCTag()) {
        // return empty string to disable usage of CTag
        return "";
    }

    contactServer();

    Timespec deadline = createDeadline();
    Props_t davProps;
    Neon::Session::PropfindPropCallback_t callback =
        boost::bind(&WebDAVSource::openPropCallback,
                    this, boost::ref(davProps), _1, _2, _3, _4);
    SE_LOG_DEBUG(NULL, "read ctag of %s", m_calendar.m_path.c_str());
    m_session->propfindProp(m_calendar.m_path, 0, getctag, callback, deadline);
    // Fatal communication problems will be reported via exceptions.
    // Once we get here, invalid or incomplete results can be
    // treated as "don't have revision string".
    string ctag = davProps[m_calendar.m_path]["http://calendarserver.org/ns/:getctag"];
    return ctag;
}


static const ne_propname getetag[] = {
    { "DAV:", "getetag" },
    { "DAV:", "resourcetype" },
    { NULL, NULL }
};

void WebDAVSource::listAllItems(RevisionMap_t &revisions)
{
    contactServer();

    if (!getContentMixed()) {
        // Can use simple PROPFIND because we do not have to
        // double-check that each item really contains the right data.
        bool failed = false;
        Timespec deadline = createDeadline();
        m_session->propfindURI(m_calendar.m_path, 1, getetag,
                               boost::bind(&WebDAVSource::listAllItemsCallback,
                                           this, _1, _2, boost::ref(revisions),
                                           boost::ref(failed)),
                               deadline);
        if (failed) {
            SE_THROW("incomplete listing of all items");
        }
    } else {
        // We have to read item data and verify that it really is
        // something we have to (and may) work on. Currently only
        // happens for CalDAV, CardDAV items are uniform. The CalDAV
        // comp-filter alone should the trick, but some servers (for
        // example Radicale 0.7) ignore it and thus we could end up
        // deleting items we were not meant to touch.
        const std::string query =
            "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
            "<C:calendar-query xmlns:D=\"DAV:\"\n"
            "xmlns:C=\"urn:ietf:params:xml:ns:caldav\">\n"
            "<D:prop>\n"
            "<D:getetag/>\n"
            "<C:calendar-data>\n"
            "<C:comp name=\"VCALENDAR\">\n"
            "<C:comp name=\"" + getContent() + "\">\n"
            "<C:prop name=\"UID\"/>\n"
            "</C:comp>\n"
            "</C:comp>\n"
            "</C:calendar-data>\n"
            "</D:prop>\n"
            // filter expected by Yahoo! Calendar
            "<C:filter>\n"
            "<C:comp-filter name=\"VCALENDAR\">\n"
            "<C:comp-filter name=\"" + getContent() + "\">\n"
            "</C:comp-filter>\n"
            "</C:comp-filter>\n"
            "</C:filter>\n"
            "</C:calendar-query>\n";
        Timespec deadline = createDeadline();
        getSession()->startOperation("REPORT 'meta data'", deadline);
        while (true) {
            string data;
            Neon::XMLParser parser;
            parser.initReportParser(boost::bind(&WebDAVSource::checkItem, this,
                                                boost::ref(revisions),
                                                _1, _2, &data));
            parser.pushHandler(boost::bind(Neon::XMLParser::accept, "urn:ietf:params:xml:ns:caldav", "calendar-data", _2, _3),
                               boost::bind(Neon::XMLParser::append, boost::ref(data), _2, _3));
            Neon::Request report(*getSession(), "REPORT", getCalendar().m_path, query, parser);
            report.addHeader("Depth", "1");
            report.addHeader("Content-Type", "application/xml; charset=\"utf-8\"");
            if (report.run()) {
                break;
            }
        }
    }
}

std::string WebDAVSource::findByUID(const std::string &uid,
                                    const Timespec &deadline)
{
    RevisionMap_t revisions;
    std::string query;
    if (getContent() == "VCARD") {
        // use CardDAV
        query =
            "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
            "<C:addressbook-query xmlns:D=\"DAV:\"\n"
            "xmlns:C=\"urn:ietf:params:xml:ns:carddav:addressbook\">\n"
            "<D:prop>\n"
            "<D:getetag/>\n"
            "</D:prop>\n"
            "<C:filter>\n"
            "<C:comp-filter name=\"" + getContent() + "\">\n"
            "<C:prop-filter name=\"UID\">\n"
            "<C:text-match>" + uid + "</C:text-match>\n"
            "</C:prop-filter>\n"
            "</C:comp-filter>\n"
            "</C:filter>\n"
            "</C:addressbook-query>\n";
    } else {
        // use CalDAV
        query =
            "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
            "<C:calendar-query xmlns:D=\"DAV:\"\n"
            "xmlns:C=\"urn:ietf:params:xml:ns:caldav\">\n"
            "<D:prop>\n"
            "<D:getetag/>\n"
            "</D:prop>\n"
            "<C:filter>\n"
            "<C:comp-filter name=\"VCALENDAR\">\n"
            "<C:comp-filter name=\"" + getContent() + "\">\n"
            "<C:prop-filter name=\"UID\">\n"
            // Collation from RFC 4791, not supported yet by all servers.
            // Apple Calendar Server did not like CDATA.
            // TODO: escape special characters.
            "<C:text-match" /* collation=\"i;octet\" */ ">" /* <[CDATA[ */ + uid + /* ]]> */ "</C:text-match>\n"
            "</C:prop-filter>\n"
            "</C:comp-filter>\n"
            "</C:comp-filter>\n"
            "</C:filter>\n"
            "</C:calendar-query>\n";
    }
    getSession()->startOperation("REPORT 'UID lookup'", deadline);
    while (true) {
        Neon::XMLParser parser;
        parser.initReportParser(boost::bind(&WebDAVSource::checkItem, this,
                                            boost::ref(revisions),
                                            _1, _2, (std::string *)0));
        Neon::Request report(*getSession(), "REPORT", getCalendar().m_path, query, parser);
        report.addHeader("Depth", "1");
        report.addHeader("Content-Type", "application/xml; charset=\"utf-8\"");
        if (report.run()) {
            break;
        }
    }

    switch (revisions.size()) {
    case 0:
        SE_THROW_EXCEPTION_STATUS(TransportStatusException,
                                  "object not found",
                                  SyncMLStatus(404));
        break;
    case 1:
        return revisions.begin()->first;
        break;
    default:
        // should not happen
        SE_THROW(StringPrintf("UID %s not unique?!", uid.c_str()));
    }

    // not reached
    return "";
}

void WebDAVSource::listAllItemsCallback(const Neon::URI &uri,
                                        const ne_prop_result_set *results,
                                        RevisionMap_t &revisions,
                                        bool &failed)
{
    static const ne_propname prop = {
        "DAV:", "getetag"
    };
    static const ne_propname resourcetype = {
        "DAV:", "resourcetype"
    };

    const char *type = ne_propset_value(results, &resourcetype);
    if (type && strstr(type, "<DAV:collection></DAV:collection>")) {
        // skip collections
        return;
    }

    std::string uid = path2luid(uri.m_path);
    if (uid.empty()) {
        // skip collection itself (should have been detected as collection already)
        return;
    }

    const char *etag = ne_propset_value(results, &prop);
    if (etag) {
        std::string rev = ETag2Rev(etag);
        SE_LOG_DEBUG(NULL, "item %s = rev %s",
                     uid.c_str(), rev.c_str());
        revisions[uid] = rev;
    } else {
        failed = true;
        SE_LOG_ERROR(NULL,
                     "%s: %s",
                     uri.toURL().c_str(),
                     Neon::Status2String(ne_propset_status(results, &prop)).c_str());
    }
}

int WebDAVSource::checkItem(RevisionMap_t &revisions,
                            const std::string &href,
                            const std::string &etag,
                            std::string *data)
{
    // Ignore responses with no data: this is not perfect (should better
    // try to figure out why there is no data), but better than
    // failing.
    //
    // One situation is the response for the collection itself,
    // which comes with a 404 status and no data with Google Calendar.
    if (data && data->empty()) {
        return 0;
    }

    // No need to parse, user content cannot start at start of line in
    // iCalendar 2.0.
    if (!data ||
        data->find("\nBEGIN:" + getContent()) != data->npos) {
        std::string davLUID = path2luid(Neon::URI::parse(href).m_path);
        std::string rev = ETag2Rev(etag);
        revisions[davLUID] = rev;
    }

    // reset data for next item
    if (data) {
        data->clear();
    }
    return 0;
}


std::string WebDAVSource::path2luid(const std::string &path)
{
    // m_calendar.m_path is normalized, path is not.
    // Before comparing, normalize it.
    std::string res = Neon::URI::normalizePath(path, false);
    if (boost::starts_with(res, m_calendar.m_path)) {
        res = Neon::URI::unescape(res.substr(m_calendar.m_path.size()));
    } else {
        // keep full, absolute path as LUID
    }
    return res;
}

std::string WebDAVSource::luid2path(const std::string &luid)
{
    if (boost::starts_with(luid, "/")) {
        return luid;
    } else {
        return m_calendar.resolve(Neon::URI::escape(luid)).m_path;
    }
}

void WebDAVSource::readItem(const string &uid, std::string &item, bool raw)
{
    Timespec deadline = createDeadline();
    m_session->startOperation("GET", deadline);
    while (true) {
        item.clear();
        Neon::Request req(*m_session, "GET", luid2path(uid),
                          "", item);
        // useful with CardDAV: server might support more than vCard 3.0, but we don't
        req.addHeader("Accept", contentType());
        try {
            if (req.run()) {
                break;
            }
        } catch (const TransportStatusException &ex) {
            if (ex.syncMLStatus() == 410) {
                // Radicale reports 410 'Gone'. Hmm, okay.
                // Let's map it to the expected 404.
                SE_THROW_EXCEPTION_STATUS(TransportStatusException,
	                                  "object not found (was 410 'Gone')",
	                                  SyncMLStatus(404));
            }
            throw;
        }
    }
}

TrackingSyncSource::InsertItemResult WebDAVSource::insertItem(const string &uid, const std::string &item, bool raw)
{
    std::string new_uid;
    std::string rev;
    InsertItemResultState state = ITEM_OKAY;

    // By default use PUT. Change that to POST when creating new items
    // and server supports it. That avoids the problem of having to
    // choose a path and figuring out whether the server really used it.
    static const char putOperation[] = "PUT";
    static const char postOperation[] = "POST";
    const char *operation = putOperation;
    if (uid.empty()) {
        checkPostSupport();
        if (!m_postPath.empty()) {
            operation = postOperation;
        }
    }
    Timespec deadline = createDeadline(); // no resending if left empty
    m_session->startOperation(operation, deadline);
    std::string result;
    int counter = 0;
 retry:
    counter++;
    result = "";
    if (uid.empty()) {
        // Pick a resource name (done by derived classes, by default random),
        // catch unexpected conflicts via If-None-Match: *.
        std::string buffer;
        const std::string *data = createResourceName(item, buffer, new_uid);
        Neon::Request req(*m_session, operation,
                          operation == postOperation ? m_postPath : luid2path(new_uid),
                          *data, result);
        // Clearing the idempotent flag would allow us to clearly
        // distinguish between a connection error (no changes made
        // on server) and a server failure (may or may not have
        // changed something) because it'll close the connection
        // first.
        //
        // But because we are going to try resending
        // the PUT anyway in case of 5xx errors we might as well 
        // treat it like an idempotent request (which it is,
        // in a way, because we'll try to get our data onto
        // the server no matter what) and keep reusing an
        // existing connection.
        // req.setFlag(NE_REQFLAG_IDEMPOTENT, 0);

        // For this to work we must allow the server to overwrite
        // an item that we might have created before. Don't allow
        // that in the first attempt. Only relevant for PUT.
        if (operation != postOperation && counter == 1) {
            req.addHeader("If-None-Match", "*");
        }
        req.addHeader("Content-Type", contentType().c_str());
        static const std::set<int> expected = boost::assign::list_of(412)(403);
        if (!req.run(&expected)) {
            goto retry;
        }
        SE_LOG_DEBUG(NULL, "add item status: %s",
                     Neon::Status2String(req.getStatus()).c_str());
        switch (req.getStatusCode()) {
        case 204:
            // stored, potentially in a different resource than requested
            // when the UID was recognized
            break;
        case 201:
            // created
            break;
        case 403:
            // For a POST, this might be a UID conflict that we didn't detect
            // ourselves. Happens for VJOURNAL and the testInsertTwice test
            // when testing with Apple Calendar server. It then returns:
            // Content-Type: text/xml
            // Body:
            // <?xml version='1.0' encoding='UTF-8'?>
            // <error xmlns='DAV:'>
            //    <no-uid-conflict xmlns='urn:ietf:params:xml:ns:caldav'>
            //    <href xmlns='DAV:'>/calendars/__uids__/user01/tasks/c5490e736b6836c4d353d98bc78b3a3d.ics</href>
            //    </no-uid-conflict>
            //    <error-description xmlns='http://twistedmatrix.com/xml_namespace/dav/'>UID already exists</error-description>
            // </error>
            //
            // Handling that would be nice (see FDO #77424), but for now we just
            // do the same as for "Precondition Failed" and search for the UID.
            if (operation == postOperation) {
                try {
                    std::string uid = extractUID(item);
                    if (!uid.empty()) {
                        std::string luid = findByUID(uid, deadline);
                        return InsertItemResult(luid, "", ITEM_NEEDS_MERGE);
                    }
                } catch (...) {
                    // Ignore the error and report the original problem below.
                    Exception::log();
                }
            }
            SE_THROW_EXCEPTION_STATUS(TransportStatusException,
                                      std::string("unexpected status for PUT: ") +
                                      Neon::Status2String(req.getStatus()),
                                      SyncMLStatus(req.getStatus()->code));
            break;
        case 412: {
            // "Precondition Failed": our only precondition is the one about
            // If-None-Match, which means that there must be an existing item
            // with the same UID. Go find it, so that we can report back the
            // right luid.
            std::string uid = extractUID(item);
            std::string luid = findByUID(uid, deadline);
            return InsertItemResult(luid, "", ITEM_NEEDS_MERGE);
            break;
        }
        default:
            SE_THROW_EXCEPTION_STATUS(TransportStatusException,
                                      std::string("unexpected status for insert: ") +
                                      Neon::Status2String(req.getStatus()),
                                      SyncMLStatus(req.getStatus()->code));
            break;
        }
        rev = getETag(req);
        std::string real_luid = getLUID(req);
        if (!real_luid.empty()) {
            // Google renames the resource automatically to something of the form
            // <UID>.ics. Interestingly enough, our 1234567890!@#$%^&*()<>@dummy UID
            // test case leads to a resource path which Google then cannot find
            // via CalDAV. client-test must run with CLIENT_TEST_SIMPLE_UID=1...
            SE_LOG_DEBUG(NULL, "new item mapped to %s", real_luid.c_str());
            new_uid = real_luid;
            // TODO: find a better way of detecting unexpected updates.
            // state = ...
        } else if (!rev.empty()) {
            // Yahoo Contacts returns an etag, but no href. For items
            // that were really created as requested, that's okay. But
            // Yahoo Contacts silently merges the new contact with an
            // existing one, presumably if it is "similar" enough. The
            // web interface allows creating identical contacts
            // multiple times; not so CardDAV.  We are not even told
            // the path of that other contact...  Detect this by
            // checking whether the item really exists.
            //
            // Google also returns an etag without a href. However,
            // Google really creates a new item. We cannot tell here
            // whether merging took place. As we are supporting Google
            // but not Yahoo at the moment, let's assume that a new item
            // was created.
            RevisionMap_t revisions;
            bool failed = false;
            m_session->propfindURI(luid2path(new_uid), 0, getetag,
                                   boost::bind(&WebDAVSource::listAllItemsCallback,
                                               this, _1, _2, boost::ref(revisions),
                                               boost::ref(failed)),
                                   deadline);
            // Turns out we get a result for our original path even in
            // the case of a merge, although the original path is not
            // listed when looking at the collection.  Let's use that
            // to return the "real" uid to our caller.
            if (revisions.size() == 1 &&
                revisions.begin()->first != new_uid) {
                SE_LOG_DEBUG(NULL, "%s mapped to %s by peer",
                             new_uid.c_str(),
                             revisions.begin()->first.c_str());
                new_uid = revisions.begin()->first;
                // This would have to be uncommented for Yahoo.
                // state = ITEM_REPLACED;
            }
        }
    } else {
        new_uid = uid;
        std::string buffer;
        const std::string *data = setResourceName(item, buffer, new_uid);
        Neon::Request req(*m_session, "PUT", luid2path(new_uid),
                          *data, result);
        // See above for discussion of idempotent and PUT.
        // req.setFlag(NE_REQFLAG_IDEMPOTENT, 0);
        req.addHeader("Content-Type", contentType());
        // TODO: match exactly the expected revision, aka ETag,
        // or implement locking. Note that the ETag might not be
        // known, for example in this case:
        // - PUT succeeds
        // - PROPGET does not
        // - insertItem() fails
        // - Is retried? Might need slow sync in this case!
        //
        // req.addHeader("If-Match", etag);
        if (!req.run()) {
            goto retry;
        }
        SE_LOG_DEBUG(NULL, "update item status: %s",
                     Neon::Status2String(req.getStatus()).c_str());
        switch (req.getStatusCode()) {
        case 204:
            // the expected outcome, as we were asking for an overwrite
            break;
        case 201:
            // Huh? Shouldn't happen, but Google sometimes reports it
            // even when updating an item. Accept it.
            // SE_THROW("unexpected creation instead of update");
            break;
        default:
            SE_THROW_EXCEPTION_STATUS(TransportStatusException,
                                      std::string("unexpected status for update: ") +
                                      Neon::Status2String(req.getStatus()),
                                      SyncMLStatus(req.getStatus()->code));
            break;
        }
        rev = getETag(req);
        std::string real_luid = getLUID(req);
        if (!real_luid.empty() && real_luid != new_uid) {
            SE_THROW(StringPrintf("updating item: real luid %s does not match old luid %s",
                                  real_luid.c_str(), new_uid.c_str()));
        }
    }

    if (rev.empty()) {
        // Server did not include etag header. Must request it
        // explicitly (leads to race condition!). Google Calendar
        // assigns a new ETag even if the body has not changed,
        // so any kind of caching of ETag would not work either.
        bool failed = false;
        RevisionMap_t revisions;
        m_session->propfindURI(luid2path(new_uid), 0, getetag,
                               boost::bind(&WebDAVSource::listAllItemsCallback,
                                           this, _1, _2, boost::ref(revisions),
                                           boost::ref(failed)),
                               deadline);
        rev = revisions[new_uid];
        if (failed || rev.empty()) {
            SE_THROW("could not retrieve ETag");
        }
    }

    return InsertItemResult(new_uid, rev, state);
}

std::string WebDAVSource::ETag2Rev(const std::string &etag)
{
    std::string res = etag;
    if (boost::starts_with(res, "W/")) {
        res.erase(0, 2);
    }
    if (res.size() >= 2 &&
        res[0] == '"' &&
        res[res.size() - 1] == '"') {
        res = res.substr(1, res.size() - 2);
    }
    return res;
}

std::string WebDAVSource::getLUID(Neon::Request &req)
{
    std::string location = req.getResponseHeader("Location");
    if (location.empty()) {
        return location;
    } else {
        return path2luid(Neon::URI::parse(location).m_path);
    }
}

bool WebDAVSource::ignoreCollection(const StringMap &props) const
{
    // CardDAV and CalDAV both promise to not contain anything
    // unrelated to them
    StringMap::const_iterator it = props.find("DAV::resourcetype");
    if (it != props.end()) {
        const std::string &type = it->second;
        // allow parameters (no closing bracket)
        // and allow also "carddavaddressbook" (caused by invalid Neon 
        // string concatenation?!)
        if (type.find("<urn:ietf:params:xml:ns:caldav:calendar") != type.npos ||
            type.find("<urn:ietf:params:xml:ns:caldavcalendar") != type.npos ||
            type.find("<urn:ietf:params:xml:ns:carddav:addressbook") != type.npos ||
            type.find("<urn:ietf:params:xml:ns:carddavaddressbook") != type.npos) {
            return true;
        }
    }
    return false;
}

Timespec WebDAVSource::createDeadline() const
{
    int timeoutSeconds = m_settings->timeoutSeconds();
    int retrySeconds = m_settings->retrySeconds();
    if (timeoutSeconds > 0 &&
        retrySeconds > 0) {
        return Timespec::monotonic() + timeoutSeconds;
    } else {
        return Timespec();
    }
}

void WebDAVSource::removeItem(const string &uid)
{
    Timespec deadline = createDeadline();
    m_session->startOperation("DELETE", deadline);
    std::string item, result;
    boost::scoped_ptr<Neon::Request> req;
    while (true) {
        req.reset(new Neon::Request(*m_session, "DELETE", luid2path(uid),
                                    item, result));
        // TODO: match exactly the expected revision, aka ETag,
        // or implement locking.
        // req.addHeader("If-Match", etag);
        static const std::set<int> expected = boost::assign::list_of(412);
        if (req->run(&expected)) {
            break;
        }
    }
    SE_LOG_DEBUG(NULL, "remove item status: %s",
                 Neon::Status2String(req->getStatus()).c_str());
    switch (req->getStatusCode()) {
    case 204:
        // the expected outcome
        break;
    case 200:
        // reported by Radicale, also okay
        break;
    case 412:
        // Radicale reports 412 'Precondition Failed'. Hmm, okay.
        // Let's map it to the expected 404.
        SE_THROW_EXCEPTION_STATUS(TransportStatusException,
                                  "object not found (was 412 'Precondition Failed')",
                                  SyncMLStatus(404));
        break;
    default:
        SE_THROW_EXCEPTION_STATUS(TransportStatusException,
                                  std::string("unexpected status for removal: ") +
                                  Neon::Status2String(req->getStatus()),
                                  SyncMLStatus(req->getStatus()->code));
        break;
    }
}

#endif /* ENABLE_DAV */

SE_END_CXX


#ifdef ENABLE_MODULES
# include "WebDAVSourceRegister.cpp"
#endif
