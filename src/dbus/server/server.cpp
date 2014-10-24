/*
 * Copyright (C) 2009 Intel Corporation
 * Copyright (C) 2011 Symbio, Ville Nummela
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

#include <fstream>

#include <boost/bind.hpp>

#include <syncevo/GLibSupport.h>

#include "server.h"
#include "info-req.h"
#include "connection.h"
#include "bluez-manager.h"
#include "session.h"
#include "timeout.h"
#include "restart.h"
#include "client.h"
#include "auto-sync-manager.h"
#include "connman-client.h"
#include "network-manager-client.h"
#include "presence-status.h"

#include <boost/pointer_cast.hpp>

using namespace GDBusCXX;

SE_BEGIN_CXX

void Server::onIdleChange(bool idle)
{
    SE_LOG_DEBUG(NULL, "server is %s", idle ? "idle" : "not idle");
    if (idle) {
        autoTermUnref();
    } else {
        autoTermRef();
    }
}

class ServerLogger : public Logger
{
    Logger::Handle m_parentLogger;
    // Currently a strong reference. Would be a weak reference
    // if we had proper reference counting for Server.
    boost::shared_ptr<Server> m_server;

public:
    ServerLogger(const boost::shared_ptr<Server> &server) :
        m_parentLogger(Logger::instance()),
        m_server(server)
    {
    }

    virtual void remove() throw ()
    {
        // Hold the Logger mutex while cutting our connection to the
        // server. The code using m_server below does the same and
        // holds the mutex while logging. That way we prevent threads
        // from holding onto the server while it tries to shut down.
        //
        // This is important because the server's live time is not
        // really controlled via the boost::shared_ptr, it may
        // destruct while there are still references. See
        // Server::m_logger instantiation below.
        RecMutex::Guard guard = lock();
        m_server.reset();
    }

    virtual void messagev(const MessageOptions &options,
                          const char *format,
                          va_list args)
    {
        // Ensure that remove() cannot proceed while we have the
        // server in use.
        RecMutex::Guard guard = lock();
        Server *server = m_server.get();
        message2DBus(server,
                     options,
                     format,
                     args,
                     server ? server->getPath() : "",
                     getProcessName());
    }

    /**
     * @param server    may be NULL, in which case logging only goes to parent
     */
    void message2DBus(Server *server,
                      const MessageOptions &options,
                      const char *format,
                      va_list args,
                      const std::string &dbusPath,
                      const std::string &procname)
    {
        // Keeps logging consistent: otherwise thread A might log to
        // parent, thread B to parent and D-Bus, then thread A
        // finishes its logging via D-Bus.  The order of log messages
        // would then not be the same in the parent and D-Bus.
        RecMutex::Guard guard = lock();

        // iterating over args in messagev() is destructive, must make a copy first
        va_list argsCopy;
        va_copy(argsCopy, args);
        m_parentLogger.messagev(options, format, args);

        if (server) {
            try {
                if (options.m_level <= server->getDBusLogLevel()) {
                    string log = StringPrintfV(format, argsCopy);
                    server->logOutput(dbusPath, options.m_level, log, procname);
                }
            } catch (...) {
                remove();
                // Give up on server logging silently.
            }
        }
        va_end(argsCopy);
    }
};

void Server::clientGone(Client *c)
{
    for (Clients_t::iterator it = m_clients.begin();
        it != m_clients.end();
        ++it) {
        if (it->second.get() == c) {
            SE_LOG_DEBUG(NULL, "D-Bus client %s has disconnected",
                         c->m_ID.c_str());
            autoTermUnref(it->second->getAttachCount());
            m_clients.erase(it);
            return;
        }
    }
    SE_LOG_DEBUG(NULL, "unknown client has disconnected?!");
}

std::string Server::getNextSession()
{
    // Make the session ID somewhat random. This protects to
    // some extend against injecting unwanted messages into the
    // communication.
    m_lastSession++;
    if (!m_lastSession) {
        m_lastSession++;
    }
    return StringPrintf("%u%u", rand(), m_lastSession);
}

