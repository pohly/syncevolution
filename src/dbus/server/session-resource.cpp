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

#include "session-resource.h"
#include "client.h"
#include "restart.h"
#include "info-req.h"
#include "session-common.h"
#include "dbus-proxy.h"
#include "dbus-callbacks.h"

#include <memory>

#include <boost/foreach.hpp>

SE_BEGIN_CXX

void SessionResource::attach(const GDBusCXX::Caller_t &caller)
{
    boost::shared_ptr<Client> client(m_server.findClient(caller));
    if (!client) {
        throw runtime_error("unknown client");
    }
    boost::shared_ptr<SessionResource> me = m_me.lock();
    if (!me) {
        throw runtime_error("session resource already deleted?!");
    }
    client->attach(me);
}

void SessionResource::detach(const GDBusCXX::Caller_t &caller)
{
    boost::shared_ptr<Client> client(m_server.findClient(caller));
    if (!client) {
        throw runtime_error("unknown client");
    }
    client->detach(this);
}

void SessionResource::serverShutdown()
{
    m_sessionProxy->m_serverShutdown.start(boost::bind(&Resource::printStatus,
                                                       _1,
                                                       m_resourceName,
                                                       m_sessionProxy->m_serverShutdown.getMethod()));
}

void SessionResource::setActiveAsyncCb(bool active,
                                       const std::string &error,
                                       const boost::function<void()> &callback)
{
    if (error.empty()) {
        m_active = active;
        SE_LOG_DEBUG(NULL, NULL, "m_active = %s", m_active ? "yes" : "no");
        callback();
    } else {
        SE_LOG_ERROR(NULL, NULL, "setActiveAsync failed: %s", error.c_str());
    }
}


void SessionResource::setActiveAsync(bool active, const boost::function<void ()> &callback)
{
    m_sessionProxy->m_setActive.start(active, boost::bind(&SessionResource::setActiveAsyncCb,
                                                          this,
                                                          active,
                                                          _1,
                                                          callback));
}

void SessionResource::restore(const string &dir, bool before, const std::vector<std::string> &sources,
                              const boost::shared_ptr<GDBusCXX::Result0> &result)
{
    ProxyCallback0 callback(result);

    defaultConnectToBoth(callback, m_sessionProxy->m_restore.getMethod());
    m_sessionProxy->m_restore.start(dir, before, sources, callback);
}

void SessionResource::checkPresence(std::string &status)
{
    vector<string> transport;
    m_server.checkPresence(m_configName, status, transport);
}

void SessionResource::execute(const vector<string> &args, const map<string, string> &vars,
                              const boost::shared_ptr<GDBusCXX::Result0> &result)
{
    ProxyCallback0 callback(result);

    defaultConnectToBoth(callback, m_sessionProxy->m_execute.getMethod());
    m_sessionProxy->m_execute.start(args, vars, callback);
}

void SessionResource::onPasswordResponse(boost::shared_ptr<InfoReq> infoReq)
{
    std::string password = "";
    std::map<string, string> response;
    if(infoReq->getResponse(response)) {
        std::map<string, string>::const_iterator it = response.find("password");
        if (it != response.end()) {
            password = it->second;
        }
    }

    SE_LOG_INFO(NULL, NULL, "SessionResource::onPasswordResponse: Waiting for password response");

    m_sessionProxy->m_passwordResponse.start(false, password, boost::bind(&Resource::printStatus,
                                                                          _1,
                                                                          m_resourceName,
                                                                          m_sessionProxy->m_passwordResponse.getMethod()));
}

void SessionResource::requestPasswordCb(const std::map<std::string, std::string> & params)
{
    boost::shared_ptr<InfoReq> req = m_server.createInfoReq("password", params, this);
    req->m_onResponse.connect(boost::bind(&SessionResource::onPasswordResponse, this, req));

    SE_LOG_INFO(NULL, NULL, "SessionResource::requestPasswordCb: req->m_onResponse.connect");
}

bool SessionResource::getActive()
{
    return m_active;
}

void SessionResource::init(const Callback_t &callback)
{
    SE_LOG_INFO(NULL, NULL, "SessionResource (%s) forking...", getPath());

    m_forkExecParent->m_onReady.connect(boost::bind(&SessionResource::onSessionReady, this, callback));
    m_forkExecParent->m_onConnect.connect(boost::bind(&SessionResource::onSessionConnect, this, _1));
    m_forkExecParent->m_onQuit.connect(boost::bind(&SessionResource::onQuit, this, _1));
    m_forkExecParent->m_onFailure.connect(boost::bind(&SessionResource::onFailure, this, _2));
    m_forkExecParent->addEnvVar("SYNCEVO_SESSION_ID", m_sessionID);
    m_forkExecParent->addEnvVar("SYNCEVO_SESSION_CONFIG", m_configName);
    m_forkExecParent->start();
}

