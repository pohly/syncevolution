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

#include "session.h"
#include "connection.h"
#include "server.h"
#include "client.h"
#include "restart.h"
#include "info-req.h"
#include "session-common.h"
#include "dbus-callbacks.h"
#include "presence-status.h"

#include <syncevo/ForkExec.h>
#include <syncevo/SyncContext.h>
#include <syncevo/BoostHelper.h>

#ifdef USE_DLT
#include <syncevo/LogDLT.h>
#endif

#include <memory>


using namespace GDBusCXX;

SE_BEGIN_CXX

/** A Proxy to the remote session. */
class SessionProxy : public GDBusCXX::DBusRemoteObject
{
public:
    SessionProxy(const GDBusCXX::DBusConnectionPtr &conn, const std::string &instance) :
    GDBusCXX::DBusRemoteObject(conn.get(),
                               std::string(SessionCommon::HELPER_PATH) + "/" + instance,
                               SessionCommon::HELPER_IFACE,
                               SessionCommon::HELPER_DESTINATION,
                               true), // This is a one-to-one connection. Close it.
         /* m_getNamedConfig   (*this, "GetNamedConfig"), */
         /* m_setNamedConfig   (*this, "SetNamedConfig"), */
         /* m_getReports       (*this, "GetReports"), */
         /* m_checkSource      (*this, "CheckSource"), */
         /* m_getDatabases     (*this, "GetDatabases"), */
    m_sync(*this, "Sync"),
    m_setFreeze(*this, "SetFreeze"),
    m_restore(*this, "Restore"),
    m_execute(*this, "Execute"),
    m_passwordResponse(*this, "PasswordResponse"),
    m_storeMessage(*this, "StoreMessage"),
    m_connectionState(*this, "ConnectionState"),
         /* m_abort            (*this, "Abort"), */
         /* m_suspend          (*this, "Suspend"), */
         /* m_getStatus        (*this, "GetStatus"), */
         /* m_getProgress      (*this, "GetProgress"), */
         /* m_restore          (*this, "Restore"), */
         /* m_execute          (*this, "Execute"), */
         /* m_serverShutdown   (*this, "ServerShutdown"), */
         /* m_passwordResponse (*this, "PasswordResponse"), */
         /* m_setActive        (*this, "SetActive"), */
         /* m_statusChanged    (*this, "StatusChanged", false), */
         /* m_progressChanged  (*this, "ProgressChanged", false), */
    m_logOutput(*this, "LogOutput", false),
    m_syncProgress(*this, "SyncProgress", false),
    m_sourceProgress(*this, "SourceProgress", false),
    m_sourceSynced(*this, "SourceSynced", false),
    m_waiting(*this, "Waiting", false),
    m_syncSuccessStart(*this, "SyncSuccessStart", false),
    m_configChanged(*this, "ConfigChanged", false),
    m_passwordRequest(*this, "PasswordRequest", false),
    m_sendMessage(*this, "Message", false),
    m_shutdownConnection(*this, "Shutdown", false)
    {}

    /* GDBusCXX::DBusClientCall<ReadOperations::Config_t>          m_getNamedConfig; */
    /* GDBusCXX::DBusClientCall<bool>                              m_setNamedConfig; */
    /* GDBusCXX::DBusClientCall<std::vector<StringMap> >           m_getReports; */
    /* GDBusCXX::DBusClientCall<>                                    m_checkSource; */
    /* GDBusCXX::DBusClientCall<ReadOperations::SourceDatabases_t> m_getDatabases; */
    GDBusCXX::DBusClientCall<bool, SyncReport> m_sync;
    GDBusCXX::DBusClientCall<bool> m_setFreeze;
    GDBusCXX::DBusClientCall<bool> m_restore;
    GDBusCXX::DBusClientCall<bool> m_execute;
    /* GDBusCXX::DBusClientCall<>                                    m_serverShutdown; */
    GDBusCXX::DBusClientCall<> m_passwordResponse;
    GDBusCXX::DBusClientCall<> m_storeMessage;
    GDBusCXX::DBusClientCall<> m_connectionState;
    /* GDBusCXX::DBusClientCall<>                                    m_setActive; */
    /* GDBusCXX::SignalWatch<std::string, uint32_t, */
    /*                        SessionCommon::SourceStatuses_t>      m_statusChanged; */
    GDBusCXX::SignalWatch<std::string, std::string, std::string> m_logOutput;
    GDBusCXX::SignalWatch<sysync::TProgressEventEnum,
                           int32_t, int32_t, int32_t> m_syncProgress;
    GDBusCXX::SignalWatch<sysync::TProgressEventEnum,
                           std::string, SyncMode,
                           int32_t, int32_t, int32_t> m_sourceProgress;
    GDBusCXX::SignalWatch<std::string, SyncSourceReport> m_sourceSynced;
    GDBusCXX::SignalWatch<bool> m_waiting;
    GDBusCXX::SignalWatch<> m_syncSuccessStart;
    GDBusCXX::SignalWatch<> m_configChanged;
    GDBusCXX::SignalWatch<std::string, ConfigPasswordKey> m_passwordRequest;
    GDBusCXX::SignalWatch<DBusArray<uint8_t>, std::string, std::string> m_sendMessage;
    GDBusCXX::SignalWatch<> m_shutdownConnection;
};

void Session::attach(const Caller_t &caller)
{
    std::shared_ptr<Client> client(m_server.findClient(caller));
    if (!client) {
        throw runtime_error("unknown client");
    }
    std::shared_ptr<Session> me = shared_from_this();
    client->attach(me);
}

void Session::detach(const Caller_t &caller)
{
    std::shared_ptr<Client> client(m_server.findClient(caller));
    if (!client) {
        throw runtime_error("unknown client");
    }
    client->detach(this);
}

/**
 * validate key/value property and copy it to the filter
 * if okay
 */
static void copyProperty(const StringPair &keyvalue,
                         ConfigPropertyRegistry &registry,
                         FilterConfigNode::ConfigFilter &filter)
{
    const std::string &name = keyvalue.first;
    const std::string &value = keyvalue.second;
    const ConfigProperty *prop = registry.find(name);
    if (!prop) {
        SE_THROW_EXCEPTION(InvalidCall, StringPrintf("unknown property '%s'", name.c_str()));
    }
    std::string error;
    if (!prop->checkValue(value, error)) {
        SE_THROW_EXCEPTION(InvalidCall, StringPrintf("invalid value '%s' for property '%s': '%s'",
                                                     value.c_str(), name.c_str(), error.c_str()));
    }
    filter.insert(std::make_pair(keyvalue.first, InitStateString(keyvalue.second, true)));
}