vector<string> Server::getCapabilities()
{
    // Note that this is tested by test-dbus.py in
    // TestServer.testCapabilities, update the test when adding
    // capabilities.
    vector<string> capabilities;

    capabilities.push_back("ConfigChanged");
    capabilities.push_back("GetConfigName");
    capabilities.push_back("NamedConfig");
    capabilities.push_back("Notifications");
    capabilities.push_back("Version");
    capabilities.push_back("SessionFlags");
    capabilities.push_back("SessionAttach");
    capabilities.push_back("DatabaseProperties");
    return capabilities;
}

StringMap Server::getVersions()
{
    StringMap versions;

    versions["version"] = VERSION;
    versions["system"] = EDSAbiWrapperInfo();
    versions["backends"] = SyncSource::backendsInfo();
    return versions;
}

void Server::attachClient(const Caller_t &caller,
                          const boost::shared_ptr<Watch> &watch)
{
    boost::shared_ptr<Client> client = addClient(caller, watch);
    autoTermRef();
    client->increaseAttachCount();
}

void Server::detachClient(const Caller_t &caller)
{
    boost::shared_ptr<Client> client = findClient(caller);
    if (client) {
        autoTermUnref();
        client->decreaseAttachCount();
    }
}

void Server::setNotifications(bool enabled,
                              const Caller_t &caller,
                              const string & /* notifications */)
{
    boost::shared_ptr<Client> client = findClient(caller);
    if (client && client->getAttachCount()) {
        client->setNotificationsEnabled(enabled);
    } else {
        SE_THROW("client not attached, not allowed to change notifications");
    }
}

bool Server::notificationsEnabled()
{
    for (Clients_t::iterator it = m_clients.begin();
        it != m_clients.end();
        ++it) {
        if (!it->second->getNotificationsEnabled()) {
            return false;
        }
    }
    return true;
}

void Server::connect(const Caller_t &caller,
                     const boost::shared_ptr<Watch> &watch,
                     const StringMap &peer,
                     bool must_authenticate,
                     const std::string &session,
                     DBusObject_t &object)
{
    if (m_shutdownRequested) {
        // don't allow new connections, we cannot activate them
        SE_THROW("server shutting down");
    }

    if (!session.empty()) {
        // reconnecting to old connection is not implemented yet
        throw std::runtime_error("not implemented");
    }
    std::string new_session = getNextSession();

    boost::shared_ptr<Connection> c(Connection::createConnection(*this,
                                                                 getConnection(),
                                                                 new_session,
                                                                 peer,
                                                                 must_authenticate));
    SE_LOG_DEBUG(NULL, "connecting D-Bus client %s with connection %s '%s'",
                 caller.c_str(),
                 c->getPath(),
                 c->m_description.c_str());

    boost::shared_ptr<Client> client = addClient(caller,
                                                 watch);
    client->attach(c);
    c->activate();

    object = c->getPath();
}

void Server::startSessionWithFlags(const Caller_t &caller,
                                   const boost::shared_ptr<Watch> &watch,
                                   const std::string &server,
                                   const std::vector<std::string> &flags,
                                   DBusObject_t &object)
{
    if (m_shutdownRequested) {
        // don't allow new sessions, we cannot activate them
        SE_THROW("server shutting down");
    }

    boost::shared_ptr<Client> client = addClient(caller,
                                                 watch);
    std::string new_session = getNextSession();
    boost::shared_ptr<Session> session = Session::createSession(*this,
                                                                "is this a client or server session?",
                                                                server,
                                                                new_session,
                                                                flags);
    client->attach(session);
    session->activate();
    enqueue(session);
    object = session->getPath();
}


boost::shared_ptr<Session> Server::startInternalSession(const std::string &server,
                                                        SessionFlags flags,
                                                        const boost::function<void (const boost::weak_ptr<Session> &session)> &callback)
{
    if (m_shutdownRequested) {
        // don't allow new sessions, we cannot activate them
        SE_THROW("server shutting down");
    }

    std::vector<std::string> dbusFlags;
    if (flags & SESSION_FLAG_NO_SYNC) {
        dbusFlags.push_back("no-sync");
    }
    if (flags & SESSION_FLAG_ALL_CONFIGS) {
        dbusFlags.push_back("all-configs");
    }

    std::string new_session = getNextSession();
    boost::shared_ptr<Session> session = Session::createSession(*this,
                                                                "is this a client or server session?",
                                                                server,
                                                                new_session,
                                                                dbusFlags);
    session->m_sessionActiveSignal.connect(boost::bind(callback, boost::weak_ptr<Session>(session)));
    session->activate();
    enqueue(session);
    return session;
}


