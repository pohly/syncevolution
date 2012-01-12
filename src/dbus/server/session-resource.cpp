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

void SessionResource::startShutdown()
{
    return;
}

void SessionResource::shutdownFileModified()
{
    return;
}

bool SessionResource::shutdownServer()
{
    Timespec now = Timespec::monotonic();
    bool autosync = m_server.getAutoSyncManager().hasTask() ||
        m_server.getAutoSyncManager().hasAutoConfigs();
    SE_LOG_DEBUG(NULL, NULL, "shut down server at %lu.%09lu because of file modifications, auto sync %s",
                 now.tv_sec, now.tv_nsec,
                 autosync ? "on" : "off");
    if (autosync) {
        // suitable exec() call which restarts the server using the same environment it was in
        // when it was started
        m_server.m_restart->restart();
    } else {
        // leave server now
        m_server.m_shutdownRequested = true;
        g_main_loop_quit(m_server.getLoop());
        SE_LOG_INFO(NULL, NULL, "server shutting down because files loaded into memory were modified on disk");
    }

    return false;
}

bool SessionResource::readyToRun()
{
    return true;
}

void SessionResource::restore(const string &dir, bool before, const std::vector<std::string> &sources)
{
    return;
}

void SessionResource::execute(const vector<string> &args, const map<string, string> &vars)
{
    return;
}

void SessionResource::checkPresence (string &status)
{
    return;
}

bool SessionResource::getActive()
{
    return true;
}

void SessionResource::setConfig(bool update, bool temporary, const ReadOperations::Config_t &config)
{
    return;
}

void SessionResource::setNamedConfig(const std::string &configName, bool update, bool temporary,
                             const ReadOperations::Config_t &config)
{
    return;
}

void SessionResource::sync(const std::string &mode, const SessionCommon::SourceModes_t &source_modes)
{
    return;
}

void SessionResource::abort()
{
    m_sessionProxy->m_abort(boost::bind(&SessionResource::abortCb, this, _1));
}

void SessionResource::abortCb(const string &error)
{
    SE_LOG_INFO(NULL, NULL, "Session.Abort returned: error=%s",
                error.empty() ? "None" : error.c_str());
}

void SessionResource::suspend()
{
    m_sessionProxy->m_suspend(boost::bind(&SessionResource::suspendCb, this, _1));
}

void SessionResource::suspendCb(const string &error)
{
    SE_LOG_INFO(NULL, NULL, "Session.Suspend returned: error=%s",
                error.empty() ? "None" : error.c_str());
}

void SessionResource::getStatus(std::string &status, uint32_t &error, SessionCommon::SourceStatuses_t &sources)
{
    return;
}

void SessionResource::getProgress(int32_t &progress, SessionCommon::SourceProgresses_t &sources)
{
    return;
}

void SessionResource::fireStatus(bool flush)
{
    std::string status;
    uint32_t error;
    SessionCommon::SourceStatuses_t sources;

    /** not force flushing and not timeout, return */
    if(!flush && !m_statusTimer.timeout()) {
        return;
    }
    m_statusTimer.reset();

    getStatus(status, error, sources);
    emitStatus(status, error, sources);
}

void SessionResource::fireProgress(bool flush)
{
    int32_t progress;
    SessionCommon::SourceProgresses_t sources;

    /** not force flushing and not timeout, return */
    if(!flush && !m_progressTimer.timeout()) {
        return;
    }
    m_progressTimer.reset();

    getProgress(progress, sources);
    emitProgress(progress, sources);
}

void SessionResource::getConfig(bool getTemplate, ReadOperations::Config_t &config)
{
    getNamedConfig(m_configName, getTemplate, config);
    return;
}

void SessionResource::getNamedConfig(const std::string &configName, bool getTemplate,
                                     ReadOperations::Config_t &config)
{
    m_sessionProxy->m_getNamedConfig(configName, getTemplate,
                                     boost::bind(&SessionResource::getNamedConfigCb, this, _1, _2));
}

void SessionResource::getNamedConfigCb(const ReadOperations::Config_t &config, const string &error)
{
    string configAsStr("");
    BOOST_FOREACH(ReadOperations::Config_t::value_type configItem, config)
    {
        configAsStr.append(configItem.first + ":\n");

        BOOST_FOREACH(StringMap::value_type &configItemKeyValue, configItem.second)
        {
            configAsStr.append(string("\t") + configItemKeyValue.first  + ": "
                                            + configItemKeyValue.second + "\n");
        }
    }

    SE_LOG_INFO(NULL, NULL, "Session.GetNamedCb returned: config = %s; error=%s", configAsStr.c_str(),
                error.empty() ? "None" : error.c_str());
}

