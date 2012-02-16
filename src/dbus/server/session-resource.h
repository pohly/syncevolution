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

#ifndef SESSION_RESOURCE_H
#define SESSION_RESOURCE_H

#include "session-common.h"
#include "resource.h"
#include "server.h"

#include <syncevo/ForkExec.h>
#include <boost/lexical_cast.hpp>

SE_BEGIN_CXX

class SessionProxy : public GDBusCXX::DBusRemoteObject
{
public:
  SessionProxy(const GDBusCXX::DBusConnectionPtr &conn, const std::string &session) :
    GDBusCXX::DBusRemoteObject(conn.get(),
                               "/dbushelper",
                               std::string("dbushelper.Session") + session,
                               "direct.peer",
                               true), // This is a one-to-one connection. Close it.
         m_getNamedConfig   (*this, "GetNamedConfig"),
         m_setNamedConfig   (*this, "SetNamedConfig"),
         m_getReports       (*this, "GetReports"),
         m_checkSource      (*this, "CheckSource"),
         m_getDatabases     (*this, "GetDatabases"),
         m_sync             (*this, "Sync"),
         m_abort            (*this, "Abort"),
         m_suspend          (*this, "Suspend"),
         m_getStatus        (*this, "GetStatus"),
         m_getProgress      (*this, "GetProgress"),
         m_restore          (*this, "Restore"),
         m_execute          (*this, "Execute"),
         m_serverShutdown   (*this, "ServerShutdown"),
         m_passwordResponse (*this, "PasswordResponse"),
         m_setActive        (*this, "SetActive"),
         m_statusChanged    (*this, "StatusChanged", false),
         m_progressChanged  (*this, "ProgressChanged", false),
         m_passwordRequest  (*this, "PasswordRequest", false),
         m_done             (*this, "Done", false)
    {}

    GDBusCXX::DBusClientCall1<ReadOperations::Config_t>          m_getNamedConfig;
    GDBusCXX::DBusClientCall1<bool>                              m_setNamedConfig;
    GDBusCXX::DBusClientCall1<std::vector<StringMap> >           m_getReports;
    GDBusCXX::DBusClientCall0                                    m_checkSource;
    GDBusCXX::DBusClientCall1<ReadOperations::SourceDatabases_t> m_getDatabases;
    GDBusCXX::DBusClientCall0                                    m_sync;
    GDBusCXX::DBusClientCall0                                    m_abort;
    GDBusCXX::DBusClientCall0                                    m_suspend;
    GDBusCXX::DBusClientCall3<std::string, uint32_t,
                              SessionCommon::SourceStatuses_t>   m_getStatus;
    GDBusCXX::DBusClientCall2<int32_t,
                              SessionCommon::SourceProgresses_t> m_getProgress;
    GDBusCXX::DBusClientCall0                                    m_restore;
    GDBusCXX::DBusClientCall0                                    m_execute;
    GDBusCXX::DBusClientCall0                                    m_serverShutdown;
    GDBusCXX::DBusClientCall0                                    m_passwordResponse;
    GDBusCXX::DBusClientCall0                                    m_setActive;
    GDBusCXX::SignalWatch3<std::string, uint32_t,
                           SessionCommon::SourceStatuses_t>      m_statusChanged;
    GDBusCXX::SignalWatch2<int32_t,
                           SessionCommon::SourceProgresses_t>    m_progressChanged;
    GDBusCXX::SignalWatch1<std::map<std::string, std::string> >  m_passwordRequest;
    GDBusCXX::SignalWatch0                                       m_done;
};

/**
 * Handles supplying the session info needed by the server and
 * clients.
 */