void Server::checkPresence(const std::string &server,
                           std::string &status,
                           std::vector<std::string> &transports)
{
    return getPresenceStatus().checkPresence(server, status, transports);
}

void Server::getSessions(std::vector<DBusObject_t> &sessions)
{
    sessions.reserve(m_workQueue.size() + 1);
    if (m_activeSession) {
        sessions.push_back(m_activeSession->getPath());
    }
    BOOST_FOREACH(boost::weak_ptr<Session> &session, m_workQueue) {
        boost::shared_ptr<Session> s = session.lock();
        if (s) {
            sessions.push_back(s->getPath());
        }
    }
}

Server::Server(GMainLoop *loop,
               boost::shared_ptr<Restart> &restart,
               const DBusConnectionPtr &conn,
               int duration) :
    DBusObjectHelper(conn,
                     SessionCommon::SERVER_PATH,
                     SessionCommon::SERVER_IFACE,
                     boost::bind(&Server::autoTermCallback, this)),
    m_loop(loop),
    m_suspendFlagsSource(0),
    m_shutdownRequested(false),
    m_restart(restart),
    m_conn(conn),
    m_lastSession(time(NULL)),
    m_activeSession(NULL),
    m_lastInfoReq(0),
    m_bluezManager(new BluezManager(*this)),
    sessionChanged(*this, "SessionChanged"),
    presence(*this, "Presence"),
    templatesChanged(*this, "TemplatesChanged"),
    configChanged(*this, "ConfigChanged"),
    infoRequest(*this, "InfoRequest"),
    m_logOutputSignal(*this, "LogOutput"),
    m_autoTerm(m_loop, m_shutdownRequested, duration),
    m_dbusLogLevel(Logger::INFO),
    // TODO (?): turn Server into a proper reference counted instance.
    // This would help with dangling references to it when other threads
    // use it for logging, see ServerLogger. However, with mutex locking
    // in ServerLogger that shouldn't be a problem.
    m_logger(new ServerLogger(boost::shared_ptr<Server>(this, NopDestructor())))
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srand(tv.tv_usec);
    add(this, &Server::getCapabilities, "GetCapabilities");
    add(this, &Server::getVersions, "GetVersions");
    add(this, &Server::attachClient, "Attach");
    add(this, &Server::detachClient, "Detach");
    add(this, &Server::enableNotifications, "EnableNotifications");
    add(this, &Server::disableNotifications, "DisableNotifications");
    add(this, &Server::notificationAction, "NotificationAction");
    add(this, &Server::connect, "Connect");
    add(this, &Server::startSession, "StartSession");
    add(this, &Server::startSessionWithFlags, "StartSessionWithFlags");
    add(this, &Server::getConfigs, "GetConfigs");
    add(this, &Server::getConfig, "GetConfig");
    add(this, &Server::getReports, "GetReports");
    add(this, &Server::checkSource, "CheckSource");
    add(this, &Server::getDatabases, "GetDatabases");
    add(this, &Server::checkPresence, "CheckPresence");
    add(this, &Server::getSessions, "GetSessions");
    add(this, &Server::infoResponse, "InfoResponse");
    add(sessionChanged);
    add(templatesChanged);
    add(configChanged);
    add(presence);
    add(infoRequest);
    add(m_logOutputSignal);

    // Log entering and leaving idle state and
    // allow/prevent auto-termination.
    m_idleSignal.connect(boost::bind(&Server::onIdleChange, this, _1));

    // connect ConfigChanged signal to source for that information
    m_configChangedSignal.connect(boost::bind(boost::ref(configChanged)));
}