void SessionResource::setNamedConfigCb(bool setConfig)
{
    m_setConfig = setConfig;
    SE_LOG_INFO(NULL, NULL, "m_setConfig = %d", (int)m_setConfig);
}

void SessionResource::setNamedConfigCommon(const std::string &configName, bool temporary,
                                           const ReadOperations::Config_t &config)
{
    // avoid the check if effect is the same as setConfig()
    if (m_configName != configName) {
        bool found = false;
        BOOST_FOREACH(const std::string &flag, m_flags) {
            if (boost::iequals(flag, "all-configs")) {
                found = true;
                break;
            }
        }
        if (!found) {
            SE_THROW_EXCEPTION(InvalidCall,
                               "SetNameConfig() only allowed in 'all-configs' sessions");
        }

        if (temporary) {
            SE_THROW_EXCEPTION(InvalidCall,
                               "SetNameConfig() with temporary config change only supported for config named when starting the session");
        }
    }

    m_server.getPresenceStatus().updateConfigPeers (configName, config);
}

void SessionResource::setNamedConfigAsyncCb(bool setConfig,
                                            const std::string &error,
                                            const boost::function<void()> &callback)
{
    if (error.empty()) {
        m_setConfig = setConfig;
        SE_LOG_INFO(NULL, NULL, "m_setConfig = %d", (int)m_setConfig);
        callback();
    } else {
        SE_LOG_ERROR(NULL, NULL, "setNamedConfigAsync failed: %s", error.c_str());
    }
}

void SessionResource::setNamedConfigAsync(const std::string &configName, bool update, bool temporary,
                                          const ReadOperations::Config_t &config,
                                          const boost::function<void()> &callback)
{
    setNamedConfigCommon(configName, temporary, config);

    m_sessionProxy->m_setNamedConfig.start(configName, update, temporary, config,
                                           boost::bind(&SessionResource::setNamedConfigAsyncCb,
                                                       this,
                                                       _1,
                                                       _2,
                                                       callback));
}

void SessionResource::setNamedConfig(const std::string &configName, bool update, bool temporary,
                                     const ReadOperations::Config_t &config,
                                     const boost::shared_ptr<GDBusCXX::Result1<bool> > &result)
{
    setNamedConfigCommon(configName, temporary, config);

    typedef ProxyCallback1<bool> Callback_t;
    Callback_t callback(result);

    callback.m_success->connect(Callback_t::SuccessSignalType::slot_type(&SessionResource::setNamedConfigCb, this, _1).track(m_me));
    defaultConnectToFailure(callback, m_sessionProxy->m_setNamedConfig.getMethod());
    m_sessionProxy->m_setNamedConfig.start(configName, update, temporary, config, callback);
}

void SessionResource::syncAsync(const std::string &mode,
                                const SessionCommon::SourceModes_t &source_modes,
                                const boost::function<void()> &callback)
{
    m_sessionProxy->m_sync.start(mode, source_modes, boost::bind(&Resource::printStatusWithCallback,
                                                                 _1,
                                                                 m_resourceName,
                                                                 m_sessionProxy->m_sync.getMethod(),
                                                                 callback));
}

void SessionResource::sync(const std::string &mode, const SessionCommon::SourceModes_t &source_modes,
                           const boost::shared_ptr<GDBusCXX::Result0> &result)
{
    ProxyCallback0 callback(result);

    defaultConnectToBoth(callback, m_sessionProxy->m_sync.getMethod());
    m_sessionProxy->m_sync.start(mode, source_modes, callback);
}

void SessionResource::abortAsync(const boost::function<void ()> &callback)
{
    m_sessionProxy->m_abort.start(boost::bind(&Resource::printStatusWithCallback,
                                              _1,
                                              m_resourceName,
                                              m_sessionProxy->m_abort.getMethod(),
                                              callback));
}

void SessionResource::abort(const boost::shared_ptr<GDBusCXX::Result0> &result)
{
    ProxyCallback0 callback(result);

    defaultConnectToBoth(callback, m_sessionProxy->m_abort.getMethod());
    m_sessionProxy->m_abort.start(callback);
}

void SessionResource::suspend(const boost::shared_ptr<GDBusCXX::Result0> &result)
{
    ProxyCallback0 callback(result);

    defaultConnectToBoth(callback, m_sessionProxy->m_suspend.getMethod());
    m_sessionProxy->m_suspend.start(callback);
}

// static
void SessionResource::getStatusCb(const std::string &status, const uint32_t &error)
{
    SE_LOG_INFO(NULL, NULL, "status=%s, error code=%d",
                status.c_str(), error);

}

