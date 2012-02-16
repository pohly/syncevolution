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
    std::string str_error;

    genericCall(m_sessionProxy->m_serverShutdown,
                m_sessionProxy->m_serverShutdown.bindGeneric(&str_error),
                str_error);
}

void SessionResource::setActive(bool active)
{
    std::string str_error;

    genericCall(m_sessionProxy->m_setActive,
                m_sessionProxy->m_setActive.bindGeneric(&str_error),
                active,
                str_error);

    m_active = active;
}

void SessionResource::restore(const string &dir, bool before, const std::vector<std::string> &sources)
{
    std::string str_error;

    genericCall(m_sessionProxy->m_restore,
                m_sessionProxy->m_restore.bindGeneric(&str_error),
                dir,
                before,
                sources,
                str_error);
}

void SessionResource::checkPresence(std::string &status)
{
    vector<string> transport;
    m_server.checkPresence(m_configName, status, transport);
}

void SessionResource::execute(const vector<string> &args, const map<string, string> &vars)
{
    std::string str_error;

    genericCall(m_sessionProxy->m_execute,
                m_sessionProxy->m_execute.bindGeneric(&str_error),
                args,
                vars,
                str_error);
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
    std::string str_error;

    genericCall(m_sessionProxy->m_passwordResponse,
                m_sessionProxy->m_passwordResponse.bindGeneric(&str_error),
                false,
                password,
                str_error);

    SE_LOG_INFO(NULL, NULL, "SessionResource::onPasswordResponse: Finished waiting for password response. Password%s received", str_error.empty() ? "" : " not");
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

void SessionResource::init()
{
    SE_LOG_INFO(NULL, NULL, "SessionResource (%s) forking...", getPath());

    m_forkExecParent->m_onConnect.connect(boost::bind(&SessionResource::onSessionConnect, this, _1));
    m_forkExecParent->m_onQuit.connect(boost::bind(&SessionResource::onQuit, this, _1));
    m_forkExecParent->m_onFailure.connect(boost::bind(&SessionResource::onFailure, this, _2));
    m_forkExecParent->addEnvVar("SYNCEVO_SESSION_ID", m_sessionID);
    m_forkExecParent->addEnvVar("SYNCEVO_SESSION_CONFIG", m_configName);
    m_forkExecParent->start();

    // Wait for onSessionConnect to be called so that the dbus
    // interface is ready to be used.
    resetReplies();
    waitForReply();
}

void SessionResource::setNamedConfig(const std::string &configName, bool update, bool temporary,
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

    std::string str_error;

    genericCall(m_sessionProxy->m_setNamedConfig,
                m_sessionProxy->m_setNamedConfig.bindGeneric(&m_setConfig, &str_error),
                configName,
                update,
                temporary,
                config,
                str_error);

    SE_LOG_INFO(NULL, NULL, "m_setConfig = %d", (int)m_setConfig);
}

void SessionResource::sync(const std::string &mode, const SessionCommon::SourceModes_t &source_modes)
{
    std::string str_error;

    genericCall(m_sessionProxy->m_sync,
                m_sessionProxy->m_sync.bindGeneric(&str_error),
                mode,
                source_modes,
                str_error);
}

void SessionResource::abort()
{
    std::string str_error;

    genericCall(m_sessionProxy->m_abort,
                m_sessionProxy->m_abort.bindGeneric(&str_error),
                str_error);
}

void SessionResource::suspend()
{
    std::string str_error;

    genericCall(m_sessionProxy->m_suspend,
                m_sessionProxy->m_suspend.bindGeneric(&str_error),
                str_error);
}

void SessionResource::getStatus(std::string &status, uint32_t &error,
                                SessionCommon::SourceStatuses_t &sources)
{
    std::string str_error;

    genericCall(m_sessionProxy->m_getStatus,
                m_sessionProxy->m_getStatus.bindGeneric(&status, &error, &sources, &str_error),
                str_error);

    SE_LOG_INFO(NULL, NULL, "status=%s, error code=%d",
                status.c_str(), error);
}

void SessionResource::getProgress(int32_t &progress, SessionCommon::SourceProgresses_t &sources)
{
    std::string str_error;

    genericCall(m_sessionProxy->m_getProgress,
                m_sessionProxy->m_getProgress.bindGeneric(&progress, &sources, &str_error),
                str_error);

    SE_LOG_INFO(NULL, NULL, "Progress=%d", progress);
}

void SessionResource::getNamedConfig(const std::string &configName, bool getTemplate,
                                     ReadOperations::Config_t &config)
{
    std::string str_error;

    genericCall(m_sessionProxy->m_getNamedConfig,
                m_sessionProxy->m_getNamedConfig.bindGeneric(&config, &str_error),
                configName,
                getTemplate,
                str_error);
}

void SessionResource::getReports(uint32_t start, uint32_t count, ReadOperations::Reports_t &reports)
{
    std::string str_error;

    genericCall(m_sessionProxy->m_getReports,
                m_sessionProxy->m_getReports.bindGeneric(&reports, &str_error),
                start,
                count,
                str_error);
}

void SessionResource::checkSource(const string &sourceName)
{
    std::string str_error;

    genericCall(m_sessionProxy->m_checkSource,
                m_sessionProxy->m_checkSource.bindGeneric(&str_error),
                sourceName,
                str_error);
}

void SessionResource::getDatabases(const string &sourceName, ReadOperations::SourceDatabases_t &databases)
{
    std::string str_error;

    genericCall(m_sessionProxy->m_getDatabases,
                m_sessionProxy->m_getDatabases.bindGeneric(&databases, &str_error),
                sourceName,
                str_error);
}

SessionListener* SessionResource::addListener(SessionListener *listener)
{
    return listener;
}

void SessionResource::statusChangedCb(const std::string &status, uint32_t error,
                                      const SessionCommon::SourceStatuses_t &sources)
{
    SE_LOG_INFO(NULL, NULL, "Session.StatusChanged signal received and relayed: status=%s", status.c_str());

    // Keep track of weather this session is running.
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
    SE_LOG_INFO(NULL, NULL, "SessionProxy interface end with: %s", m_sessionID.c_str());
    m_sessionProxy.reset(new SessionProxy(conn, m_sessionID));

    /* Enable public dbus interface for Session. */
    activate();
    replyInc(); // Init is waiting on a reply.

    // Activate signal watch on helper signals.
    m_sessionProxy->m_statusChanged.activate  (boost::bind(&SessionResource::statusChangedCb,   this, _1, _2, _3));
    m_sessionProxy->m_progressChanged.activate(boost::bind(&SessionResource::progressChangedCb, this, _1, _2));
    m_sessionProxy->m_passwordRequest.activate(boost::bind(&SessionResource::requestPasswordCb, this, _1));
    m_sessionProxy->m_done.activate           (boost::bind(&SessionResource::done,              this));

    SE_LOG_INFO(NULL, NULL, "onSessionConnect called in session-resource (path: %s interface: %s)",
                m_sessionProxy->getPath(), m_sessionProxy->getInterface());

    SE_LOG_INFO(NULL, NULL, "Session connection made.");
}

void SessionResource::onQuit(int status)
{
    m_server.checkQueue();
    SE_LOG_INFO(NULL, NULL, "dbus-helper quit with status: %d", status);
}

void SessionResource::onFailure(const std::string &error)
{
    m_server.checkQueue();
    SE_LOG_INFO(NULL, NULL, "dbus-helper failed with error: %s", error.c_str());
}

boost::shared_ptr<SessionResource> SessionResource::createSessionResource(Server &server,
                                                                          const std::string &peerDeviceID,
                                                                          const std::string &configName,
                                                                          const std::string &session,
                                                                          const std::vector<std::string> &flags)
{
    boost::shared_ptr<SessionResource> me(new SessionResource(server, peerDeviceID,
                                                              configName, session, flags));
    me->init();
    me->m_me = me;
    return me;
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
    Resource(m_server, "Session"),
    m_flags(flags),
    m_sessionID(session),
    m_peerDeviceID(peerDeviceID),
    m_path(std::string(SessionCommon::SESSION_PATH) + "/" + session),
    m_configName(configName),
    m_setConfig(false),
    m_forkExecParent(SyncEvo::ForkExecParent::create("syncevo-dbus-helper")),
    m_done(false),
    m_active(false),
    emitStatus(*this, "StatusChanged"),
    emitProgress(*this, "ProgressChanged")
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
    m_server.removeResource(m_me.lock());

    // now tell other clients about config change?
    if (m_setConfig) {
        m_server.configChanged();
    }
}

SessionResource::~SessionResource()
{
    SE_LOG_DEBUG(NULL, NULL, "session resource %s deconstructing", getPath());
    done();
}

SE_END_CXX