gboolean Server::onSuspendFlagsChange(GIOChannel *source,
                                      GIOCondition condition,
                                      gpointer data) throw ()
{
    Server *me = static_cast<Server *>(data);
    try {
        if (!SuspendFlags::getSuspendFlags().isNormal()) {
            me->m_shutdownRequested = true;
            g_main_loop_quit(me->m_loop);
            SE_LOG_INFO(NULL, "server shutting down because of SIGINT or SIGTERM");
        }
    } catch (...) {
        Exception::handle();
    }
    // Keep watching, just in case that we catch multiple signals.
    return TRUE;
}

void Server::activate()
{
    // Watch SuspendFlags fd to react to signals quickly.
    int fd = SuspendFlags::getSuspendFlags().getEventFD();
    GIOChannelCXX channel(g_io_channel_unix_new(fd), TRANSFER_REF);
    m_suspendFlagsSource = g_io_add_watch(channel, G_IO_IN, onSuspendFlagsChange, this);

    // Activate our D-Bus object *before* interacting with D-Bus
    // any further. Otherwise GIO D-Bus will start processing
    // messages for us while we start up and reject them because
    // out object isn't visible to it yet.
    GDBusCXX::DBusObjectHelper::activate();

    // Push ourselves as logger for the time being.
    m_logger->setLevel(Logger::DEBUG);
    m_pushLogger.reset(m_logger);

    m_presence.reset(new PresenceStatus(*this));

    // Assume that Bluetooth is available. Neither ConnMan nor Network
    // manager can tell us about that. The "Bluetooth" ConnMan technology
    // is about IP connection via Bluetooth - not what we need.
    getPresenceStatus().updatePresenceStatus(true, PresenceStatus::BT_TRANSPORT);

    m_connman.reset(new ConnmanClient(*this));
    m_networkManager.reset(new NetworkManagerClient(*this));

    if ((!m_connman || !m_connman->isAvailable()) &&
        (!m_networkManager || !m_networkManager->isAvailable())) {
        // assume that we are online if no network manager was found at all
        getPresenceStatus().updatePresenceStatus(true, PresenceStatus::HTTP_TRANSPORT);
    }

    // create auto sync manager, now that server is ready
    m_autoSync = AutoSyncManager::createAutoSyncManager(*this);
}

Server::~Server()
{
    // make sure all other objects are gone before destructing ourselves
    if (m_suspendFlagsSource) {
        g_source_remove(m_suspendFlagsSource);
    }
    m_syncSession.reset();
    m_workQueue.clear();
    m_clients.clear();
    m_autoSync.reset();
    m_infoReqMap.clear();
    m_timeouts.clear();
    m_delayDeletion.clear();
    m_connman.reset();
    m_networkManager.reset();
    m_presence.reset();

    m_pushLogger.reset();
    m_logger.reset();
}

bool Server::shutdown()
{
    Timespec now = Timespec::monotonic();
    bool autosync = m_autoSync && m_autoSync->preventTerm();
    SE_LOG_DEBUG(NULL, "shut down or restart server at %lu.%09lu because of file modifications, auto sync %s",
                 now.tv_sec, now.tv_nsec, autosync ? "on" : "off");
    if (autosync) {
        // suitable exec() call which restarts the server using the same environment it was in
        // when it was started
        SE_LOG_INFO(NULL, "server restarting because files loaded into memory were modified on disk");
        m_restart->restart();
    } else {
        // leave server now
        g_main_loop_quit(m_loop);
        SE_LOG_INFO(NULL, "server shutting down because files loaded into memory were modified on disk");
    }

    return false;
}

void Server::fileModified(const std::string &file)
{
    SE_LOG_DEBUG(NULL, "file %s modified, %s shutdown: %s, %s",
                 file.c_str(),
                 m_shutdownRequested ? "continuing" : "initiating",
                 m_shutdownTimer ? "timer already active" : "timer not yet active",
                 m_activeSession ? "waiting for active session to finish" : "setting timer");
    m_lastFileMod = Timespec::monotonic();
    if (!m_activeSession) {
        m_shutdownTimer.activate(SHUTDOWN_QUIESENCE_SECONDS,
                                 boost::bind(&Server::shutdown, this));
    }
    m_shutdownRequested = true;
}