void SessionResource::getStatus(const boost::shared_ptr<GDBusCXX::Result3<std::string, uint32_t, SessionCommon::SourceStatuses_t> >&result)
{
    typedef ProxyCallback3<std::string, uint32_t, SessionCommon::SourceStatuses_t> Callback_t;
    Callback_t callback(result);

    callback.m_success->connect(Callback_t::SuccessSignalType::slot_type(&SessionResource::getStatusCb, _1, _2));
    defaultConnectToFailure(callback, m_sessionProxy->m_getStatus.getMethod());
    m_sessionProxy->m_getStatus.start(callback);

}
// static
void SessionResource::getProgressCb(const int32_t &progress)
{
    SE_LOG_INFO(NULL, NULL, "Progress=%d", progress);
}

void SessionResource::getProgress(const boost::shared_ptr<GDBusCXX::Result2<int32_t, SessionCommon::SourceProgresses_t> > &result)
{
    typedef ProxyCallback2<int32_t, SessionCommon::SourceProgresses_t> Callback_t;
    Callback_t callback(result);

    callback.m_success->connect(Callback_t::SuccessSignalType::slot_type(&getProgressCb, _1));
    defaultConnectToFailure(callback, m_sessionProxy->m_getProgress.getMethod());
    m_sessionProxy->m_getProgress.start(callback);
}

void SessionResource::getNamedConfig(const std::string &configName, bool getTemplate,
                                     const boost::shared_ptr<GDBusCXX::Result1<ReadOperations::Config_t> > &result)
{
    ProxyCallback1<ReadOperations::Config_t> callback(result);

    defaultConnectToBoth(callback, m_sessionProxy->m_getNamedConfig.getMethod());
    m_sessionProxy->m_getNamedConfig.start(configName, getTemplate, callback);
}

void SessionResource::getReports(uint32_t start, uint32_t count,
                                 const boost::shared_ptr<GDBusCXX::Result1<ReadOperations::Reports_t> > &result)
{
    ProxyCallback1<ReadOperations::Reports_t> callback(result);

    defaultConnectToBoth(callback, m_sessionProxy->m_getReports.getMethod());
    m_sessionProxy->m_getReports.start(start, count, callback);
}

void SessionResource::checkSource(const string &sourceName,
                                  const boost::shared_ptr<GDBusCXX::Result0> &result)
{
    ProxyCallback0 callback(result);

    defaultConnectToBoth(callback, m_sessionProxy->m_checkSource.getMethod());
    m_sessionProxy->m_checkSource.start(sourceName, callback);
}

void SessionResource::getDatabases(const string &sourceName,
                                   const boost::shared_ptr<GDBusCXX::Result1<ReadOperations::SourceDatabases_t> > &result)
{
    ProxyCallback1<ReadOperations::SourceDatabases_t> callback(result);

    defaultConnectToBoth(callback, m_sessionProxy->m_getDatabases.getMethod());
    m_sessionProxy->m_getDatabases.start(sourceName, callback);
}

SessionListener* SessionResource::addListener(SessionListener *listener)
{
    return listener;
}

void SessionResource::statusChangedCb(const std::string &status, uint32_t error,
                                      const SessionCommon::SourceStatuses_t &sources)
{
    SE_LOG_INFO(NULL, NULL, "Session.StatusChanged signal received and relayed: status=%s", status.c_str());

    // Keep track of whether this session is running.
    if(status.find("running") != std::string::npos) {
        m_isRunning = true;
    } else {
        m_isRunning = false;
    }

    // Relay signal to client.
    emitStatus(status, error, sources);
}

void SessionResource::progressChangedCb(int32_t error, const SessionCommon::SourceProgresses_t &sources)
{
    SE_LOG_INFO(NULL, NULL, "Session.ProgressChanged signal received and relayed: error=%d", error);
    // Relay signal to client.
    emitProgress(error, sources);
}

void SessionResource::onSessionConnect(const GDBusCXX::DBusConnectionPtr &conn)
{
    m_helper_conn = conn;
}

void SessionResource::onSessionReady(const Callback_t &callback)
{
    SE_LOG_INFO(NULL, NULL, "SessionProxy interface end with: %s", m_sessionID.c_str());
    m_sessionProxy.reset(new SessionProxy(m_helper_conn, m_sessionID));

    /* Enable public dbus interface for Session. */
    activate();

    // Activate signal watch on helper signals.
    m_sessionProxy->m_statusChanged.activate  (boost::bind(&SessionResource::statusChangedCb,   this, _1, _2, _3));
    m_sessionProxy->m_progressChanged.activate(boost::bind(&SessionResource::progressChangedCb, this, _1, _2));
    m_sessionProxy->m_passwordRequest.activate(boost::bind(&SessionResource::requestPasswordCb, this, _1));
    m_sessionProxy->m_done.activate           (boost::bind(&SessionResource::done,              this));

    SE_LOG_INFO(NULL, NULL, "onSessionConnect called in session-resource (path: %s interface: %s)",
                m_sessionProxy->getPath(), m_sessionProxy->getInterface());

    SE_LOG_INFO(NULL, NULL, "Session connection made.");
    boost::shared_ptr<SessionResource> me(this);
    m_me = me;
    // if callback owner won't copy this shared pointer
    // then session resource will be destroyed.
    callback(me);
}

