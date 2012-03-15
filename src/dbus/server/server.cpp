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

#include <syncevo/GLibSupport.h>

#include "server.h"
#include "info-req.h"
#include "bluez-manager.h"
#include "connection-resource.h"
#include "session-resource.h"
#include "timeout.h"
#include "restart.h"
#include "client.h"
#include "session-common.h"
#include "dbus-callbacks.h"

#include <boost/pointer_cast.hpp>

using namespace GDBusCXX;

SE_BEGIN_CXX

namespace {

void resultDoneCb(const boost::shared_ptr<GDBusCXX::DBusObjectHelper> &helper,
                  const boost::shared_ptr<GDBusCXX::Result1<GDBusCXX::DBusObject_t> > &result)
{
  result->done(helper->getPath());
}

}

void Server::clientGone(Client *c)
{
    for (Clients_t::iterator it = m_clients.begin();
        it != m_clients.end();
        ++it) {
        if (it->second.get() == c) {
            SE_LOG_DEBUG(NULL, NULL, "D-Bus client %s has disconnected",
                         c->m_ID.c_str());
            autoTermUnref(it->second->getAttachCount());
            m_clients.erase(it);
            return;
        }
    }
    SE_LOG_DEBUG(NULL, NULL, "unknown client has disconnected?!");
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

void Server::connectCb(const GDBusCXX::Caller_t &caller,
                       const boost::shared_ptr<ConnectionResource> &resource,
                       const boost::shared_ptr<Client> &client,
                       const boost::shared_ptr<GDBusCXX::Result1<GDBusCXX::DBusObject_t> > &result)
{
    SE_LOG_DEBUG(NULL, NULL, "connecting D-Bus client %s with connection %s '%s'",
                 caller.c_str(),
                 resource->getPath(),
                 resource->m_description.c_str());

    client->attach(resource);
    addResource(resource, boost::bind(&resultDoneCb, resource, result));
}

void Server::connect(const Caller_t &caller,
                     const boost::shared_ptr<Watch> &watch,
                     const StringMap &peer,
                     bool must_authenticate,
                     const std::string &session,
                     const boost::shared_ptr<GDBusCXX::Result1<GDBusCXX::DBusObject_t> > &result)
{
    if (m_shutdownRequested) {
        // don't allow new sessions, we cannot activate them
        SE_THROW("server shutting down");
    }

    if (!session.empty()) {
        // reconnecting to old connection is not implemented yet
        throw std::runtime_error("not implemented");
    }
    std::string new_session = getNextSession();
    boost::shared_ptr<Client> client = addClient(caller, watch);

    ConnectionResource::createConnectionResource(boost::bind(&Server::connectCb, this, caller, _1, client, result),
                                                 *this,
                                                 new_session,
                                                 peer,
                                                 must_authenticate);
}

void Server::startSessionCb(const boost::shared_ptr<Client> &client,
                            const boost::shared_ptr<SessionResource> &resource,
                            const boost::shared_ptr<GDBusCXX::Result1<DBusObject_t> > &result)
{
    if (client && resource) {
        client->attach(resource);
        addResource(resource, boost::bind(&resultDoneCb, resource, result));
    } else if (result) {
      result->failed(GDBusCXX::dbus_error("org.Syncevolution.Server", "Ajwaj!"));
    }
}

void Server::startSessionWithFlags(const Caller_t &caller,
                                   const boost::shared_ptr<Watch> &watch,
                                   const std::string &server,
                                   const std::vector<std::string> &flags,
                                   const boost::shared_ptr<GDBusCXX::Result1<DBusObject_t> > &result)
{
    if (m_shutdownRequested) {
        // don't allow new sessions, we cannot activate them
        SE_THROW("server shutting down");
    }

    boost::shared_ptr<Client> client = addClient(caller, watch);
    std::string new_session = getNextSession();
    SessionResource::createSessionResource(boost::bind(&Server::startSessionCb, this, client, _1, result),
                                           *this,
                                           "is this a client or server session?", // TODO: what the heck?!
                                           server,
                                           new_session,
                                           flags);
}

void Server::checkPresence(const std::string &server,
                           std::string &status,
                           std::vector<std::string> &transports)
{
    return m_presence.checkPresence(server, status, transports);
}

void Server::getSessions(std::vector<DBusObject_t> &sessions)
{
    sessions.reserve(m_waitingResources.size() + m_activeResources.size());
    BOOST_FOREACH(const WeakResource_t &resource, m_activeResources) {
        boost::shared_ptr<SessionResource> session =
                    boost::dynamic_pointer_cast<SessionResource>(resource.lock());
        if (session) {
            sessions.push_back(session->getPath());
        }
    }

    BOOST_FOREACH(const WeakResource_t &resource, m_waitingResources) {
        boost::shared_ptr<SessionResource> session =
                    boost::dynamic_pointer_cast<SessionResource>(resource.lock());
        if (session) {
            sessions.push_back(session->getPath());
        }
    }
}

Server::Server(GMainLoop *loop,
               bool &shutdownRequested,
               boost::shared_ptr<Restart> &restart,
               const DBusConnectionPtr &conn,
               int duration) :
    DBusObjectHelper(conn,
                     SessionCommon::SERVER_PATH,
                     SessionCommon::SERVER_IFACE,
                     boost::bind(&Server::autoTermCallback, this)),
    m_loop(loop),
    m_shutdownRequested(shutdownRequested),
    m_restart(restart),
    m_lastSession(time(NULL)),
    m_lastInfoReq(0),
    m_bluezManager(new BluezManager(*this)),
    sessionChanged(*this, "SessionChanged"),
    presence(*this, "Presence"),
    templatesChanged(*this, "TemplatesChanged"),
    configChanged(*this, "ConfigChanged"),
    infoRequest(*this, "InfoRequest"),
    logOutput(*this, "LogOutput"),
    m_presence(*this),
    m_connman(*this),
    m_networkManager(*this),
    m_autoSync(*this),
    m_autoTerm(m_loop, m_shutdownRequested, m_autoSync.preventTerm() ? -1 : duration), //if there is any task in auto sync, prevent auto termination
    m_parentLogger(LoggerBase::instance())
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
    add(logOutput);

    LoggerBase::pushLogger(this);
    setLevel(LoggerBase::DEBUG);

    // Assume that Bluetooth is available. Neither ConnMan nor Network
    // manager can tell us about that. The "Bluetooth" ConnMan technology
    // is about IP connection via Bluetooth - not what we need.
    getPresenceStatus().updatePresenceStatus(true, PresenceStatus::BT_TRANSPORT);

    if (!m_connman.isAvailable() &&
        !m_networkManager.isAvailable()) {
        // assume that we are online if no network manager was found at all
        getPresenceStatus().updatePresenceStatus(true, PresenceStatus::HTTP_TRANSPORT);
    }
}