void Server::run()
{
    // This has the intended side effect that it loads everything into
    // memory which might be dynamically loadable, like backend
    // plugins.
    StringMap map = getVersions();
    SE_LOG_DEBUG(NULL, "D-Bus server ready to run, versions:");
    BOOST_FOREACH(const StringPair &entry, map) {
        SE_LOG_DEBUG(NULL, "%s: %s", entry.first.c_str(), entry.second.c_str());
    }

    // Now that everything is loaded, check memory map for files which we have to monitor.
    set<string> files;
    std::ifstream in("/proc/self/maps");
    while (!in.eof()) {
        string line;
        getline(in, line);
        size_t off = line.find('/');
        if (off != line.npos &&
            line.find(" r-xp ") != line.npos) {
            files.insert(line.substr(off));
        }
    }
    in.close();
    BOOST_FOREACH(const string &file, files) {
        try {
            SE_LOG_DEBUG(NULL, "watching: %s", file.c_str());
            boost::shared_ptr<SyncEvo::GLibNotify> notify(new GLibNotify(file.c_str(), boost::bind(&Server::fileModified, this, file)));
            m_files.push_back(notify);
        } catch (...) {
            // ignore errors for indidividual files
            Exception::handle();
        }
    }

    SE_LOG_INFO(NULL, "ready to run");
    // Note that with GDBus GIO, this will also finally request the
    // "org.syncevolution" name. This relies on preserving the name in
    // m_conn that we originally passed to dbus_get_bus_connection().
    // getConnection() works with a plain GDBusConnection and doesn't
    // have the name, so we really need our own copy of
    // DBusConnectionPtr here.
    dbus_bus_connection_undelay(m_conn);
    if (!m_shutdownRequested) {
        g_main_loop_run(m_loop);
    }

    SE_LOG_DEBUG(NULL, "%s", "Exiting Server::run");
}


/**
 * look up client by its ID
 */
boost::shared_ptr<Client> Server::findClient(const Caller_t &ID)
{
    for (Clients_t::iterator it = m_clients.begin();
        it != m_clients.end();
        ++it) {
        if (it->second->m_ID == ID) {
            return it->second;
        }
    }
    return boost::shared_ptr<Client>();
}

boost::shared_ptr<Client> Server::addClient(const Caller_t &ID,
                                            const boost::shared_ptr<Watch> &watch)
{
    boost::shared_ptr<Client> client(findClient(ID));
    if (client) {
        return client;
    }
    client.reset(new Client(*this, ID));
    // add to our list *before* checking that peer exists, so
    // that clientGone() can remove it if the check fails
    m_clients.push_back(std::make_pair(watch, client));
    watch->setCallback(boost::bind(&Server::clientGone, this, client.get()));
    return client;
}


void Server::detach(Resource *resource)
{
    BOOST_FOREACH(const Clients_t::value_type &client_entry,
                  m_clients) {
        client_entry.second->detachAll(resource);
    }
}

void Server::enqueue(const boost::shared_ptr<Session> &session)
{
    bool idle = isIdle();

    WorkQueue_t::iterator it = m_workQueue.end();
    while (it != m_workQueue.begin()) {
        --it;
        // skip over dead sessions, they will get cleaned up elsewhere
        boost::shared_ptr<Session> session = it->lock();
        if (session && session->getPriority() <= session->getPriority()) {
            ++it;
            break;
        }
    }
    m_workQueue.insert(it, session);
    checkQueue();

    if (idle) {
        m_idleSignal(false);
    }
}

void Server::killSessionsAsync(const std::string &peerDeviceID,
                               const SimpleResult &onResult)
{
    WorkQueue_t::iterator it = m_workQueue.begin();
    while (it != m_workQueue.end()) {
        boost::shared_ptr<Session> session = it->lock();
        if (session && session->getPeerDeviceID() == peerDeviceID) {
            SE_LOG_DEBUG(NULL, "removing pending session %s because it matches deviceID %s",
                         session->getSessionID().c_str(),
                         peerDeviceID.c_str());
            // remove session and its corresponding connection
            boost::shared_ptr<Connection> c = session->getStubConnection().lock();
            if (c) {
                c->shutdown();
            }
            it = m_workQueue.erase(it);
        } else {
            ++it;
        }
    }

    // Check active session. We need to wait for it to shut down cleanly.
    boost::shared_ptr<Session> active = m_activeSessionRef.lock();
    if (active &&
        active->getPeerDeviceID() == peerDeviceID) {
        SE_LOG_DEBUG(NULL, "aborting active session %s because it matches deviceID %s",
                     active->getSessionID().c_str(),
                     peerDeviceID.c_str());
        // hand over work to session
        active->abortAsync(onResult);
    } else {
        onResult.done();
    }
}

