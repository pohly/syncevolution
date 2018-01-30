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
#include <syncevo/ForkExec.h>

SE_BEGIN_CXX

DBusSync::DBusSync(const SessionCommon::SyncParams &params,
                   SessionHelper &helper) :
    SyncContext(params.m_config, true),
    m_helper(helper),
    m_params(params),
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

    // Watch status of parent and our own process and cancel
    // any pending password request if parent or we go down.
    auto forkexec = m_helper.getForkExecChild();
    if (forkexec) {
        m_parentWatch = forkexec->m_onQuit.connect([this] () { passwordResponse(true, false, ""); });
    }
    m_suspendFlagsWatch = SuspendFlags::getSuspendFlags().m_stateChanged.connect([this] (SuspendFlags &flags) {
            if (flags.getState() != SuspendFlags::NORMAL) {
                passwordResponse(true, false, "");
            }
        });

    // Apply temporary config filters. The parameters of this function
    // override the source filters, if set.
    setConfigFilter(true, "", params.m_syncFilter);
    FilterConfigNode::ConfigFilter filter;
    filter = params.m_sourceFilter;
    if (!params.m_mode.empty()) {
        if (params.m_mode == "ephemeral") {
            makeEphemeral();
        } else if (params.m_mode == "pbap") {
            // "pbap" may only be used by caller when it knows that
            // the mode is safe to use.
            makeEphemeral();
            const char *sync = getenv("SYNCEVOLUTION_PBAP_SYNC");
            if (!sync) {
                SE_LOG_DEBUG(NULL, "enabling default SYNCEVOLUTION_PBAP_SYNC=incremental");
                setenv("SYNCEVOLUTION_PBAP_SYNC", "incremental", true);
            } else {
                SE_LOG_DEBUG(NULL, "using SYNCEVOLUTION_PBAP_SYNC=%s from environment", sync);
            }
        } else {
            filter["sync"] = params.m_mode;
        }
    }
    setConfigFilter(false, "", filter);
    for (const std::string &source:
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
    for (const std::string &source:
                  getSyncSources()) {
        m_helper.emitSourceProgress(sysync::PEV_PREPARING,
                                    source,
                                    SYNC_NONE,
                                    0, 0, 0);
    }

    // Forward the SourceSyncedSignal via D-Bus.
    m_sourceSyncedSignal.connect([this] (const std::string &name, const SyncSourceReport &source) { m_helper.emitSourceSynced(name, source); });
}

DBusSync::~DBusSync()
{
    m_parentWatch.disconnect();
    m_suspendFlagsWatch.disconnect();
}

std::shared_ptr<TransportAgent> DBusSync::createTransportAgent()
{
    if (m_params.m_serverAlerted || m_params.m_serverMode) {
        // Use the D-Bus Connection to send and receive messages.
        auto agent = std::make_shared<DBusTransportAgent>(m_helper);

        // Hook up agent with D-Bus in the helper. The agent may go
        // away at any time, so use instance tracking.
        m_helper.m_messageSignal.connect(SessionHelper::MessageSignal_t::slot_type(&DBusTransportAgent::storeMessage,
                                                                                   agent.get(),
                                                                                   _1,
                                                                                   _2).track_foreign(agent));
        m_helper.m_connectionStateSignal.connect(SessionHelper::ConnectionStateSignal_t::slot_type(&DBusTransportAgent::storeState,
                                                                                                   agent.get(),
                                                                                                   _1).track_foreign(agent));

        if (m_params.m_serverAlerted) {
            // A SAN message was sent to us, need to reply.
            agent->serverAlerted();
        } else if (m_params.m_serverMode) {
            // Let transport return initial message to engine.
            agent->storeMessage(GDBusCXX::DBusArray<uint8_t>(m_params.m_initialMessage.size(),
                                                             reinterpret_cast<const uint8_t *>(m_params.m_initialMessage.get())),
                                m_params.m_initialMessageType);
        }

        return std::static_pointer_cast<TransportAgent>(agent);
    } else {
        // no connection, use HTTP via libsoup/GMainLoop
        GMainLoop *loop = m_helper.getLoop();
        auto agent = SyncContext::createTransportAgent(loop);
        return agent;
    }
}

void DBusSync::displaySyncProgress(sysync::TProgressEventEnum type,
                                   int32_t extra1, int32_t extra2, int32_t extra3)
{
    SyncContext::displaySyncProgress(type, extra1, extra2, extra3);
    m_helper.emitSyncProgress(type, extra1, extra2, extra3);
}