static void setSyncFilters(const ReadOperations::Config_t &config,FilterConfigNode::ConfigFilter &syncFilter,std::map<std::string, FilterConfigNode::ConfigFilter> &sourceFilters)
{
    for (const auto &item: config) {
        string name = item.first;
        if (name.empty()) {
            ConfigPropertyRegistry &registry = SyncConfig::getRegistry();
            for (const auto &prop: item.second) {
                // read-only properties can (and have to be) ignored
                static const set< std::string, Nocase<std::string> > special {
                    "configName",
                    "description",
                    "score",
                    "deviceName",
                    "hardwareName",
                    "templateName",
                    "fingerprint"
                };
                if (special.find(prop.first) == special.end()) {
                    copyProperty(prop, registry, syncFilter);
                }
            }
        } else if (boost::starts_with(name, "source/")) {
            name = name.substr(strlen("source/"));
            FilterConfigNode::ConfigFilter &sourceFilter = sourceFilters[name];
            ConfigPropertyRegistry &registry = SyncSourceConfig::getRegistry();
            for (const auto &prop: item.second) {
                copyProperty(prop, registry, sourceFilter);
            }
        } else {
            SE_THROW_EXCEPTION(InvalidCall, StringPrintf("invalid config entry '%s'", name.c_str()));
        }
    }
}

void Session::setConfig(bool update, bool temporary,
                        const ReadOperations::Config_t &config)
{
    setNamedConfig(m_configName, update, temporary, config);
}