void Server::dequeue(Session *session)
{
    bool idle = isIdle();

    if (m_syncSession.get() == session) {
        // This is the running sync session.
        // It's not in the work queue and we have to
        // keep it active, so nothing to do.
        return;
    }

    for (WorkQueue_t::iterator it = m_workQueue.begin();
         it != m_workQueue.end();
         ++it) {
        if (it->lock().get() == session) {
            // remove from queue
            m_workQueue.erase(it);
            break;
        }
    }

    if (m_activeSession == session) {
        // The session is releasing the lock, so someone else might
        // run now.
        sessionChanged(session->getPath(), false);
        m_activeSession = NULL;
        m_activeSessionRef.reset();
        checkQueue();
    }

    if (!idle && isIdle()) {
        m_idleSignal(true);
    }
}

void Server::addSyncSession(Session *session)
{
    // Only one session can run a sync, and only the active session
    // can make itself the sync session.
    if (m_syncSession) {
        if (m_syncSession.get() != session) {
            SE_THROW("already have a sync session");
        } else {
            return;
        }
    }
    m_syncSession = m_activeSessionRef.lock();
    m_newSyncSessionSignal(m_syncSession);
    if (!m_syncSession) {
        SE_THROW("session should not start a sync, all clients already detached");
    }
    if (m_syncSession.get() != session) {
        m_syncSession.reset();
        SE_THROW("inactive session asked to become sync session");
    }
}

void Server::removeSyncSession(Session *session)
{
    if (session == m_syncSession.get()) {
        // Normally the owner calls this, but if it is already gone,
        // then do it again and thus effectively start counting from
        // now.
        delaySessionDestruction(m_syncSession);
        m_syncSession.reset();
    } else {
        SE_LOG_DEBUG(NULL, "ignoring removeSyncSession() for session %s, it is not the sync session",
                     session->getSessionID().c_str());
    }
}

static void quitLoop(GMainLoop *loop)
{
    SE_LOG_DEBUG(NULL, "stopping server's event loop");
    g_main_loop_quit(loop);
}

void Server::checkQueue()
{
    if (m_activeSession) {
        // still busy
        return;
    }

    if (m_shutdownRequested) {
        // Don't schedule new sessions. Instead return to Server::run().
        // But don't do it immediately: when done inside the Session.Detach()
        // call, the D-Bus response was not delivered reliably to the client
        // which caused the shutdown.
        SE_LOG_DEBUG(NULL, "shutting down in checkQueue(), idle and shutdown was requested");
        addTimeout(boost::bind(quitLoop, m_loop), 0);
        return;
    }

    while (!m_workQueue.empty()) {
        boost::shared_ptr<Session> session = m_workQueue.front().lock();
        m_workQueue.pop_front();
        if (session) {
            // activate the session
            m_activeSession = session.get();
            m_activeSessionRef = session;
            SE_LOG_DEBUG(NULL, "activating session %p", m_activeSession);
            session->activateSession();
            sessionChanged(session->getPath(), true);
            return;
        }
    }
}

void Server::sessionExpired(const boost::shared_ptr<Session> &session)
{
    SE_LOG_DEBUG(NULL, "session %s expired",
                 session->getSessionID().c_str());
}

void Server::delaySessionDestruction(const boost::shared_ptr<Session> &session)
{
    if (!session) {
        return;
    }

    SE_LOG_DEBUG(NULL, "delaying destruction of session %s by one minute",
                 session->getSessionID().c_str());
    addTimeout(boost::bind(&Server::sessionExpired,
                           session),
               60 /* 1 minute */);
}

inline void insertPair(std::map<string, string> &params,
                       const string &key,
                       const string &value)
{
    if(!value.empty()) {
        params.insert(pair<string, string>(key, value));
    }
}