void SessionResource::getReports(uint32_t start, uint32_t count, ReadOperations::Reports_t &reports)
{
    return;
}

void SessionResource::checkSource(const string &sourceName)
{
    return;
}

void SessionResource::getDatabases(const string &sourceName, ReadOperations::SourceDatabases_t &databases)
{
    return;
}

SessionListener* SessionResource::addListener(SessionListener *listener)
{
    return listener;
}

void SessionResource::statusChangedCb(const std::string &status, uint32_t error,
                                      const SessionCommon::SourceStatuses_t &sources)
{
    SE_LOG_INFO(NULL, NULL, "Session.StatusChanged signal received: status=%s", status.c_str());
    return;
}

void SessionResource::progressChangedCb(int32_t error, const SessionCommon::SourceProgresses_t &sources)
{
    SE_LOG_INFO(NULL, NULL, "Session.ProgressChanged signal received: error=%d", error);
    return;
}

void SessionResource::onSessionConnect(const GDBusCXX::DBusConnectionPtr &conn)
{
    SE_LOG_INFO(NULL, NULL, "SessionProxy interface end with: %d", m_forkExecParent->getChildPid());
    m_sessionProxy.reset(new SessionProxy(conn, boost::lexical_cast<string>(m_forkExecParent->getChildPid())));

    /* Enable public dbus interface for Session. */
    activate();

    SE_LOG_INFO(NULL, NULL, "onSessionConnect called in session-resource (path: %s interface: %s)",
                m_sessionProxy->getPath(), m_sessionProxy->getInterface());

    SE_LOG_INFO(NULL, NULL, "Session connection made.");
}

void SessionResource::onQuit(int status)
{
    SE_LOG_INFO(NULL, NULL, "dbus-helper quit with status: %d", status);
}

void SessionResource::onFailure(const std::string &error)
{
    SE_LOG_INFO(NULL, NULL, "dbus-helper failed with error: %s", error.c_str());
}

boost::shared_ptr<SessionResource> SessionResource::createSessionResource(Server &server,
                                                                          const std::string &peerDeviceID,
                                                                          const std::string &config_name,
                                                                          const std::string &session,
                                                                          const std::vector<std::string> &flags)
{
    boost::shared_ptr<SessionResource> me(new SessionResource(server, peerDeviceID,
                                                              config_name, session, flags));
    me->m_me = me;
    return me;
}

SessionResource::SessionResource(Server &server,
                                 const std::string &peerDeviceID,
                                 const std::string &configName,
                                 const std::string &session,
                                 const std::vector<std::string> &flags) :
    DBusObjectHelper(server.getConnection(),
                     std::string("/org/syncevolution/Session/") + session,
                     "org.syncevolution.Session",
                     boost::bind(&Server::autoTermCallback, &server)),
    m_server(server),
    m_flags(flags),
    m_sessionID(session),
    m_peerDeviceID(peerDeviceID),
    m_path(std::string("/org/syncevolution/Session/") + session),
    m_configName(configName),
    m_replyTotal(0),
    m_replyCounter(0),
    m_forkExecParent(SyncEvo::ForkExecParent::create("syncevo-dbus-helper")),
    emitStatus(*this, "StatusChanged"),
    emitProgress(*this, "ProgressChanged")
{
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
    add(this, &SessionResource::checkPresence, "checkPresence");
    add(this, &SessionResource::execute, "Execute");
    add(emitStatus);
    add(emitProgress);

    SE_LOG_INFO(NULL, NULL, "SessionResource (%s) forking...", getPath());

    m_forkExecParent->m_onConnect.connect(boost::bind(&SessionResource::onSessionConnect, this, _1));
    m_forkExecParent->m_onQuit.connect(boost::bind(&SessionResource::onQuit, this, _1));
    m_forkExecParent->m_onFailure.connect(boost::bind(&SessionResource::onFailure, this, _2));
    m_forkExecParent->start();

    SE_LOG_DEBUG(NULL, NULL, "session resource %s created", getPath());
}

void SessionResource::replyInc()
{
    // increase counter and check whether all replies are returned
    m_replyCounter++;
    if(methodInvocationDone()) {
        g_main_loop_quit(getLoop());
    }
}

void SessionResource::waitForReply()
{
    while(!methodInvocationDone()) {
        g_main_loop_run(getLoop());
    }
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
    m_server.removeSession(this);

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