void Session::setNamedConfig(const std::string &configName,
                             bool update, bool temporary,
                             const ReadOperations::Config_t &config)
{
    PushLogger<Logger> guard(weak_from_this());
    if (m_runOperation != SessionCommon::OP_NULL) {
        string msg = StringPrintf("%s started, cannot change configuration at this time", runOpToString(m_runOperation).c_str());
        SE_THROW_EXCEPTION(InvalidCall, msg);
    }
    if (m_status != SESSION_ACTIVE) {
        SE_THROW_EXCEPTION(InvalidCall, "session is not active, call not allowed at this time");
    }
    // avoid the check if effect is the same as setConfig()
    if (m_configName != configName) {
        bool found = false;
        for (const std::string &flag: m_flags) {
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
    /** check whether we need remove the entire configuration */
    if(!update && !temporary && config.empty()) {
        auto syncConfig = std::make_shared<SyncConfig>(configName);
        if(syncConfig.get()) {
            syncConfig->remove();
            m_setConfig = true;
        }
        return;
    }

    /*
     * validate input config and convert to filters;
     * if validation fails, no harm was done at this point yet
     */
    FilterConfigNode::ConfigFilter syncFilter;
    SourceFilters_t sourceFilters;
    setSyncFilters(config, syncFilter, sourceFilters);

    if (temporary) {
        /* save temporary configs in session filters, either erasing old
           temporary settings or adding to them */
        if (update) {
            m_syncFilter.insert(syncFilter.begin(), syncFilter.end());
            for (const auto &source: sourceFilters) {
                auto it = m_sourceFilters.find(source.first);
                if (it != m_sourceFilters.end()) {
                    // add to existing source filter
                    it->second.insert(source.second.begin(), source.second.end());
                } else {
                    // add source filter
                    m_sourceFilters.insert(source);
                }
            }
        } else {
            m_syncFilter = syncFilter;
            m_sourceFilters = sourceFilters;
        }
        m_tempConfig = true;
    } else {
        /* need to save configurations */
        auto from = std::make_shared<SyncConfig>(configName);
        /* if it is not clear mode and config does not exist, an error throws */
        if(update && !from->exists()) {
            SE_THROW_EXCEPTION(NoSuchConfig, "The configuration '" + configName + "' doesn't exist" );
        }
        if(!update) {
            list<string> sources = from->getSyncSources();
            list<string>::iterator it;
            for(it = sources.begin(); it != sources.end(); ++it) {
                string source = "source/";
                source += *it;
                auto configIt = config.find(source);
                if(configIt == config.end()) {
                    /** if no config for this source, we remove it */
                    from->removeSyncSource(*it);
                } else {
                    /** just clear visiable properties, remove them and their values */
                    from->clearSyncSourceProperties(*it);
                }
            }
            from->clearSyncProperties();
        }
        /** generate new sources in the config map */
        for (const auto &source: config) {
            string sourceName = source.first;
            if (boost::starts_with(sourceName, "source/")) {
                sourceName = sourceName.substr(7); ///> 7 is the length of "source/"
                from->getSyncSourceNodes(sourceName);
            }
        }
        /* apply user settings */
        from->setConfigFilter(true, "", syncFilter);
        for (const auto &source: sourceFilters) {
            from->setConfigFilter(false, source.first, source.second);
        }

        // We need no interactive user interface, but we do need to handle
        // storing passwords in a keyring here.
        auto syncConfig = std::make_shared<SyncContext>(configName);
        syncConfig->prepareConfigForWrite();
        syncConfig->copy(*from, nullptr);

        class KeyringUI : public UserInterface {
            InitStateString m_keyring;
        public:
            KeyringUI(const InitStateString &keyring) :
                m_keyring(keyring)
            {}

            // Implement UserInterface.
            virtual bool savePassword(const std::string &passwordName,
                                      const std::string &password,
                                      const ConfigPasswordKey &key)
            {
                return GetSavePasswordSignal()(m_keyring, passwordName, password, key);
            }
            virtual void readStdin(std::string &content) { SE_THROW("not implemented"); }
            virtual std::string askPassword(const std::string &passwordName,
                                            const std::string &descr,
                                            const ConfigPasswordKey &key)
            {
                SE_THROW("not implemented");
                return "";
            }

        } ui(syncConfig->getKeyring());
        syncConfig->preFlush(ui);
        syncConfig->flush();
        m_setConfig = true;
    }
}

void Session::initServer(SharedBuffer data, const std::string &messageType)
{
    PushLogger<Logger> guard(weak_from_this());
    m_serverMode = true;
    m_initialMessage = data;
    m_initialMessageType = messageType;
}

void Session::syncExtended(const std::string &mode, const SessionCommon::SourceModes_t &sourceModes,
                           const StringMap &env)
{
    PushLogger<Logger> guard(weak_from_this());
    if (m_runOperation == SessionCommon::OP_SYNC) {
        string msg = StringPrintf("%s started, cannot start again", runOpToString(m_runOperation).c_str());
        SE_THROW_EXCEPTION(InvalidCall, msg);
    } else if (m_runOperation != SessionCommon::OP_NULL) {
        string msg = StringPrintf("%s started, cannot start sync", runOpToString(m_runOperation).c_str());
        SE_THROW_EXCEPTION(InvalidCall, msg);
    }
    if (m_status != SESSION_ACTIVE) {
        SE_THROW_EXCEPTION(InvalidCall, "session is not active, call not allowed at this time");
    }

    m_syncMode = mode;
    m_syncEnv = env;

    // Turn session into "running sync" now, before returning to
    // caller. Starting the helper (if needed) and making it
    // execute the sync is part of "running sync".
    runOperationAsync(SessionCommon::OP_SYNC,
                      [this, mode, sourceModes] () { sync2(mode, sourceModes); },
                      env);
}

void Session::sync2(const std::string &mode, const SessionCommon::SourceModes_t &sourceModes)
{
    PushLogger<Logger> guard(weak_from_this());
    if (!m_forkExecParent || !m_helper) {
        SE_THROW("syncing cannot continue, helper died");
    }

    // helper is ready, tell it what to do
    SyncParams params;
    params.m_config = m_configName;
    params.m_mode = mode;
    params.m_sourceModes = sourceModes;
    params.m_serverMode = m_serverMode;
    params.m_serverAlerted = m_serverAlerted;
    params.m_remoteInitiated = m_remoteInitiated;
    params.m_sessionID = m_sessionID;
    params.m_initialMessage = m_initialMessage;
    params.m_initialMessageType = m_initialMessageType;
    params.m_syncFilter = m_syncFilter;
    params.m_sourceFilter = m_sourceFilter;
    params.m_sourceFilters = m_sourceFilters;

    std::shared_ptr<Connection> c = m_connection.lock();
    if (c && !c->mustAuthenticate()) {
        // unsetting username/password disables checking them
        params.m_syncFilter["password"] = InitStateString("", true);
        params.m_syncFilter["username"] = InitStateString("", true);
    }

    // Relay messages between connection and helper.If the
    // connection goes away, we need to tell the helper, because
    // otherwise it will never know that its message went into nirvana
    // and that it is waiting for a reply that will never come.
    //
    // We also need to send responses to the helper asynchronously
    // and ignore failures -> do it in our code instead of connection
    // signals directly.
    //
    // Session might quit before connection, so use instance
    // tracking.
    auto sendViaConnection = [this] (const DBusArray<uint8_t> buffer,
                                     const std::string &type,
                                     const std::string &url) {
        PushLogger<Logger> guard(weak_from_this());
        try {
            std::shared_ptr<Connection> connection = m_connection.lock();

            if (!connection) {
                SE_THROW_EXCEPTION(TransportException,
                                   "D-Bus peer has disconnected");
            }

            connection->send(buffer, type, url);
        } catch (...) {
            std::string explanation;
            Exception::handle(explanation);
            connectionState(explanation);
        }
    };
    m_helper->m_sendMessage.activate(sendViaConnection);
    auto shutdownConnection = [this] () {
        PushLogger<Logger> guard(weak_from_this());
        try {
            std::shared_ptr<Connection> connection = m_connection.lock();

            if (!connection) {
                SE_THROW_EXCEPTION(TransportException,
                                   "D-Bus peer has disconnected");
            }

            connection->sendFinalMsg();
        } catch (...) {
            std::string explanation;
            Exception::handle(explanation);
            connectionState(explanation);
        }
    };
    m_helper->m_shutdownConnection.activate(shutdownConnection);
    std::shared_ptr<Connection> connection = m_connection.lock();
    if (connection) {
        connection->m_messageSignal.connect(Connection::MessageSignal_t::slot_type(&Session::storeMessage,
                                                                                   this,
                                                                                   boost::placeholders::_1, boost::placeholders::_2).track_foreign(weak_from_this()));
        connection->m_statusSignal.connect(Connection::StatusSignal_t::slot_type(&Session::connectionState,
                                                                                 this,
                                                                                 boost::placeholders::_1).track_foreign(weak_from_this()));
    }

    // Helper implements Sync() asynchronously. If it completes
    // normally, dbusResultCb() will call doneCb() directly. Otherwise
    // the error is recorded before ending the session. Premature
    // exits by the helper are handled by D-Bus, which then will abort
    // the pending method call.
    m_helper->m_sync.start([me = weak_from_this()] (bool success, const SyncReport &report, const std::string &error) {
            dbusResultCb(me, "sync()", success, report, error);
        },
        params);
}

void Session::abort()
{
    PushLogger<Logger> guard(weak_from_this());
    if (m_runOperation != SessionCommon::OP_SYNC && m_runOperation != SessionCommon::OP_CMDLINE) {
        SE_THROW_EXCEPTION(InvalidCall, "sync not started, cannot abort at this time");
    }
    if (m_forkExecParent) {
        // Tell helper to abort via SIGTERM. The signal might get
        // delivered so soon that the helper quits immediately.
        // Treat that as "aborted by user" instead of failure
        // in m_onQuit.
        m_wasAborted = true;
        m_forkExecParent->stop(SIGTERM);
    }
    if (m_syncStatus == SYNC_RUNNING ||
        m_syncStatus == SYNC_SUSPEND) {
        m_syncStatus = SYNC_ABORT;
        fireStatus(true);
    }
}

void Session::setFreezeAsync(bool freeze, const Result<void (bool)> &result)
{
    PushLogger<Logger> guard(weak_from_this());
    SE_LOG_DEBUG(NULL, "session %s: SetFreeze(%s), %s",
                 getPath(),
                 freeze ? "freeze" : "thaw",
                 m_forkExecParent ? "send to helper" : "no effect, because no helper");
    if (m_forkExecParent) {
        auto done = [this, me = weak_from_this(), freeze, result] (bool changed, const std::string &error) noexcept {
            auto lock = me.lock();
            if (!lock) {
                return;
            }
            PushLogger<Logger> guard(weak_from_this());
            try {
                SE_LOG_DEBUG(NULL, "session %s: SetFreeze(%s) returned from helper %s, error %s",
                             getPath(),
                             freeze ? "freeze" : "thaw",
                             changed ? "changed freeze state" : "no effect",
                             error.c_str());
                if (!error.empty()) {
                    Exception::tryRethrowDBus(error);
                }
                if (changed) {
                    m_freeze = freeze;
                }
                result.done(changed);
            } catch (...) {
                result.failed();
            }
        };
        m_helper->m_setFreeze.start(done, freeze);
    } else {
        // Had no effect.
        result.done(false);
    }
}

void Session::suspend()
{
    PushLogger<Logger> guard(weak_from_this());
    if (m_runOperation != SessionCommon::OP_SYNC && m_runOperation != SessionCommon::OP_CMDLINE) {
        SE_THROW_EXCEPTION(InvalidCall, "sync not started, cannot suspend at this time");
    }
    if (m_forkExecParent) {
        // same as abort(), except that we use SIGINT
        m_wasAborted = true;
        m_forkExecParent->stop(SIGINT);
    }
    if (m_syncStatus == SYNC_RUNNING) {
        m_syncStatus = SYNC_SUSPEND;
        fireStatus(true);
    }
}

void Session::abortAsync(const SimpleResult &result)
{
    PushLogger<Logger> guard(weak_from_this());
    if (!m_forkExecParent) {
        result.done();
    } else {
        // Tell helper to quit, if necessary by aborting a running sync.
        // Once it is dead we know that the session no longer runs.
        // This must succeed; there is no timeout or failure mode.
        // TODO: kill helper after a certain amount of time?!
        m_forkExecParent->stop(SIGTERM);
        m_forkExecParent->m_onQuit.connect([result] (int) { result.done(); });
    }
}

void Session::getStatus(std::string &status,
                        uint32_t &error,
                        SourceStatuses_t &sources)
{
    PushLogger<Logger> guard(weak_from_this());
    status = syncStatusToString(m_syncStatus);
    if (m_stepIsWaiting) {
        status += ";waiting";
    }

    error = m_error;
    sources = m_sourceStatus;
}

void Session::getAPIProgress(int32_t &progress,
                             APISourceProgresses_t &sources)
{
    PushLogger<Logger> guard(weak_from_this());
    progress = m_progData.getProgress();
    sources = m_sourceProgress;
}

void Session::getProgress(int32_t &progress,
                          SourceProgresses_t &sources)
{
    PushLogger<Logger> guard(weak_from_this());
    progress = m_progData.getProgress();
    sources = m_sourceProgress;
}

bool Session::getSyncSourceReport(const std::string &sourceName, SyncSourceReport &report) const
{
    auto it = m_syncSourceReports.find(sourceName);
    if (it != m_syncSourceReports.end()) {
        report = it->second;
        return true;
    } else {
        return false;
    }
}

void Session::fireStatus(bool flush)
{
    PushLogger<Logger> guard(weak_from_this());
    std::string status;
    uint32_t error;
    SourceStatuses_t sources;

    /** not force flushing and not timeout, return */
    if(!flush && !m_statusTimer.timeout()) {
        return;
    }
    m_statusTimer.reset();

    getStatus(status, error, sources);
    m_statusSignal(status, error, sources);
}

void Session::fireProgress(bool flush)
{
    PushLogger<Logger> guard(weak_from_this());
    int32_t progress;
    SourceProgresses_t sources;

    /** not force flushing and not timeout, return */
    if(!flush && !m_progressTimer.timeout()) {
        return;
    }
    m_progressTimer.reset();

    getProgress(progress, sources);
    m_progressSignal(progress, sources);
}

Session::Session(Server &server,
                 const std::string &peerDeviceID,
                 const std::string &config_name,
                 const std::string &session,
                 const std::vector<std::string> &flags) :
    DBusObjectHelper(server.getConnection(),
                     std::string("/org/syncevolution/Session/") + session,
                     "org.syncevolution.Session",
                     [serverPtr = &server] () { serverPtr->autoTermCallback(); }),
    ReadOperations(config_name, server),
    m_flags(flags),
    m_sessionID(session),
    m_peerDeviceID(peerDeviceID),
    m_serverMode(false),
    m_serverAlerted(false),
    m_useConnection(false),
    m_tempConfig(false),
    m_setConfig(false),
    m_status(SESSION_IDLE),
    m_wasAborted(false),
    m_remoteInitiated(false),
    m_syncStatus(SYNC_QUEUEING),
    m_stepIsWaiting(false),
    m_priority(PRI_DEFAULT),
    m_error(0),
    m_lastProgressTimestamp(Timespec::monotonic()),
    m_freeze(false),
    m_statusTimer(100),
    m_progressTimer(50),
    m_restoreSrcTotal(0),
    m_restoreSrcEnd(0),
    m_runOperation(SessionCommon::OP_NULL),
    m_cmdlineOp(SessionCommon::OP_CMDLINE),
    emitStatus(*this, "StatusChanged"),
    emitProgress(*this, "ProgressChanged")
{
    add(this, &Session::attach, "Attach");
    add(this, &Session::detach, "Detach");
    add(this, &Session::getFlags, "GetFlags");
    add(this, &Session::getNormalConfigName, "GetConfigName");
    add(static_cast<ReadOperations *>(this), &ReadOperations::getConfigs, "GetConfigs");
    add(static_cast<ReadOperations *>(this), &ReadOperations::getConfig, "GetConfig");
    add(static_cast<ReadOperations *>(this), &ReadOperations::getNamedConfig, "GetNamedConfig");
    add(this, &Session::setConfig, "SetConfig");
    add(this, &Session::setNamedConfig, "SetNamedConfig");
    add(static_cast<ReadOperations *>(this), &ReadOperations::getReports, "GetReports");
    add(static_cast<ReadOperations *>(this), &ReadOperations::checkSource, "CheckSource");
    add(static_cast<ReadOperations *>(this), &ReadOperations::getDatabases, "GetDatabases");
    add(this, &Session::sync, "Sync");
    add(this, &Session::abort, "Abort");
    add(this, &Session::suspend, "Suspend");
    add(this, &Session::getStatus, "GetStatus");
    add(this, &Session::getAPIProgress, "GetProgress");
    add(this, &Session::restore, "Restore");
    add(this, &Session::checkPresence, "CheckPresence");
    add(this, &Session::execute, "Execute");
    add(emitStatus);
    add(emitProgress);
    auto status = [this] (const std::string &status,
                          uint32_t error,
                          const SourceStatuses_t &sources) {
        emitStatus(status, error, sources);
    };
    m_statusSignal.connect(status);
    auto progress = [this] (int32_t progress,
                            const SourceProgresses_t &sources) {
        m_lastProgressTimestamp.resetMonotonic();
        m_lastProgress = sources;
        emitProgress(progress, sources);
    };
    m_progressSignal.connect(progress);

    SE_LOG_DEBUG(NULL, "session %s created", getPath());
}

void Session::dbusResultCb(const std::weak_ptr<Session> &me, const std::string &operation, bool success, const SyncReport &report, const std::string &error) noexcept
{
    auto lock = me.lock();
    if (!lock) {
        return;
    }
    PushLogger<Logger> guard(me);
    try {
        SE_LOG_DEBUG(NULL, "%s helper call completed, %s",
                     operation.c_str(),
                     !error.empty() ? error.c_str() :
                     success ? "<<successfully>>" :
                     "<<unsuccessfully>>");
        if (error.empty()) {
            lock->doneCb(false, success, report);
        } else {
            // Translate back into local exception, will be handled by
            // catch clause and (eventually) failureCb().
            Exception::tryRethrowDBus(error);
            // generic fallback
            throw GDBusCXX::dbus_error("org.syncevolution.gdbuscxx.Exception",
                                       error);
        }
    } catch (...) {
        lock->failureCb();
    }
}

void Session::failureCb() throw()
{
    PushLogger<Logger> guard(weak_from_this());
    try {
        if (m_status == SESSION_DONE) {
            // ignore errors that happen after session already closed,
            // only log them
            std::string explanation;
            Exception::handle(explanation, HANDLE_EXCEPTION_NO_ERROR);
            m_server.logOutput(getPath(),
                               Logger::ERROR,
                               explanation,
                               "");
        } else {
            // finish session with failure
            uint32_t error;
            try {
                throw;
            } catch (...) {
                // only record problem
                std::string explanation;
                error = Exception::handle(explanation, HANDLE_EXCEPTION_NO_ERROR);
                m_server.logOutput(getPath(),
                                   Logger::ERROR,
                                   explanation,
                                   "");
            }
            // set error, but don't overwrite older one
            if (!m_error) {
                SE_LOG_DEBUG(NULL, "session failed: remember %d error", error);
                m_error = error;
            }
            // will fire status signal, including the error
            doneCb(false, false);
        }
    } catch (...) {
        // fatal problem, log it and terminate
        Exception::handle(HANDLE_EXCEPTION_FATAL);
    }
}

void Session::doneCb(bool destruct, bool success, const SyncReport &report) noexcept
{
    // When called from our destructor, then weak_from_this() fails (__cxa_call_unexpected).
    // We have to ignore logging in that case.
    std::weak_ptr<Session> me;
    if (!destruct) {
        me = weak_from_this();
    }
    PushLogger<Logger> guard(me);
    try {
        if (m_status == SESSION_DONE) {
            return;
        }
        m_status = SESSION_DONE;
        m_syncStatus = SYNC_DONE;
        if (!success && !m_error) {
            // some kind of local, internal problem
            m_error = STATUS_FATAL + sysync::LOCAL_STATUS_CODE;
        }

        fireStatus(true);

        std::shared_ptr<Connection> connection = m_connection.lock();
        if (connection) {
            connection->shutdown();
        }

        // tell everyone who is interested that our config changed (includes D-Bus signal)
        if (m_setConfig) {
            m_server.m_configChangedSignal(m_configName);
        }

        SE_LOG_DEBUG(NULL, "session %s done, config %s, %s, result %d",
                     getPath(),
                     m_configName.c_str(),
                     m_setConfig ? "modified" : "not modified",
                     m_error);
        m_doneSignal((SyncMLStatus)m_error, report);

        // now also kill helper
        m_helper.reset();
        if (m_forkExecParent) {
            // Abort (just in case, helper should already be waiting
            // for SIGURG).
            m_forkExecParent->stop(SIGTERM);
            // Quit.
            m_forkExecParent->stop(SIGURG);
        }

        m_server.removeSyncSession(this);
        m_server.dequeue(this);
    } catch (...) {
        // fatal problem, log it and terminate (?!)
        Exception::handle();
    }
}

Session::~Session()
{
    SE_LOG_DEBUG(NULL, "session %s deconstructing", getPath());
    // If we are not done yet, then something went wrong.
    doneCb(true, false);
}

void Session::runOperationAsync(SessionCommon::RunOperation op,
                                const SuccessCb_t &helperReady,
                                const StringMap &env)
{
    PushLogger<Logger> guard(weak_from_this());
    m_server.addSyncSession(this);
    m_runOperation = op;
    m_status = SESSION_RUNNING;
    m_syncStatus = SYNC_RUNNING;
    fireStatus(true);

    useHelperAsync(SimpleResult(helperReady, [this] () { failureCb(); }),
                   env);
}

void Session::useHelperAsync(const SimpleResult &result, const StringMap &env)
{
    PushLogger<Logger> guard(weak_from_this());
    try {
        if (m_helper) {
            // exists already, invoke callback directly
            result.done();
        }

        // Construct m_forkExecParent if it doesn't exist yet or not
        // currently starting. The only situation where the latter
        // might happen is when the helper is still starting when
        // a new request comes in. In that case we reuse the same
        // helper process for both operations.
        if (!m_forkExecParent ||
            m_forkExecParent->getState() != ForkExecParent::STARTING) {
            std::vector<std::string> args;
            args.push_back("--dbus-verbosity");
            args.push_back(StringPrintf("%d", m_server.getDBusLogLevel()));
            m_forkExecParent = make_weak_shared::make<ForkExecParent>("syncevo-dbus-helper", args);
#ifdef USE_DLT
            if (getenv("SYNCEVOLUTION_USE_DLT")) {
                m_forkExecParent->addEnvVar("SYNCEVOLUTION_USE_DLT", StringPrintf("%d", LoggerDLT::getCurrentDLTLogLevel()));
            }
#endif
            for (const auto &entry: env) {
                SE_LOG_DEBUG(NULL, "running helper with env variable %s=%s",
                             entry.first.c_str(), entry.second.c_str());
                m_forkExecParent->addEnvVar(entry.first, entry.second);
            }
            // We own m_forkExecParent, so the "this" pointer for
            // onConnect will live longer than the signal in
            // m_forkExecParent -> no need for resource
            // tracking. onConnect sets up m_helper. The other two
            // only log the event.
            auto onConnect = [this] (const GDBusCXX::DBusConnectionPtr &conn) noexcept {
                PushLogger<Logger> guard(weak_from_this());
                try {
                    std::string instance = m_forkExecParent->getInstance();
                    SE_LOG_DEBUG(NULL, "helper %s has connected", instance.c_str());
                    m_helper.reset(new SessionProxy(conn, instance));

                    // Activate signal watch on helper signals.
                    m_helper->m_syncProgress.activate([this] (sysync::TProgressEventEnum type,
                                                              int32_t extra1, int32_t extra2, int32_t extra3) {
                                                          syncProgress(type, extra1, extra2, extra3);
                                                      });
                    m_helper->m_sourceProgress.activate([this] (sysync::TProgressEventEnum type,
                                                                const std::string &sourceName,
                                                                SyncMode sourceSyncMode,
                                                                int32_t extra1, int32_t extra2, int32_t extra3) {
                                                            sourceProgress(type, sourceName, sourceSyncMode, extra1, extra2, extra3);
                                                        });
                    m_helper->m_sourceSynced.activate([this] (const std::string &name, const SyncSourceReport &report) {
                            m_sourceSynced(name, report);
                        });
                    m_sourceSynced.connect([this] (const std::string &name, const SyncSourceReport &report) {
                            m_syncSourceReports[name] = report;
                        });
                    auto setWaiting = [this] (bool isWaiting) {
                        PushLogger<Logger> guard(weak_from_this());
                        // if stepInfo doesn't change, then ignore it to avoid duplicate status info
                        if (m_stepIsWaiting != isWaiting) {
                            m_stepIsWaiting = isWaiting;
                            fireStatus(true);
                        }
                    };
                    m_helper->m_waiting.activate(setWaiting);
                    m_helper->m_syncSuccessStart.activate([this] () {
                            m_syncSuccessStartSignal();
                        });
                    m_helper->m_configChanged.activate([this] () {
                            m_server.m_configChangedSignal("");
                        });
                    auto passwordRequest = [this] (const std::string &descr, const ConfigPasswordKey &key) {
                        PushLogger<Logger> guard(weak_from_this());
                        m_passwordRequest = m_server.passwordRequest(descr, key, weak_from_this());
                    };
                    m_helper->m_passwordRequest.activate(passwordRequest);
                } catch (...) {
                    Exception::handle();
                }
            };
            auto onQuit = [this] (int status) noexcept {
                PushLogger<Logger> guard(weak_from_this());
                try {
                    SE_LOG_DEBUG(NULL, "helper quit with return code %d, was %s",
                                 status,
                                 m_wasAborted ? "aborted" : "not aborted");
                    if (m_status == SESSION_DONE) {
                        // don't care anymore whether the helper goes down, not an error
                        SE_LOG_DEBUG(NULL, "session already completed, ignore helper");
                    } else if (m_wasAborted  &&
                               ((WIFEXITED(status) && WEXITSTATUS(status) == 0) ||
                                (WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM))) {
                        SE_LOG_DEBUG(NULL, "helper terminated via SIGTERM, as expected");
                        if (!m_error) {
                            m_error = sysync::LOCERR_USERABORT;
                            SE_LOG_DEBUG(NULL, "helper was asked to quit -> error %d = LOCERR_USERABORT",
                                         m_error);
                        }
                    } else {
                        // Premature exit from helper?! Not necessarily, it could
                        // be that we get the "helper has quit" signal from
                        // ForkExecParent before processing the helper's D-Bus
                        // method reply. So instead of recording an error here,
                        // wait for that reply. If the helper died without sending
                        // it, then D-Bus will generate a "connection lost" error
                        // for our pending method call.
                        //
                        // Except that libdbus does not deliver that error
                        // reliably. As a workaround, schedule closing the
                        // session as an idle callback, after that potential
                        // future method return call was handled. The assumption
                        // is that it is pending - it must be, because with the
                        // helper gone, IO with it must be ready. Just to be sure
                        // a small delay is used.
                    }
                    auto done = [me = weak_from_this()] () {
                        auto lock = me.lock();
                        if (lock) {
                            lock->doneCb(false, {});
                        }
                    };
                    m_server.addTimeout(done, 1 /* seconds */);
                } catch (...) {
                    Exception::handle();
                }
            };
            auto onFailure = [this] (SyncMLStatus status, const std::string &explanation) noexcept {
                PushLogger<Logger> guard(weak_from_this());
                try {
                    SE_LOG_DEBUG(NULL, "helper failed, status code %d = %s, %s",
                                 status,
                                 Status2String(status).c_str(),
                                 explanation.c_str());
                } catch (...) {
                    Exception::handle();
                }
            };
            m_forkExecParent->m_onConnect.connect(onConnect);
            m_forkExecParent->m_onQuit.connect(onQuit);
            m_forkExecParent->m_onFailure.connect(onFailure);

            if (!getenv("SYNCEVOLUTION_DEBUG")) {
                // Any output from the helper is unexpected and will be
                // logged as error. The helper initializes stderr and
                // stdout redirection once it runs, so anything that
                // reaches us must have been problems during early process
                // startup or final shutdown.
                auto onOutput = [this] (const char *buffer, size_t length) {
                    PushLogger<Logger> guard(weak_from_this());
                    // treat null-bytes inside the buffer like line breaks
                    size_t off = 0;
                    do {
                        SE_LOG_ERROR("session-helper", "%s", buffer + off);
                        off += strlen(buffer + off) + 1;
                    } while (off < length);
                };
                m_forkExecParent->m_onOutput.connect(onOutput);
            }
        }

        // Now also connect result with the right events. Will be
        // called after setting up m_helper (first come, first
        // serve). We copy the "result" instance with the closure, and
        // the creator of it must have made sure that we can invoke it
        // at any time without crashing.
        //
        // If the helper quits before connecting, the startup
        // failed. Need to remove that connection when successful.
        auto raiseChildTermError = [result] (int status) noexcept {
            try {
                SE_THROW(StringPrintf("helper died unexpectedly with return code %d before connecting", status));
            } catch (...) {
                result.failed();
            }
        };
        auto c = m_forkExecParent->m_onQuit.connect(raiseChildTermError);

        m_forkExecParent->m_onConnect.connect([this, result, c] (const GDBusCXX::DBusConnectionPtr &) { useHelper2(result, c); });

        if (m_forkExecParent->getState() == ForkExecParent::IDLE) {
            m_forkExecParent->start();
        }
    } catch (...) {
        // The assumption here is that any exception is related only
        // to the requested operation, and that the server itself is still
        // healthy.
        result.failed();
    }
}

void Session::messagev(const MessageOptions &options,
                       const char *format,
                       va_list args)
{
    // log with session path and empty process name,
    // just like the syncevo-dbus-helper does
    m_server.message2DBus(options,
                          format, args,
                          getPath(), "");
}

static void Logging2ServerAndStdout(Server &server,
                                    const GDBusCXX::DBusObject_t &path,
                                    const Logger::MessageOptions &options,
                                    const char *format,
                                    ...)
{
    va_list args;
    va_start(args, format);
    server.message2DBus(options, format, args, path, options.m_processName ? *options.m_processName : "");
    va_end(args);
}

static void Logging2Server(Server &server,
                           const GDBusCXX::DBusObject_t &path,
                           const std::string &strLevel,
                           const std::string &explanation,
                           const std::string &procname)
{
    static bool dbg = getenv("SYNCEVOLUTION_DEBUG");

    if (dbg) {
        // Print to D-Bus directly. The helper handles its own
        // printing to the console.
        server.logOutput(path,
                         Logger::strToLevel(strLevel.c_str()),
                         explanation,
                         procname);
    } else {
        // Print to D-Bus and console, because the helper
        // relies on us to do that. Its own stdout/stderr
        // was redirected into our pipe and any output
        // there is considered an error.
        Logger::MessageOptions options(Logger::strToLevel(strLevel.c_str()));
        options.m_processName = &procname;
        options.m_flags = Logger::MessageOptions::ALREADY_LOGGED;
        Logging2ServerAndStdout(server, path, options, "%s", explanation.c_str());
    }
}

void Session::useHelper2(const SimpleResult &result, const boost::signals2::connection &c)
{
    PushLogger<Logger> guard(weak_from_this());
    try {
        // helper is running, don't call result.failed() when it quits
        // sometime in the future
        c.disconnect();

        // Verify that helper is really ready. Might not be the
        // case when something internally failed in onConnect.
        if (m_helper) {
            // Resend all output from helper via the server's own
            // LogOutput signal, with the session's object path as
            // first parameter.
            //
            // Any code in syncevo-dbus-server which might produce
            // output related to the session runs while a Session::LoggingGuard
            // captures output by pushing Session as logger onto the
            // logging stack. The Session::messagev implementation then
            // also calls m_server.logOutput, as if the syncevo-dbus-helper
            // had produced that output.
            //
            // The downside is that unrelated output (like
            // book-keeping messages about other clients) will also be
            // captured.
            m_helper->m_logOutput.activate([this] (const std::string &strLevel,
                                                   const std::string &explanation,
                                                   const std::string &procname) {
                                               Logging2Server(m_server, getPath(),
                                                              strLevel, explanation, procname);
                                           });
            result.done();
        } else {
            SE_THROW("internal error, helper not ready");
        }
    } catch (...) {
        // Same assumption as above: let's hope the server is still
        // sane.
        result.failed();
    }
}

void Session::activateSession()
{
    PushLogger<Logger> guard(weak_from_this());
    if (m_status != SESSION_IDLE) {
        SE_THROW("internal error, session changing from non-idle to active");
    }
    m_status = SESSION_ACTIVE;

    if (m_syncStatus == SYNC_QUEUEING) {
        m_syncStatus = SYNC_IDLE;
        fireStatus(true);
    }

    std::shared_ptr<Connection> c = m_connection.lock();
    if (c) {
        c->ready();
    }

    m_sessionActiveSignal();
}

void Session::passwordResponse(bool timedOut, bool aborted, const std::string &password)
{
    PushLogger<Logger> guard(weak_from_this());
    if (m_helper) {
        // Ignore communicaton failures with helper here,
        // we'll notice that elsewhere
        m_helper->m_passwordResponse.start(std::function<void (const std::string &)>(),
                                           timedOut, aborted, password);
    }
}


void Session::syncProgress(sysync::TProgressEventEnum type,
                           int32_t extra1, int32_t extra2, int32_t extra3)
{
    PushLogger<Logger> guard(weak_from_this());
    switch(type) {
    case sysync::PEV_CUSTOM_START:
        m_cmdlineOp = (RunOperation)extra1;
        break;
    case sysync::PEV_SESSIONSTART:
        m_progData.setStep(ProgressData::PRO_SYNC_INIT);
        fireProgress(true);
        break;
    case sysync::PEV_SESSIONEND:
        // Ignore the error here. It was seen
        // (TestSessionAPIsDummy.testAutoSyncNetworkFailure) that the
        // engine reports 20017 = user abort when the real error is a
        // transport error encountered outside of the
        // engine. Recording the error as seen by the engine leads to
        // an incorrect final session result. Instead wait for the
        // result of the sync method invocation.
        //
        // if((uint32_t)extra1 != m_error) {
        //     SE_LOG_DEBUG(NULL, "session sync progress: failed with code %d", extra1);
        //     m_error = extra1;
        //     fireStatus(true);
        // }
        m_progData.setStep(ProgressData::PRO_SYNC_INVALID);
        fireProgress(true);
        break;
    case sysync::PEV_SENDSTART:
        m_progData.sendStart();
        break;
    case sysync::PEV_SENDEND:
    case sysync::PEV_RECVSTART:
    case sysync::PEV_RECVEND:
        m_progData.receiveEnd();
        fireProgress();
        break;
    case sysync::PEV_DISPLAY100:
    case sysync::PEV_SUSPENDCHECK:
    case sysync::PEV_DELETING:
        break;
    case sysync::PEV_SUSPENDING:
        m_syncStatus = SYNC_SUSPEND;
        fireStatus(true);
        break;
    default:
        ;
    }
}

void Session::sourceProgress(sysync::TProgressEventEnum type,
                             const std::string &sourceName,
                             SyncMode sourceSyncMode,
                             int32_t extra1, int32_t extra2, int32_t extra3)
{
    PushLogger<Logger> guard(weak_from_this());
    // a command line operation can be many things, helper must have told us
    SessionCommon::RunOperation op = m_runOperation == SessionCommon::OP_CMDLINE ?
        m_cmdlineOp :
        m_runOperation;

    switch(op) {
    case SessionCommon::OP_SYNC: {
        // Helper will create new source entries by sending a
        // sysync::PEV_PREPARING with SYNC_NONE. Must fire progress
        // and status events for such new sources.
        auto pit = m_sourceProgress.find(sourceName);
        bool sourceProgressCreated = pit == m_sourceProgress.end();
        SourceProgress &progress = sourceProgressCreated ? m_sourceProgress[sourceName] : pit->second;

        auto sit = m_sourceStatus.find(sourceName);
        bool sourceStatusCreated = sit == m_sourceStatus.end();
        SourceStatus &status = sourceStatusCreated ? m_sourceStatus[sourceName] : sit->second;

        switch(type) {
        case sysync::PEV_SYNCSTART:
            if (sourceSyncMode != SYNC_NONE) {
                m_progData.setStep(ProgressData::PRO_SYNC_UNINIT);
                fireProgress();
            }
            break;
        case sysync::PEV_SYNCEND:
            if (sourceSyncMode != SYNC_NONE) {
                status.set(PrettyPrintSyncMode(sourceSyncMode), "done", extra1);
                fireStatus(true);
            }
            break;
        case sysync::PEV_PREPARING:
            if (sourceSyncMode != SYNC_NONE) {
                progress.m_phase        = "preparing";
                progress.m_prepareCount = extra1;
                progress.m_prepareTotal = extra2;
                m_progData.itemPrepare();
                fireProgress(true);
            } else {
                // Check whether the sources where created.
                if (sourceProgressCreated) {
                    fireProgress();
                }
                if (sourceStatusCreated) {
                    fireStatus();
                }
            }
            break;
        case sysync::PEV_ITEMSENT:
            if (sourceSyncMode != SYNC_NONE) {
                progress.m_phase     = "sending";
                progress.m_sendCount = extra1;
                progress.m_sendTotal = extra2;
                fireProgress(true);
            }
            break;
        case sysync::PEV_ITEMRECEIVED:
            if (sourceSyncMode != SYNC_NONE) {
                progress.m_phase        = "receiving";
                progress.m_receiveCount = extra1;
                progress.m_receiveTotal = extra2;
                m_progData.itemReceive(sourceName, extra1, extra2);
                fireProgress();
            }
            break;
        case sysync::PEV_ITEMPROCESSED:
            progress.m_added = extra1;
            progress.m_updated = extra2;
            progress.m_deleted = extra3;
            // Do not fireProgress() here! We are going to get a
            // PEV_ITEMRECEIVED directly afterwards (see
            // dbus-sync.cpp).
            break;
        case sysync::PEV_ALERTED:
            if (sourceSyncMode != SYNC_NONE) {
                // Reset item counts, must be set (a)new.
                // Relevant in multi-cycle syncing.
                progress.m_receiveCount = -1;
                progress.m_receiveTotal = -1;
                progress.m_sendCount = -1;
                progress.m_sendTotal = -1;
                status.set(PrettyPrintSyncMode(sourceSyncMode), "running", 0);
                fireStatus(true);
                m_progData.setStep(ProgressData::PRO_SYNC_DATA);
                m_progData.addSyncMode(sourceSyncMode);
                fireProgress();
            }
            break;
        default:
            ;
        }
        break;
    }
    case SessionCommon::OP_RESTORE: {
        switch(type) {
        case sysync::PEV_ALERTED:
            // count the total number of sources to be restored
            m_restoreSrcTotal++;
            break;
        case sysync::PEV_SYNCSTART: {
            if (sourceSyncMode != SYNC_NONE) {
                SourceStatus &status = m_sourceStatus[sourceName];
                // set statuses as 'restore-from-backup'
                status.set(PrettyPrintSyncMode(sourceSyncMode), "running", 0);
                fireStatus(true);
            }
            break;
        }
        case sysync::PEV_SYNCEND: {
            if (sourceSyncMode != SYNC_NONE) {
                m_restoreSrcEnd++;
                SourceStatus &status = m_sourceStatus[sourceName];
                status.set(PrettyPrintSyncMode(sourceSyncMode), "done", 0);
                m_progData.setProgress(100 * m_restoreSrcEnd / m_restoreSrcTotal);
                fireStatus(true);
                fireProgress(true);
            }
            break;
        }
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
}

bool Session::setFilters(SyncConfig &config)
{
    PushLogger<Logger> guard(weak_from_this());
    /** apply temporary configs to config */
    config.setConfigFilter(true, "", m_syncFilter);
    // set all sources in the filter to config
    for (const auto &value: m_sourceFilters) {
        config.setConfigFilter(false, value.first, value.second);
    }
    return m_tempConfig;
}

void Session::restore(const string &dir, bool before, const std::vector<std::string> &sources)
{
    PushLogger<Logger> guard(weak_from_this());
    if (m_runOperation == SessionCommon::OP_RESTORE) {
        string msg = StringPrintf("restore started, cannot restore again");
        SE_THROW_EXCEPTION(InvalidCall, msg);
    } else if (m_runOperation != SessionCommon::OP_NULL) {
        // actually this never happen currently, for during the real restore process,
        // it never poll the sources in default main context
        string msg = StringPrintf("%s started, cannot restore", runOpToString(m_runOperation).c_str());
        SE_THROW_EXCEPTION(InvalidCall, msg);
    }
    if (m_status != SESSION_ACTIVE) {
        SE_THROW_EXCEPTION(InvalidCall, "session is not active, call not allowed at this time");
    }

    runOperationAsync(SessionCommon::OP_RESTORE,
                      [this, dir, before, sources] () {
                          restore2(dir, before, sources);
                      });
}

void Session::restore2(const string &dir, bool before, const std::vector<std::string> &sources)
{
    PushLogger<Logger> guard(weak_from_this());
    if (!m_forkExecParent || !m_helper) {
        SE_THROW("syncing cannot continue, helper died");
    }

    // helper is ready, tell it what to do
    m_helper->m_restore.start([me = weak_from_this()] (bool success, const std::string &error) {
            dbusResultCb(me, "restore()", success, {}, error);
        },
        m_configName, dir, before, sources);
}

void Session::execute(const vector<string> &args, const map<string, string> &vars)
{
    PushLogger<Logger> guard(weak_from_this());
    if (m_runOperation == SessionCommon::OP_CMDLINE) {
        SE_THROW_EXCEPTION(InvalidCall, "cmdline started, cannot start again");
    } else if (m_runOperation != SessionCommon::OP_NULL) {
        string msg = StringPrintf("%s started, cannot start cmdline", runOpToString(m_runOperation).c_str());
        SE_THROW_EXCEPTION(InvalidCall, msg);
    }
    if (m_status != SESSION_ACTIVE) {
        SE_THROW_EXCEPTION(InvalidCall, "session is not active, call not allowed at this time");
    }

    runOperationAsync(SessionCommon::OP_CMDLINE,
                      [this, args, vars] () {
                          execute2(args, vars);
                      });
}

void Session::execute2(const vector<string> &args, const map<string, string> &vars)
{
    PushLogger<Logger> guard(weak_from_this());
    if (!m_forkExecParent || !m_helper) {
        SE_THROW("syncing cannot continue, helper died");
    }

    // helper is ready, tell it what to do
    m_helper->m_execute.start([me = weak_from_this()] (bool success, const std::string &error) {
            dbusResultCb(me, "execute()", success, {}, error);
        },
        args, vars);
}

/*Implementation of Session.CheckPresence */
void Session::checkPresence (string &status)
{
    PushLogger<Logger> guard(weak_from_this());
    vector<string> transport;
    m_server.checkPresence(m_configName, status, transport);
}

void Session::storeMessage(const DBusArray<uint8_t> &message,
                           const std::string &type)
{
    PushLogger<Logger> guard(weak_from_this());
    // ignore errors
    if (m_helper) {
        m_helper->m_storeMessage.start(std::function<void (const std::string &)>(),
                                       message, type);
    }
}

void Session::connectionState(const std::string &error)
{
    PushLogger<Logger> guard(weak_from_this());
    // ignore errors
    if (m_helper) {
        m_helper->m_connectionState.start(std::function<void (const std::string &)>(),
                                          error);
    }
}

SE_END_CXX
