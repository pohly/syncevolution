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

#include "dbus-sync.h"
#include "session-helper.h"
#include "dbus-transport-agent.h"

#include <syncevo/SyncSource.h>
#include <syncevo/SuspendFlags.h>

SE_BEGIN_CXX

DBusSync::DBusSync(const SessionCommon::SyncParams &params,
                   SessionHelper &helper) :
    SyncContext(params.m_config, true),
    m_helper(helper),
    m_params(params),
    m_pwResponseStatus(PW_RES_IDLE),
    m_waiting(false)
{
    setUserInterface(this);

    setServerAlerted(params.m_serverAlerted);
    if (params.m_serverMode) {
        initServer(params.m_sessionID,
                   params.m_initialMessage,
                   params.m_initialMessageType);
    }

    if (params.m_remoteInitiated) {
        setRemoteInitiated(true);
    }

    // Apply temporary config filters. The parameters of this function
    // override the source filters, if set.
    setConfigFilter(true, "", params.m_syncFilter);
    FilterConfigNode::ConfigFilter filter;
    filter = params.m_sourceFilter;
    if (!params.m_mode.empty()) {
        filter["sync"] = params.m_mode;
    }
    setConfigFilter(false, "", filter);
    BOOST_FOREACH(const std::string &source,
                  getSyncSources()) {
        SessionCommon::SourceFilters_t::const_iterator fit = params.m_sourceFilters.find(source);
        filter = fit == params.m_sourceFilters.end() ?
            FilterConfigNode::ConfigFilter() :
            fit->second;
        SessionCommon::SourceModes_t::const_iterator it = params.m_sourceModes.find(source);
        if (it != params.m_sourceModes.end()) {
            filter["sync"] = it->second;
        }
        setConfigFilter(false, source, filter);
    }

    // Create source status and progress entries for each source in
    // the parent. See Session::sourceProgress().
    BOOST_FOREACH(const std::string source,
                  getSyncSources()) {
        m_helper.emitSourceProgress(sysync::PEV_PREPARING,
                                    source,
                                    SYNC_NONE,
                                    0, 0, 0);
    }
}

boost::shared_ptr<TransportAgent> DBusSync::createTransportAgent()
{
    if (m_params.m_serverAlerted || m_params.m_serverMode) {
        // Use the D-Bus Connection to send and receive messages.
        boost::shared_ptr<DBusTransportAgent> agent(new DBusTransportAgent(m_helper));

        // Hook up agent with D-Bus in the helper. The agent may go
        // away at any time, so use instance tracking.
        m_helper.m_messageSignal.connect(SessionHelper::MessageSignal_t::slot_type(&DBusTransportAgent::storeMessage,
                                                                                   agent.get(),
                                                                                   _1,
                                                                                   _2).track(agent));
        m_helper.m_connectionStateSignal.connect(SessionHelper::ConnectionStateSignal_t::slot_type(&DBusTransportAgent::storeState,
                                                                                                   agent.get(),
                                                                                                   _1).track(agent));

        if (m_params.m_serverAlerted) {
            // A SAN message was sent to us, need to reply.
            agent->serverAlerted();
        } else if (m_params.m_serverMode) {
            // Let transport return initial message to engine.
            agent->storeMessage(GDBusCXX::DBusArray<uint8_t>(m_params.m_initialMessage.size(),
                                                             reinterpret_cast<const uint8_t *>(m_params.m_initialMessage.get())),
                                m_params.m_initialMessageType);
        }

        return agent;
    } else {
        // no connection, use HTTP via libsoup/GMainLoop
        GMainLoop *loop = m_helper.getLoop();
        boost::shared_ptr<TransportAgent> agent = SyncContext::createTransportAgent(loop);
        return agent;
    }
}

void DBusSync::displaySyncProgress(sysync::TProgressEventEnum type,
                                   int32_t extra1, int32_t extra2, int32_t extra3)
{
    SyncContext::displaySyncProgress(type, extra1, extra2, extra3);
    m_helper.emitSyncProgress(type, extra1, extra2, extra3);
}

void DBusSync::displaySourceProgress(sysync::TProgressEventEnum type,
                                     SyncSource &source,
                                     int32_t extra1, int32_t extra2, int32_t extra3)
{
    SyncContext::displaySourceProgress(type, source, extra1, extra2, extra3);
    m_helper.emitSourceProgress(type, source.getName(), source.getFinalSyncMode(),
                                extra1, extra2, extra3);
}

void DBusSync::reportStepCmd(sysync::uInt16 stepCmd)
{
    switch(stepCmd) {
    case sysync::STEPCMD_SENDDATA:
    case sysync::STEPCMD_RESENDDATA:
    case sysync::STEPCMD_NEEDDATA:
        // sending or waiting
        if (!m_waiting) {
            m_helper.emitWaiting(true);
            m_waiting = true;
        }
        break;
    default:
        // otherwise, processing
        if (m_waiting) {
            m_helper.emitWaiting(false);
            m_waiting = false;
        }
        break;
    }
}

void DBusSync::syncSuccessStart()
{
    m_helper.emitSyncSuccessStart();
}

string DBusSync::askPassword(const string &passwordName,
                             const string &descr,
                             const ConfigPasswordKey &key)
{
    string password;
    if (GetLoadPasswordSignal()(passwordName, descr, key, password)) {
        // handled
        return password;
    }

    SE_LOG_DEBUG(NULL, NULL, "asking parent for password");

    m_pwResponseStatus = PW_RES_WAITING;
    m_helper.emitPasswordRequest(descr, key);

    // Wait till we've got a response from our password request or a
    // shutdown signal is recieved.
    // TODO: detect dead parent and abort
    SuspendFlags &s = SuspendFlags::getSuspendFlags();
    while (m_pwResponseStatus == PW_RES_WAITING) {
        if (s.getState() != SuspendFlags::NORMAL) {
            // return prematurely
            return "";
        }
        g_main_context_iteration(g_main_context_default(), true);
    }

    // Save the response state and reset status to idle.
    PwRespStatus respStatus = m_pwResponseStatus;
    m_pwResponseStatus = PW_RES_IDLE;

    // Check status and take apropriate action.
    if (respStatus == PW_RES_OK) {
        password = m_pwResponse;
    } else if (respStatus == PW_RES_TIMEOUT) {
        SE_THROW_EXCEPTION_STATUS(StatusException,
                                  "Can't get the password from user. The password request has timed out.",
                                  STATUS_PASSWORD_TIMEOUT);
    } else {
        SE_THROW_EXCEPTION_STATUS(StatusException, "user didn't provide password, abort",
                                  SyncMLStatus(sysync::LOCERR_USERABORT));
    }

    return password;
}

void DBusSync::passwordResponse(bool timedOut, bool aborted, const std::string &password)
{
    SE_LOG_DEBUG(NULL, NULL, "received password response");
    m_pwResponse.clear();
    if (!timedOut) {
        if (aborted) {
            m_pwResponseStatus = PW_RES_INVALID;
        } else {
            m_pwResponseStatus = PW_RES_OK;
            m_pwResponse = password;
        }
    } else {
        m_pwResponseStatus = PW_RES_TIMEOUT;
    }
}

bool DBusSync::savePassword(const std::string &passwordName, const std::string &password, const ConfigPasswordKey &key)
{
    if (GetSavePasswordSignal()(passwordName, password, key)) {
        return true;
    }

    // not saved
    return false;
}

void DBusSync::readStdin(std::string &content)
{
    // might get called, must be avoided by user
    SE_THROW("reading from stdin not supported when running with daemon, use --daemon=no");
}


SE_END_CXX