boost::shared_ptr<InfoReq> Server::passwordRequest(const string &descr,
                                                   const ConfigPasswordKey &key,
                                                   const boost::weak_ptr<Session> &s)
{
    boost::shared_ptr<Session> session = s.lock();
    if (!session) {
        // already gone, ignore request
        return boost::shared_ptr<InfoReq>();
    }

    std::map<string, string> params;
    insertPair(params, "description", descr);
    insertPair(params, "user", key.user);
    insertPair(params, "SyncML server", key.server);
    insertPair(params, "domain", key.domain);
    insertPair(params, "object", key.object);
    insertPair(params, "protocol", key.protocol);
    insertPair(params, "authtype", key.authtype);
    insertPair(params, "port", key.port ? StringPrintf("%u",key.port) : "");
    boost::shared_ptr<InfoReq> req = createInfoReq("password", params, *session);
    // Return password or failure to Session and thus the session helper.
    req->m_responseSignal.connect(boost::bind(&Server::passwordResponse,
                                              this,
                                              _1,
                                              s));
    // Tell session about timeout.
    req->m_timeoutSignal.connect(InfoReq::TimeoutSignal_t::slot_type(&Session::passwordResponse,
                                                                     session.get(),
                                                                     true,
                                                                     false,
                                                                     "").track(s));
    // Request becomes obsolete when session is done.
    session->m_doneSignal.connect(boost::bind(&Server::removeInfoReq,
                                              this,
                                              req->getId()));

    return req;
}

void Server::passwordResponse(const InfoReq::InfoMap &response,
                              const boost::weak_ptr<Session> &s)
{
    boost::shared_ptr<Session> session = s.lock();
    if (!session) {
        // already gone, ignore request
        return;
    }

    InfoReq::InfoMap::const_iterator it = response.find("password");
    if (it == response.end()) {
        // no password provided, user wants to abort
        session->passwordResponse(false, true, "");
    } else {
        // password provided, might be empty
        session->passwordResponse(false, false, it->second);
    }
}


bool Server::callTimeout(const boost::shared_ptr<Timeout> &timeout, const boost::function<void ()> &callback)
{
    callback();
    // We are executing the timeout, don't invalidate the instance
    // until later when our caller is no longer using the instance to
    // call us.
    delayDeletion(timeout);
    m_timeouts.remove(timeout);
    return false;
}

void Server::addTimeout(const boost::function<void ()> &callback,
                        int seconds)
{
    boost::shared_ptr<Timeout> timeout(new Timeout);
    m_timeouts.push_back(timeout);
    timeout->activate(seconds,
                      boost::bind(&Server::callTimeout,
                                  this,
                                  // avoid copying the shared pointer here,
                                  // otherwise the Timeout will never be deleted
                                  boost::ref(m_timeouts.back()),
                                  callback));
}

void Server::infoResponse(const Caller_t &caller,
                          const std::string &id,
                          const std::string &state,
                          const std::map<string, string> &response)
{
    InfoReqMap::iterator it = m_infoReqMap.find(id);
    // if not found, ignore
    if (it != m_infoReqMap.end()) {
        const boost::shared_ptr<InfoReq> infoReq = it->second.lock();
        if (infoReq) {
            infoReq->setResponse(caller, state, response);
        }
    }
}

boost::shared_ptr<InfoReq> Server::createInfoReq(const string &type,
                                                 const std::map<string, string> &parameters,
                                                 const Session &session)
{
    boost::shared_ptr<InfoReq> infoReq(new InfoReq(*this, type, parameters, session.getPath()));
    m_infoReqMap.insert(std::make_pair(infoReq->getId(), infoReq));
    // will be removed automatically
    infoReq->m_responseSignal.connect(boost::bind(&Server::removeInfoReq,
                                                  this,
                                                  infoReq->getId()));
    infoReq->m_timeoutSignal.connect(boost::bind(&Server::removeInfoReq,
                                                 this,
                                                 infoReq->getId()));
    return infoReq;
}

std::string Server::getNextInfoReq()
{
    return StringPrintf("%u", ++m_lastInfoReq);
}

void Server::emitInfoReq(const InfoReq &req)
{
    infoRequest(req.getId(),
                req.getSessionPath(),
                req.getInfoStateStr(),
                req.getHandler(),
                req.getType(),
                req.getParam());
}