bool DBusSync::displaySourceProgress(SyncSource &source,
                                     const SyncSourceEvent &event,
                                     bool flush)
{
    bool cached = SyncContext::displaySourceProgress(source, event, flush);
    if (!cached) {
        // Tell parent about current source statistics directly before
        // PEV_ITEMRECEIVED. The PIM Manager relies on that extra
        // information.
        if (event.m_type == sysync::PEV_ITEMRECEIVED) {
            m_helper.emitSourceProgress(sysync::PEV_ITEMPROCESSED, source.getName(),
                                        source.getFinalSyncMode(),
                                        source.getAdded(),
                                        source.getUpdated(),
                                        source.getDeleted());
        }
        m_helper.emitSourceProgress(event.m_type, source.getName(), source.getFinalSyncMode(),
                                    event.m_extra1, event.m_extra2, event.m_extra3);
    }
    return cached;
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
    std::string password;
    std::string error;

    askPasswordAsync(passwordName, descr, key,
                     [&password] (const std::string &passwordArg) { password = passwordArg; },
                     [&error] () { Exception::handle(error, HANDLE_EXCEPTION_NO_ERROR); });
    // We know that askPasswordAsync() is done when it cleared the
    // callback functors.
    while (m_passwordSuccess) {
        g_main_context_iteration(nullptr, true);
    }
    if (!error.empty()) {
        Exception::tryRethrow(error);
        SE_THROW(StringPrintf("password request failed: %s", error.c_str()));
    }
    return password;
}

void DBusSync::askPasswordAsync(const std::string &passwordName,
                                const std::string &descr,
                                const ConfigPasswordKey &key,
                                const std::function<void (const std::string &)> &success,
                                const std::function<void ()> &failureException)
{
    // cannot handle more than one password request at a time
    m_passwordSuccess = {};
    m_passwordFailure = {};
    m_passwordDescr = descr;

    InitStateString password;
    if (GetLoadPasswordSignal()(getKeyring(), passwordName, descr, key, password) &&
        password.wasSet()) {
        // handled
        success(password);
        return;
    }

    try {
        SE_LOG_DEBUG(NULL, "asking parent for password");
        m_passwordSuccess = success;
        m_passwordFailure = failureException;
        m_helper.emitPasswordRequest(descr, key);
        if (!m_helper.connected()) {
            SE_LOG_DEBUG(NULL, "password request failed, lost connection");
            SE_THROW_EXCEPTION_STATUS(StatusException,
                                      StringPrintf("Could not get the '%s' password from user, no connection to UI.",
                                                   descr.c_str()),
                                      STATUS_PASSWORD_TIMEOUT);
        }
        if (SuspendFlags::getSuspendFlags().getState() != SuspendFlags::NORMAL) {
            SE_LOG_DEBUG(NULL, "password request failed, was asked to terminate");
            SE_THROW_EXCEPTION_STATUS(StatusException,
                                      StringPrintf("Could not get the '%s' password from user, was asked to shut down.",
                                                   descr.c_str()),
                                      STATUS_PASSWORD_TIMEOUT);
        }
    } catch (...) {
        m_passwordSuccess = {};
        m_passwordFailure = {};
        failureException();
    }
}

void DBusSync::passwordResponse(bool timedOut, bool aborted, const std::string &password)
{
    std::function<void (const std::string &)> success;
    std::function<void ()> failureException;

    std::swap(success, m_passwordSuccess);
    std::swap(failureException, m_passwordFailure);

    if (success && failureException) {
        SE_LOG_DEBUG(NULL, "password result: %s",
                     timedOut ? "timeout or parent gone" :
                     aborted ? "user abort" :
                     password.empty() ? "empty password" :
                     "valid password");
        try {
            if (timedOut) {
                SE_THROW_EXCEPTION_STATUS(StatusException,
                                          StringPrintf("Could not get the '%s' password from user.",
                                                       m_passwordDescr.c_str()),
                                          STATUS_PASSWORD_TIMEOUT);
            } else if (aborted) {
                SE_THROW_EXCEPTION_STATUS(StatusException,
                                          StringPrintf("User did not provide the '%s' password.",
                                                       m_passwordDescr.c_str()),
                                          SyncMLStatus(sysync::LOCERR_USERABORT));
            } else {
                success(password);
            }
        } catch (...) {
            failureException();
        }
    }
}

bool DBusSync::savePassword(const std::string &passwordName,
                            const std::string &password,
                            const ConfigPasswordKey &key)
{
    if (GetSavePasswordSignal()(getKeyring(), passwordName, password, key)) {
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