void SessionResource::onQuit(int status)
{
    m_server.checkQueue(boost::function<void()>(&nullCb));
    SE_LOG_INFO(NULL, NULL, "dbus-helper quit with status: %d", status);
}

void SessionResource::onFailure(const std::string &error)
{
    m_server.checkQueue(boost::function<void()>(&nullCb));
    SE_LOG_INFO(NULL, NULL, "dbus-helper failed with error: %s", error.c_str());
}

void SessionResource::createSessionResource(const Callback_t &callback,
                                            Server &server,
                                            const std::string &peerDeviceID,
                                            const std::string &configName,
                                            const std::string &session,
                                            const std::vector<std::string> &flags)
{
    std::auto_ptr<SessionResource> resource(new SessionResource(server,
                                                                peerDeviceID,
                                                                configName,
                                                                session,
                                                                flags));

    resource->init(callback);
    // init did not throw any exception, so we guess that child was spawned successfully.
    // thus we release the auto_ptr, so it will not delete the resource.
    resource.release();
}

SessionResource::SessionResource(Server &server,
                                 const std::string &peerDeviceID,
                                 const std::string &configName,
                                 const std::string &session,
                                 const std::vector<std::string> &flags) :
    DBusObjectHelper(server.getConnection(),
                     std::string(SessionCommon::SESSION_PATH) + "/" + session,
                     SessionCommon::SESSION_IFACE,
                     boost::bind(&Server::autoTermCallback, &server)),
    Resource(server, "Session"),
    m_flags(flags),
    m_sessionID(session),
    m_peerDeviceID(peerDeviceID),
    m_path(std::string(SessionCommon::SESSION_PATH) + "/" + session),
    m_configName(configName),
    m_setConfig(false),
    m_forkExecParent(SyncEvo::ForkExecParent::create("syncevo-dbus-helper")),
    m_sessionProxy(),
    m_done(false),
    m_active(false),
    emitStatus(*this, "StatusChanged"),
    emitProgress(*this, "ProgressChanged"),
    m_me()
{
    m_priority = Resource::PRI_DEFAULT;
    m_isRunning = false;

    add(this, &SessionResource::attach, "Attach");
    add(this, &SessionResource::detach, "Detach");
    add(this, &SessionResource::getFlags, "GetFlags");
    add(this, &SessionResource::getNormalConfigName, "GetConfigName");
    add(this, &SessionResource::getConfigs, "GetConfigs");
    add(this, &SessionResource::getConfig, "GetConfig");
    add(this, &SessionResource::getNamedConfig, "GetNamedConfig");
    add(this, &SessionResource::setConfig, "SetConfig");
    add(this, &SessionResource::setNamedConfig, "SetNamedConfig");
    add(this, &SessionResource::getReports, "GetReports");
    add(this, &SessionResource::checkSource, "CheckSource");
    add(this, &SessionResource::getDatabases, "GetDatabases");
    add(this, &SessionResource::sync, "Sync");
    add(this, &SessionResource::abort, "Abort");
    add(this, &SessionResource::suspend, "Suspend");
    add(this, &SessionResource::getStatus, "GetStatus");
    add(this, &SessionResource::getProgress, "GetProgress");
    add(this, &SessionResource::restore, "Restore");
    add(this, &SessionResource::checkPresence, "CheckPresence");
    add(this, &SessionResource::execute, "Execute");
    add(emitStatus);
    add(emitProgress);

    SE_LOG_DEBUG(NULL, NULL, "session resource %s created", getPath());
}

void SessionResource::done()
{
    if (m_done) {
        return;
    }
    SE_LOG_DEBUG(NULL, NULL, "session %s done", getPath());

    /* update auto sync manager when a config is changed */
    if (m_setConfig) {
        m_server.getAutoSyncManager().update(m_configName);
    }
    m_server.removeResource(m_me.lock(), boost::function<void ()>(&nullCb));

    // now tell other clients about config change?
    if (m_setConfig) {
        m_server.configChanged();
    }

    m_done = true;
}

SessionResource::~SessionResource()
{
    SE_LOG_DEBUG(NULL, NULL, "session resource %s deconstructing", getPath());
}

SE_END_CXX