void Server::removeInfoReq(const std::string &id)
{
    // remove InfoRequest from hash map
    m_infoReqMap.erase(id);
}

PresenceStatus &Server::getPresenceStatus()
{
    if (!m_presence) {
        SE_THROW("internal error: Server::getPresenceStatus() called while server has no instance");
    }
    return *m_presence;
}

void Server::getDeviceList(SyncConfig::DeviceList &devices)
{
    //wait bluez or other device managers
    // TODO: make this asynchronous?!
    while(!m_bluezManager->isDone()) {
        g_main_loop_run(m_loop);
    }

    devices.clear();
    devices = m_syncDevices;
}

void Server::addPeerTempl(const string &templName,
                          const boost::shared_ptr<SyncConfig::TemplateDescription> peerTempl)
{
    std::string lower = templName;
    boost::to_lower(lower);
    m_matchedTempls.insert(MatchedTemplates::value_type(lower, peerTempl));
}

boost::shared_ptr<SyncConfig::TemplateDescription> Server::getPeerTempl(const string &peer)
{
    std::string lower = peer;
    boost::to_lower(lower);
    MatchedTemplates::iterator it = m_matchedTempls.find(lower);
    if(it != m_matchedTempls.end()) {
        return it->second;
    } else {
        return boost::shared_ptr<SyncConfig::TemplateDescription>();
    }
}

bool Server::getDevice(const string &deviceId, SyncConfig::DeviceDescription &device)
{
    SyncConfig::DeviceList::iterator syncDevIt;
    for (syncDevIt = m_syncDevices.begin(); syncDevIt != m_syncDevices.end(); ++syncDevIt) {
        if (boost::equals(syncDevIt->m_deviceId, deviceId)) {
            device = *syncDevIt;
            if (syncDevIt->m_pnpInformation) {
                device.m_pnpInformation = boost::shared_ptr<SyncConfig::PnpInformation>(
                    new SyncConfig::PnpInformation(syncDevIt->m_pnpInformation->m_vendor,
                                                   syncDevIt->m_pnpInformation->m_product));
            }
            return true;
        }
    }
    return false;
}

void Server::addDevice(const SyncConfig::DeviceDescription &device)
{
    SyncConfig::DeviceList::iterator it;
    for (it = m_syncDevices.begin(); it != m_syncDevices.end(); ++it) {
        if (boost::iequals(it->m_deviceId, device.m_deviceId)) {
            break;
        }
    }
    if (it == m_syncDevices.end()) {
        m_syncDevices.push_back(device);
        templatesChanged();
    }
}

void Server::removeDevice(const string &deviceId)
{
    SyncConfig::DeviceList::iterator syncDevIt;
    for (syncDevIt = m_syncDevices.begin(); syncDevIt != m_syncDevices.end(); ++syncDevIt) {
        if (boost::equals(syncDevIt->m_deviceId, deviceId)) {
            m_syncDevices.erase(syncDevIt);
            templatesChanged();
            break;
        }
    }
}

void Server::updateDevice(const string &deviceId,
                          const SyncConfig::DeviceDescription &device)
{
    SyncConfig::DeviceList::iterator it;
    for (it = m_syncDevices.begin(); it != m_syncDevices.end(); ++it) {
        if (boost::iequals(it->m_deviceId, deviceId)) {
            (*it) = device;
            templatesChanged();
            break;
        }
    }
}

void Server::message2DBus(const Logger::MessageOptions &options,
                          const char *format,
                          va_list args,
                          const std::string &dbusPath,
                          const std::string &procname)
{
    // prefix is used to set session path
    // for general server output, the object path field is dbus server
    // the object path can't be empty for object paths prevent using empty string.
    m_logger->message2DBus(this, options, format, args, dbusPath, procname);
}

void Server::logOutput(const GDBusCXX::DBusObject_t &path,
                       Logger::Level level,
                       const std::string &explanation,
                       const std::string &procname)
{
    if (level <= m_dbusLogLevel) {
        string strLevel = Logger::levelToStr(level);
        m_logOutputSignal(path, strLevel, explanation, procname);
    }
}

SE_END_CXX
