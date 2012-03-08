/*
 * Copyright (C) 2011 Intel Corporation
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

#ifndef SYNCEVO_DBUS_SERVER_H
#define SYNCEVO_DBUS_SERVER_H

#include <set>

#include <boost/weak_ptr.hpp>

#include "resource.h"
#include "auto-sync-manager.h"
#include "exceptions.h"
#include "auto-term.h"
#include "server-read-operations.h"
#include "connman-client.h"
#include "network-manager-client.h"
#include "presence-status.h"
#include "timeout.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

class SessionResource;
class Server;
class InfoReq;
class BluezManager;
class Timeout;
class Restart;
class Client;
class GLibNotify;
class ConnectionResource;
class SessionResource;

/**
 * Implements the main org.syncevolution.Server interface.
 *
 * The Server class is responsible for listening to clients and
 * spinning of sync sessions as requested by clients.
 */
class Server : public GDBusCXX::DBusObjectHelper,
               public LoggerBase
{
    GMainLoop *m_loop;
    bool &m_shutdownRequested;
    boost::shared_ptr<SyncEvo::Restart> &m_restart;

    uint32_t m_lastSession;
    typedef std::list< std::pair< boost::shared_ptr<GDBusCXX::Watch>, boost::shared_ptr<Client> > > Clients_t;
    Clients_t m_clients;

    /**
     * Watch all files mapped into our address space. When
     * modifications are seen (as during a package upgrade), queue a
     * high priority session. This prevents running other sessions,
     * which might not be able to execute correctly. For example, a
     * sync with libsynthesis from 1.1 does not work with
     * SyncEvolution XML files from 1.2. The dummy session then waits
     * for the changes to settle (see
     * SessionCommon::SHUTDOWN_QUIESENCE_SECONDS) and either shuts
     * down or restarts.  The latter is necessary if the daemon has
     * automatic syncing enabled in a config.
     */
    list< boost::shared_ptr<GLibNotify> > m_files;
    void fileModified();
    bool shutdown();

    /**
     * timer which counts seconds until server is meant to shut down
     */
    Timeout m_shutdownTimer;

    /**
     * Event source that regularly polls network manager
     */
    GLibEvent m_pollConnman;

    /** Define types for Resource containers. */
    typedef boost::shared_ptr<Resource> Resource_t;
    typedef boost::weak_ptr<Resource> WeakResource_t;
    typedef std::list<WeakResource_t> WeakResources_t;

    class PriorityCompare
    {
      public:
        bool operator() (const WeakResource_t &lhs, const WeakResource_t &rhs) const
        {
            boost::shared_ptr<Resource> lsr = lhs.lock();
            boost::shared_ptr<Resource> rsr = rhs.lock();
            if (lsr && rsr) {
                return lsr->getPriority() > rsr->getPriority();
            } else if (lsr) {
                return true;
            }
            return false;
        }
    };

    /**
     * A list of active Sessions & Connections.
     *
     * Resource objects are removed once the Session D-Bus interface
     * disappears.
     */
    WeakResources_t m_activeResources;

    /**
     * A std::set disguised as a priority queue. std::priority_queue
     * does not allow interating over its elements. But both allow for
     * setting a Compare class.
     */
    typedef std::set<WeakResource_t, PriorityCompare> ResourceWaitQueue_t;

    /**
     * The waiting Sessions and Connections.
     *
     * This is a set holding WeakResource_t instances priority-ordered
     * using the nested PriorityCompare class.
     */
    ResourceWaitQueue_t m_waitingResources;

    /**
     * A hash of pending InfoRequest
     */
    typedef std::map<string, boost::weak_ptr<InfoReq> > InfoReqMap;

    // hash map of pending info requests
    InfoReqMap m_infoReqMap;

    // the index of last info request
    uint32_t m_lastInfoReq;

    boost::shared_ptr<BluezManager> m_bluezManager;

    /** devices which have sync services */
    SyncConfig::DeviceList m_syncDevices;

    /**
     * Watch callback for a specific client or connection.
     */
    void clientGone(Client *c);

    /** Server.GetCapabilities() */
    vector<string> getCapabilities();

    /** Server.GetVersions() */
    StringMap getVersions();

    /** Server.Attach() */
    void attachClient(const GDBusCXX::Caller_t &caller,
                      const boost::shared_ptr<GDBusCXX::Watch> &watch);

    /** Server.Detach() */
    void detachClient(const GDBusCXX::Caller_t &caller);

    /** Server.DisableNotifications() */
    void disableNotifications(const GDBusCXX::Caller_t &caller,
                              const string &notifications) {
        setNotifications(false, caller, notifications);
    }

    /** Server.EnableNotifications() */
    void enableNotifications(const GDBusCXX::Caller_t &caller,
                             const string &notifications) {
        setNotifications(true, caller, notifications);
    }

    /** Server.NotificationAction() */
    void notificationAction(const GDBusCXX::Caller_t &caller) {
        pid_t pid;
        if((pid = fork()) == 0) {
            // search sync-ui from $PATH
            execlp("sync-ui", "sync-ui", (const char*)0);

            // Failing that, try meego-ux-settings/Sync
            execlp("meego-qml-launcher",
              "meego-qml-launcher",
              "--opengl", "--fullscreen", "--app", "meego-ux-settings",
              "--cmd", "showPage", "--cdata", "Sync", (const char*)0);

            // Failing that, simply exit
            exit(0);
        }
    }

    /** actual implementation of enable and disable */
    void setNotifications(bool enable,
                          const GDBusCXX::Caller_t &caller,
                          const string &notifications);

    /** Callbacks */
    void connectCb(const GDBusCXX::Caller_t &caller,
                   const boost::shared_ptr<ConnectionResource> &resource,
                   const boost::shared_ptr<Client> &client,
                   const boost::shared_ptr<GDBusCXX::Result1<GDBusCXX::DBusObject_t> > &result);

    void startSessionCb(const boost::shared_ptr<Client> &client,
                        const boost::shared_ptr<SessionResource> &resource,
                        const boost::shared_ptr<GDBusCXX::Result1<GDBusCXX::DBusObject_t> > &result);

    void killSessionsCb(const boost::shared_ptr<SessionResource> &session,
                        boost::shared_ptr<int> &counter,
                        const boost::function<void()> &callback);

    void setActiveCb(const boost::shared_ptr<SessionResource> &session,
                     boost::shared_ptr<int> &counter,
                     const boost::function <void()> &callback);

    /** Server.Connect() */
    void connect(const GDBusCXX::Caller_t &caller,
                 const boost::shared_ptr<GDBusCXX::Watch> &watch,
                 const StringMap &peer,
                 bool must_authenticate,
                 const std::string &session,
                 const boost::shared_ptr<GDBusCXX::Result1<GDBusCXX::DBusObject_t> > &result);

    /** Server.StartSession() */
    void startSession(const GDBusCXX::Caller_t &caller,
                      const boost::shared_ptr<GDBusCXX::Watch> &watch,
                      const std::string &server,
                      const boost::shared_ptr<GDBusCXX::Result1<GDBusCXX::DBusObject_t> > &result) {
        startSessionWithFlags(caller, watch, server, std::vector<std::string>(), result);
    }

    /** Server.StartSessionWithFlags() */
    void startSessionWithFlags(const GDBusCXX::Caller_t &caller,
                               const boost::shared_ptr<GDBusCXX::Watch> &watch,
                               const std::string &server,
                               const std::vector<std::string> &flags,
                               const boost::shared_ptr<GDBusCXX::Result1<GDBusCXX::DBusObject_t> > &result);

    /** Server.GetConfig() */
    void getConfig(const std::string &configName,
                   bool getTemplate,
                   ReadOperations::Config_t &config)
    {
        ReadOperations ops(configName);
        ops.getConfig(getTemplate , config);
    }

    /** Server.GetReports() */
    void getReports(const std::string &configName,
                    uint32_t start, uint32_t count,
                    ReadOperations::Reports_t &reports)
    {
        ReadOperations ops(configName);
        ops.getReports(start, count, reports);
    }

    /** Server.CheckSource() */
    void checkSource(const std::string &configName,
                     const std::string &sourceName)
    {
        ReadOperations ops(configName);
        ops.checkSource(sourceName);
    }

    /** Server.GetDatabases() */
    void getDatabases(const std::string &configName,
                      const string &sourceName,
                      ReadOperations::SourceDatabases_t &databases)
    {
        ReadOperations ops(configName);
        ops.getDatabases(sourceName, databases);
    }

    /** Server.GetConfigs() */
    void getConfigs(bool getTemplates,
                    std::vector<std::string> &configNames)
    {
        ServerReadOperations ops("", *this);
        ops.getConfigs(getTemplates, configNames);
    }

    /** Server.CheckPresence() */
    void checkPresence(const std::string &server,
                       std::string &status,
                       std::vector<std::string> &transports);

    /** Server.GetSessions() */
    void getSessions(std::vector<GDBusCXX::DBusObject_t> &sessions);

    /** Server.InfoResponse() */
    void infoResponse(const GDBusCXX::Caller_t &caller,
                      const std::string &id,
                      const std::string &state,
                      const std::map<string, string> &response);

    friend class InfoReq;

    /** emit InfoRequest */
    void emitInfoReq(const InfoReq &);

    /** get the next id of InfoRequest */
    std::string getNextInfoReq();

    /** remove InfoReq from hash map */
    void removeInfoReq(const InfoReq &req);

    /** Server.SessionChanged */
    GDBusCXX::EmitSignal2<const GDBusCXX::DBusObject_t &,
                bool> sessionChanged;

    /** Server.PresenceChanged */
    GDBusCXX::EmitSignal3<const std::string &,
                const std::string &,
                const std::string &> presence;

    /**
     * Server.TemplatesChanged, triggered each time m_syncDevices, the
     * input for the templates, is changed
     */
    GDBusCXX::EmitSignal0 templatesChanged;

    /**
     * Server.ConfigChanged, triggered each time a session ends
     * which modified its configuration
     */
    GDBusCXX::EmitSignal0 configChanged;

    /** Server.InfoRequest */
    GDBusCXX::EmitSignal6<const std::string &,
                          const GDBusCXX::DBusObject_t &,
                          const std::string &,
                          const std::string &,
                          const std::string &,
                          const std::map<string, string> &> infoRequest;

    /** Server.LogOutput */
    GDBusCXX::EmitSignal3<const GDBusCXX::DBusObject_t &,
                          string,
                          const std::string &> logOutput;

    friend class SessionResource;

    PresenceStatus m_presence;
    ConnmanClient m_connman;
    NetworkManagerClient m_networkManager;

    /** Manager to automatic sync */
    AutoSyncManager m_autoSync;

    // Automatic termination
    AutoTerm m_autoTerm;

    // Records the parent logger, dbus server acts as logger to
    // send signals to clients and put logs in the parent logger.
    LoggerBase &m_parentLogger;

    /**
     * All active timeouts created by addTimeout().
     * Each timeout which requests to be not called
     * again will be removed from this list.
     */
    list< boost::shared_ptr<Timeout> > m_timeouts;

    /**
     * called each time a timeout triggers,
     * removes those which are done
     */
    bool callTimeout(const boost::shared_ptr<Timeout> &timeout, const boost::function<bool ()> &callback);

    /** Called 1 minute after last client detached from a session. */
    static bool sessionExpired(const boost::shared_ptr<SessionResource> &session);

public:
    Server(GMainLoop *loop,
           bool &shutdownRequested,
           boost::shared_ptr<Restart> &restart,
           const GDBusCXX::DBusConnectionPtr &conn,
           int duration);
    ~Server();

    /** access to the GMainLoop reference used by this Server instance */
    GMainLoop *getLoop() { return m_loop; }

    /** process D-Bus calls until the server is ready to quit */
    void run();

    /**
     * look up client by its ID
     */
    boost::shared_ptr<Client> findClient(const GDBusCXX::Caller_t &ID);

    /**
     * find client by its ID or create one anew
     */
    boost::shared_ptr<Client> addClient(const GDBusCXX::Caller_t &ID,
                                        const boost::shared_ptr<GDBusCXX::Watch> &watch);

    /** detach this resource from all clients which own it */
    void detach(Resource *resource);

    /**
     * Add a session. Might also make it ready immediately, if no
     * sessions with conflicting sources are active. To be called by
     * the creator of the session, *after* the session is ready to
     * run.
     */
    void addResource(const Resource_t &session,
                     const boost::function<void()> &callback);

    /**
     * Remove all sessions with this device ID from the
     * queue. If the active session also has this ID,
     * the session will be aborted and/or deactivated.
     */
    void killSessions(const std::string &peerDeviceID,
                      const boost::function<void()> &callback);

    /**
     * Remove a resource from the list of active resources. If it is
     * running a sync, it will keep running and nothing will
     * change. Otherwise, if it is "ready" (= holds a lock on its
     * configuration), then release that lock.
     */
    void removeResource(const Resource_t &resource,
                        const boost::function<void()> &callback);

    /**
     * Checks whether the server is ready to run another resource and
     * if so, activates the first one in the queue.
     */
    void checkQueue(const boost::function<void()> &callback);

    /**
     * Special behavior for sessions: keep them around for another
     * minute after the are no longer needed. Must be called by the
     * creator of the session right before it would normally cause the
     * destruction of the session.
     *
     * This allows another client to attach and/or get information
     * about the session.
     *
     * This is implemented as a timeout which holds a reference to the
     * session. Once the timeout fires, it is called and then removed,
     * which removes the reference.
     */
    void delaySessionDestruction(const boost::shared_ptr<SessionResource> &session);

    /**
     * Invokes the given callback once in the given amount of seconds.
     * Keeps a copy of the callback. If the Server is destructed
     * before that time, then the callback will be deleted without
     * being called.
     */
    void addTimeout(const boost::function<bool ()> &callback,
                    int seconds);

    boost::shared_ptr<InfoReq> createInfoReq(const string &type,
                                             const std::map<string, string> &parameters,
                                             const SessionResource *session);
    void autoTermRef(int counts = 1) { m_autoTerm.ref(counts); }

    void autoTermUnref(int counts = 1) { m_autoTerm.unref(counts); }

    /** callback to reset for auto termination checking */
    void autoTermCallback() { m_autoTerm.reset(); }

    /** poll_nm callback for connman, used for presence detection*/
    void connmanCallback(const std::map <std::string, boost::variant <std::vector <std::string> > >& props, const string &error);

    PresenceStatus& getPresenceStatus() {return m_presence;}

    /**
     * methods to operate device list. See DeviceList definition.
     * The device id here is the identifier of device, the same as  definition in DeviceList.
     * In bluetooth devices, it refers to actually the mac address of the bluetooth.
     * The finger print and match mode is used to match templates.
     */
    /** get sync devices */
    void getDeviceList(SyncConfig::DeviceList &devices);
    /** get a device according to device id. If not found, return false. */
    bool getDevice(const string &deviceId, SyncConfig::DeviceDescription &device);
    /** add a device */
    void addDevice(const SyncConfig::DeviceDescription &device);
    /** remove a device by device id. If not found, do nothing */
    void removeDevice(const string &deviceId);
    /** update a device with the given device information. If not found, do nothing */
    void updateDevice(const string &deviceId, const SyncConfig::DeviceDescription &device);

    /* emit a presence signal */
    void emitPresence(const string &server, const string &status, const string &transport)
    {
        presence(server, status, transport);
    }

    /**
     * Returns new unique session ID. Implemented with a running
     * counter. Checks for overflow, but not currently for active
     * sessions.
     */
    std::string getNextSession();

    AutoSyncManager &getAutoSyncManager() { return m_autoSync; }

    /**
     * false if any client requested suppression of notifications
     */
    bool notificationsEnabled();

    /**
     * implement virtual method from LogStdout.
     * Not only print the message in the console
     * but also send them as signals to clients
     */
    virtual void messagev(Level level,
                          const char *prefix,
                          const char *file,
                          int line,
                          const char *function,
                          const char *format,
                          va_list args);

    virtual bool isProcessSafe() const { return false; }
};

SE_END_CXX

#endif // SYNCEVO_DBUS_SERVER_H