class SessionResource : public GDBusCXX::DBusObjectHelper,
                        public Resource,
                        private boost::noncopyable
{
    bool autoSyncManagerHasTask()        { return m_server.getAutoSyncManager().hasTask(); }
    bool autoSyncManagerHasAutoConfigs() { return m_server.getAutoSyncManager().hasAutoConfigs(); }

    /** access to the GMainLoop reference used by this Session instance */
    GMainLoop *getLoop() { return m_server.getLoop(); }

    std::vector<std::string> m_flags;
    const std::string m_sessionID;
    std::string m_peerDeviceID;
    const std::string m_path;

    const std::string m_configName;
    bool m_setConfig;

    boost::shared_ptr<SyncEvo::ForkExecParent> m_forkExecParent;
    boost::scoped_ptr<SessionProxy> m_sessionProxy;

    // Child session handlers
    void onSessionConnect(const GDBusCXX::DBusConnectionPtr &conn);
    void onQuit(int status);
    void onFailure(const std::string &error);

    /**
     * True once done() was called.
     */
    bool m_done;
    bool m_active;

    /** Session.Attach() */
    void attach(const GDBusCXX::Caller_t &caller);

    /** Session.Detach() */
    void detach(const GDBusCXX::Caller_t &caller);

    /** Session.GetStatus() */
    void getStatus(std::string &status, uint32_t &error, SessionCommon::SourceStatuses_t &sources);

    /** Session.GetProgress() */
    void getProgress(int32_t &progress, SessionCommon::SourceProgresses_t &sources);

    /** Session.Restore() */
    void restore(const std::string &dir, bool before, const std::vector<std::string> &sources);

    /** Session.checkPresence() */
    void checkPresence(std::string &status);

    /** Session.Execute() */
    void execute(const vector<std::string> &args, const map<std::string, std::string> &vars);

    /** Session.StatusChanged */
    GDBusCXX::EmitSignal3<const std::string &,
                          uint32_t,
                          const SessionCommon::SourceStatuses_t &> emitStatus;
    /** Session.ProgressChanged */
    GDBusCXX::EmitSignal2<int32_t,
                          const SessionCommon::SourceProgresses_t &> emitProgress;

    /** Session.GetConfig() */
    void getConfig(bool getTemplate, ReadOperations::Config_t &config)
    { getNamedConfig(m_configName, getTemplate, config); }

    /** Session.GetConfigs() == Server.GetConfigs*/
    void getConfigs(bool getTemplates, std::vector<std::string> &configNames)
    { m_server.getConfigs(getTemplates, configNames); }

    /** Session.GetNamedConfig() */
    void getNamedConfig(const std::string &configName, bool getTemplate,
                        ReadOperations::Config_t &config);

    /** Session.GetReports() */
    void getReports(uint32_t start, uint32_t count, ReadOperations::Reports_t &reports);

    /** Session.CheckSource() */
    void checkSource(const std::string &sourceName);

    /** Session.GetDatabases() */
    void getDatabases(const std::string &sourceName, ReadOperations::SourceDatabases_t &databases);

    /* Callbacks for signals fired from helper */
    void statusChangedCb(const std::string &status, uint32_t error,
                         const SessionCommon::SourceStatuses_t &sources);
    void progressChangedCb(int32_t error, const SessionCommon::SourceProgresses_t &sources);

    // Callback for password request signal.
    void requestPasswordCb(const std::map<std::string, std::string> & params);
    // Callback for InfoReq's response signal.
    void onPasswordResponse(boost::shared_ptr<InfoReq> infoReq);

public:
    /**
     * Session resources must always be held in a shared pointer
     * because some operations depend on that. This constructor
     * function here ensures that and also adds a weak pointer to the
     * instance itself, so that it can create more shared pointers as
     * needed.
     */
    static boost::shared_ptr<SessionResource> createSessionResource(Server &server,
                                                                    const std::string &peerDeviceID,
                                                                    const std::string &config_name,
                                                                    const std::string &session,
                                                                    const std::vector<std::string> &flags =
                                                                          std::vector<std::string>());

    /**
     * automatically marks the session as completed before deleting it
     */
    ~SessionResource();

    /** explicitly mark the session as completed, even if it doesn't get deleted yet */
    void done();

    /**
     * Initialize the session: Activate interface, connect to helper,
     * wait for helper to connect.
     */
    void init();

private:
    SessionResource(Server &server,
                    const std::string &peerDeviceID,
                    const std::string &config_name,
                    const std::string &session,
                    const std::vector<std::string> &flags = std::vector<std::string>());
    boost::weak_ptr<SessionResource> m_me;

public:

    std::string getConfigName() { return m_configName; }
    std::string getSessionID() const { return m_sessionID; }
    std::string getPeerDeviceID() const { return m_peerDeviceID; }

    bool getActive();

    /** Session.GetFlags() */
    std::vector<std::string> getFlags() { return m_flags; }

    /** Session.GetConfigName() */
    std::string getNormalConfigName() { return SyncConfig::normalizeConfigString(m_configName); }

    /** Session.SetConfig() */
    void setConfig(bool update, bool temporary, const ReadOperations::Config_t &config)
    { setNamedConfig(m_configName, update, temporary, config); }

    /** Session.SetNamedConfig() */
    void setNamedConfig(const std::string &configName, bool update, bool temporary,
                        const ReadOperations::Config_t &config);

    typedef StringMap SourceModes_t;
    /** Session.Sync() */
    void sync(const std::string &mode, const SourceModes_t &source_modes);

    /** Session.Abort() */
    void abort();

    /** Session.Suspend() */
    void suspend();

    // Called when server is shutting down.
    void serverShutdown();

    void setActive(bool active);

    /**
     * add a listener of the session. Old set listener is returned
     */
    SessionListener* addListener(SessionListener *listener);
};

SE_END_CXX

#endif // SESSION_RESOURCE_H