Server::~Server()
{
    // make sure all other objects are gone before destructing ourselves
    m_activeResources.clear();
    m_waitingResources.clear();

    m_clients.clear();
    LoggerBase::popLogger();
}

bool Server::shutdown()
{
    // Let the sessions know the server is shutting down.
    BOOST_FOREACH(const WeakResource_t &resource, m_activeResources) {
        boost::shared_ptr<SessionResource> session = boost::dynamic_pointer_cast<SessionResource>(resource.lock());
        if (session) {
            session->serverShutdown();
        }
    }

    Timespec now = Timespec::monotonic();
    bool autosync = m_autoSync.hasTask() || m_autoSync.hasAutoConfigs();
    SE_LOG_DEBUG(NULL, NULL, "shut down server at %lu.%09lu because of file modifications, auto sync %s",
                 now.tv_sec, now.tv_nsec, autosync ? "on" : "off");
    if (autosync) {
        // suitable exec() call which restarts the server using the same environment it was in
        // when it was started
        m_restart->restart();
    } else {
        // leave server now
        g_main_loop_quit(m_loop);
        SE_LOG_INFO(NULL, NULL, "server shutting down because files loaded into memory were modified on disk");
    }

    return false;
}

void Server::fileModified()
{
    SE_LOG_DEBUG(NULL, NULL, "file modified, %s shutdown: %s, %s",
                 m_shutdownRequested ? "continuing" : "initiating",
                 m_shutdownTimer ? "timer already active" : "timer not yet active",
                 m_activeResources.empty() ? "setting timer" : "waiting for active resource to finish");

    if (m_activeResources.empty()) {
        m_shutdownTimer.activate(SessionCommon::SHUTDOWN_QUIESCENCE_SECONDS,
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
    SE_LOG_DEBUG(NULL, NULL, "D-Bus server ready to run, versions:");
    BOOST_FOREACH(const StringPair &entry, map) {
        SE_LOG_DEBUG(NULL, NULL, "%s: %s", entry.first.c_str(), entry.second.c_str());
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
            SE_LOG_DEBUG(NULL, NULL, "watching: %s", file.c_str());
            boost::shared_ptr<SyncEvo::GLibNotify> notify(new GLibNotify(file.c_str(), boost::bind(&Server::fileModified, this)));
            m_files.push_back(notify);
        } catch (...) {
            // ignore errors for indidividual files
            Exception::handle();
        }
    }

    if (!m_shutdownRequested)
    {
        g_main_loop_run(m_loop);
    }

    SE_LOG_INFO(NULL, NULL, "%s", "Exiting Server::run");
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

void Server::addResource(const Resource_t &resource, const boost::function<void()> &callback)
{
    if (m_shutdownRequested) {
        // don't allow new resources, we cannot activate them
        // TODO: throwing this exception kills syncevo-dbus-server because
        // this code gets called in a helper thread; need to fix that
        // SE_THROW("server shutting down");
    }
    m_waitingResources.insert(resource);
    checkQueue(callback);
}

void Server::setActiveCb(const boost::shared_ptr<SessionResource> &session,
                         boost::shared_ptr<int> &counter,
                         const boost::function <void()> &callback)
{
    sessionChanged(session->getPath(), true);
    CounterCb(counter, callback);
}

void Server::checkQueue(const boost::function<void()> &callback)
{
    // Iterate over the wait queue...
    boost::shared_ptr<int> counter(new int(1));
    ResourceWaitQueue_t::iterator wq_iter;
    for (wq_iter = m_waitingResources.begin(); wq_iter != m_waitingResources.end();) {
        Resource_t waitingResource = wq_iter->lock();
        // ...removing any empty weak pointers...
        if (!waitingResource) {
            m_waitingResources.erase(wq_iter++);
        } else {
            bool canRun = true;
            WeakResources_t::iterator rl_iter;
            // ...then iterate over the active resources...
            for (rl_iter = m_activeResources.begin(); rl_iter != m_activeResources.end();) {
                Resource_t activeResource(rl_iter->lock());
                // ...removing any empty weak pointers again...
                if (!activeResource) {
                    rl_iter = m_activeResources.erase(rl_iter);
                } else if (canRun) {
                    // ...to see if there are any resources that can't be run concurrently.
                    if (!waitingResource->canRunConcurrently(activeResource)) {
                        canRun = false;
                        // (We are not breaking the loop here, because we want to cleanup
                        // list from empty weak pointers.)
                    }
                    ++rl_iter;
                } else {
                    ++rl_iter;
                }
            }
            // If not we activate the resource and place it in the active resource list.
            if (canRun) {
                m_activeResources.push_back(waitingResource);
                // If this is a session, we set active and emit sessionChanged to clients.
                boost::shared_ptr<SessionResource> session =
                    boost::dynamic_pointer_cast<SessionResource>(wq_iter->lock());
                if (session) {
                    ++(*counter);
                    session->setActiveAsync(true, boost::bind(&Server::setActiveCb, this, session, counter, callback));
                }
                m_waitingResources.erase(wq_iter++);
            } else {
                ++wq_iter;
            }
        }
    }
    // this will run a callback if setActiveAsync is never called.
    CounterCb(counter, callback);
}

void Server::killSessionsCb(const boost::shared_ptr<SessionResource> &session,
                            boost::shared_ptr<int> &counter,
                            const boost::function<void()> &callback)
{
    ++(*counter);
    removeResource(session, boost::bind(CounterCb, counter, callback));
    CounterCb(counter, callback);
}

void Server::killSessions(const std::string &peerDeviceID, const boost::function<void()> &callback)
{
    // Check waiting resources.
    ResourceWaitQueue_t::iterator wq_iter = m_waitingResources.begin();
    while (wq_iter != m_waitingResources.end()) {
        // boost::dynamic_pointer_cast returns null-pointer if cast is not allowed.
        boost::shared_ptr<SessionResource> session =
            boost::dynamic_pointer_cast<SessionResource>(wq_iter->lock());
        if (session && session->getPeerDeviceID() == peerDeviceID) {
            SE_LOG_DEBUG(NULL, NULL, "removing pending session %s because it matches deviceID %s",
                         session->getSessionID().c_str(), peerDeviceID.c_str());
            m_waitingResources.erase(wq_iter++);
        } else {
            ++wq_iter;
        }
    }

    boost::shared_ptr<int> counter(new int(1));

    // Check active resources.
    WeakResources_t::iterator ar_iter = m_activeResources.begin();
    while (ar_iter != m_activeResources.end()) {
        // boost::dynamic_pointer_cast returns null-pointer if cast is not allowed.
        boost::shared_ptr<SessionResource> session = boost::dynamic_pointer_cast<SessionResource>(ar_iter->lock());

        ++ar_iter;
        if (session && session->getPeerDeviceID() == peerDeviceID) {
            SE_LOG_DEBUG(NULL, NULL, "aborting active session %s because it matches deviceID %s",
                         session->getSessionID().c_str(),
                         peerDeviceID.c_str());
            try {
                // abort, even if not necessary right now
                ++(*counter);
                session->abortAsync(boost::bind(&Server::killSessionsCb,
                                                this,
                                                session,
                                                counter,
                                                callback));
            } catch (...) {
                // TODO: catch only that exception which indicates
                // incorrect use of the function
            }
        }
    }
    CounterCb(counter, callback);
}

void Server::removeResource(const Resource_t& resource, const boost::function<void()> &callback)
{
    if (resource) {
        bool found(false);

        // See if the session is among the active sessions.
        for (WeakResources_t::iterator it = m_activeResources.begin(); it != m_activeResources.end(); ++it) {
            Resource_t shared_resource(it->lock());
            // We can only remove from the active resources if the resource is not running.
            if (shared_resource && shared_resource == resource) {
                if (!shared_resource->getIsRunning()) {
                    boost::shared_ptr<SessionResource> session =
                        boost::dynamic_pointer_cast<SessionResource>(shared_resource);
                    if (session) {
                        // Signal end of session.
                        sessionChanged(session->getPath(), false);
                    }
                    m_activeResources.erase(it);
                }
                found = true;
                break;
            }
        }
        if (!found) {
            // If not in active resources, check the waiting resources.
            for (ResourceWaitQueue_t::iterator it = m_waitingResources.begin();
                 it != m_waitingResources.end();
                 ++it) {
                Resource_t res = it->lock();
                if (res == resource) {
                    boost::shared_ptr<SessionResource> session =
                        boost::dynamic_pointer_cast<SessionResource>(res);
                    if (session) {
                        // Signal end of session.
                        sessionChanged(session->getPath(), false);
                    }
                    // remove from queue
                    m_waitingResources.erase(it);
                    // session was idle, so nothing else to do
                    break;
                }
            }
        }
    }
    if (m_shutdownRequested) {
        callback();
        // Don't allow any of the pending resources to run. When the
        // last client disconnects the server shuts down.
        if (m_activeResources.empty()) {
            shutdown();
        }
    } else {
        checkQueue(callback);
    }
}

bool Server::sessionExpired(const boost::shared_ptr<SessionResource> &session)
{
    SE_LOG_DEBUG(NULL, NULL, "session %s expired",
                 session->getSessionID().c_str());
    // don't call me again
    return false;
}

void Server::delaySessionDestruction(const boost::shared_ptr<SessionResource> &session)
{
    SE_LOG_DEBUG(NULL, NULL, "delaying destruction of session %s by one minute",
                 session->getSessionID().c_str());
    addTimeout(boost::bind(&Server::sessionExpired,
                           session),
               60 /* 1 minute */);
}

bool Server::callTimeout(const boost::shared_ptr<Timeout> &timeout, const boost::function<bool ()> &callback)
{
    if (!callback()) {
        m_timeouts.remove(timeout);
        return false;
    } else {
        return true;
    }
}

void Server::addTimeout(const boost::function<bool ()> &callback,
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
        boost::shared_ptr<InfoReq> infoReq = it->second.lock();
        infoReq->setResponse(caller, state, response);
    }
}

boost::shared_ptr<InfoReq> Server::createInfoReq(const string &type,
                                                 const std::map<string, string> &parameters,
                                                 const SessionResource *sessionResource)
{
    boost::shared_ptr<InfoReq> infoReq(new InfoReq(*this, type, parameters, sessionResource->getPath()));
    boost::weak_ptr<InfoReq> item(infoReq);
    m_infoReqMap.insert(pair<string, boost::weak_ptr<InfoReq> >(infoReq->getId(), item));
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

void Server::removeInfoReq(const InfoReq &req)
{
    // Remove InfoRequest from hash map.
    InfoReqMap::iterator it = m_infoReqMap.find(req.getId());
    if (it != m_infoReqMap.end()) {
        m_infoReqMap.erase(it);
    }
}

void Server::getDeviceList(SyncConfig::DeviceList &devices)
{
    // Wait for bluez or other device managers.
    while(!m_bluezManager->isDone()) {
        g_main_loop_run(m_loop);
    }

    devices.clear();
    devices = m_syncDevices;
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

void Server::messagev(Level level,
                      const char *prefix,
                      const char *file,
                      int line,
                      const char *function,
                      const char *format,
                      va_list args)
{
    // iterating over args in messagev() is destructive, must make a copy first
    va_list argsCopy;
    va_copy(argsCopy, args);
    m_parentLogger.messagev(level, prefix, file, line, function, format, args);
    string log = StringPrintfV(format, argsCopy);
    va_end(argsCopy);

    // prefix is used to set session path
    // for general server output, the object path field is dbus server
    // the object path can't be empty for object paths prevent using empty string.
    string strLevel = Logger::levelToStr(level);
    logOutput(getPath(), strLevel, log);
}

SE_END_CXX
