/*
 * Copyright (C) 2012 Intel Corporation
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

#ifndef SESSION_IMPL_H
#define SESSION_IMPL_H

#include <syncevo/SynthesisEngine.h>
#include <syncevo/SuspendFlags.h>

#include <boost/weak_ptr.hpp>
#include <boost/utility.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/lexical_cast.hpp>

#include "read-operations.h"
#include "progress-data.h"
#include "source-progress.h"
#include "source-status.h"
#include "timer.h"
#include "session-common.h"

SE_BEGIN_CXX

class ConnectionImpl;
class CmdlineWrapper;
class DBusSync;
class SessionListener;
class LogRedirect;

/**
 * Represents and implements the Session interface.  Use
 * boost::shared_ptr to track it and ensure that there are references
 * to it as long as the connection is needed.
 */
class SessionImpl : public GDBusCXX::DBusObjectHelper,
                    private ReadOperations,
                    private boost::noncopyable
{
    std::vector<std::string> m_flags;
    const std::string m_sessionID;

    bool m_serverMode;
    bool m_serverAlerted;
    SharedBuffer m_initialMessage;
    string m_initialMessageType;

    GMainLoop *m_loop;

    boost::weak_ptr<ConnectionImpl> m_connection;
    std::string m_connectionError;
    bool m_useConnection;

    /** temporary config changes */
    FilterConfigNode::ConfigFilter m_syncFilter;
    FilterConfigNode::ConfigFilter m_sourceFilter;
    typedef std::map<std::string, FilterConfigNode::ConfigFilter> SourceFilters_t;
    SourceFilters_t m_sourceFilters;

    /** whether dbus clients set temporary configs */
    bool m_tempConfig;

    /**
     * whether the dbus clients updated, removed or cleared configs,
     * ignoring temporary configuration changes
     */
    bool m_setConfig;

    /**
     * True while clients are allowed to make calls other than Detach(),
     * which is always allowed. Some calls are not allowed while this
     * session runs a sync, which is indicated by a non-NULL m_sync
     * pointer.
     */
    bool m_active;

    /**
     * True once the server process has called ServerShutdown or a
     * shutdown signal was received.
     */
    bool m_shutdownRequested;

    /**
     * Indicates whether this session was initiated by the peer or locally.
     */
    bool m_remoteInitiated;

    /**
     * The SyncEvolution instance which currently prepares or runs a sync.
     */
    boost::shared_ptr<DBusSync> m_sync;

    /**
     * current sync status; suspend and abort must be mirrored in global SuspendFlags
     */
    class SyncStatusOwner : boost::noncopyable {
    public:
        SyncStatusOwner() : m_status(SessionCommon::SYNC_QUEUEING), m_active(false) {}
        SyncStatusOwner(SessionCommon::SyncStatus status) : m_status(SessionCommon::SYNC_QUEUEING), m_active(false)
        {
            setStatus(status);
        }
        operator SessionCommon::SyncStatus () { return m_status; }
        SyncStatusOwner &operator = (SessionCommon::SyncStatus status) { setStatus(status); return *this; }

        void setStatus(SessionCommon::SyncStatus status);

    private:
        SessionCommon::SyncStatus m_status;
        bool m_active;
        boost::shared_ptr<SuspendFlags::StateBlocker> m_blocker;
    } m_syncStatus;

    /** step info: whether engine is waiting for something */
    bool m_stepIsWaiting;

    int32_t m_progress;

    /** progress data, holding progress calculation related info */
    ProgressData m_progData;

    SessionCommon::SourceStatuses_t m_sourceStatus;

    uint32_t m_error;
    SessionCommon::SourceProgresses_t m_sourceProgress;

    /** timer for fire status/progress usages */
    Timer m_statusTimer;
    Timer m_progressTimer;

    /** restore used */
    string m_restoreDir;
    bool m_restoreBefore;
    /** the total number of sources to be restored */
    int m_restoreSrcTotal;
    /** the number of sources that have been restored */
    int m_restoreSrcEnd;

    /**
     * status of the session
     */
    enum RunOperation {
        OP_SYNC,            /**< running a sync */
        OP_RESTORE,         /**< restoring data */
        OP_CMDLINE,         /**< executing command line */
        OP_NULL             /**< idle, accepting commands via D-Bus */
    };

    static string runOpToString(RunOperation op);

    RunOperation m_runOperation;

    /** listener to listen to changes of sync */
    SessionListener *m_listener;

    /** Cmdline to execute command line args */
    boost::shared_ptr<CmdlineWrapper> m_cmdline;

    /** Session.GetStatus() */
    void getStatus(std::string &status,
                   uint32_t &error,
                   SessionCommon::SourceStatuses_t &sources);
    /** Session.GetProgress() */
    void getProgress(int32_t &progress,
                     SessionCommon::SourceProgresses_t &sources);

    /** Session.Restore() */
    void restore(const string &dir, bool before,const std::vector<std::string> &sources);

    /** Session.Execute() */
    void execute(const vector<string> &args, const map<string, string> &vars);

    std::string m_passwordReqResponse;
    SessionCommon::PwRespStatus m_pwResponseStatus;

    /**
     * Must be called each time that properties changing the
     * overall status are changed. Ensures that the corresponding
     * D-Bus signal is sent.
     *
     * Doesn't always send the signal immediately, because often it is
     * likely that more status changes will follow shortly. To ensure
     * that the "final" status is sent, call with flush=true.
     *
     * @param flush      force sending the current status
     */
    void fireStatus(bool flush = false);
    /** like fireStatus() for progress information */
    void fireProgress(bool flush = false);

    /** Session.StatusChanged */
    GDBusCXX::EmitSignal3<const std::string &,
                          uint32_t,
                          const SessionCommon::SourceStatuses_t &> emitStatus;
    /** Session.ProgressChanged */
    GDBusCXX::EmitSignal2<int32_t,
                          const SessionCommon::SourceProgresses_t &> emitProgress;

    GDBusCXX::EmitSignal0 emitDone;
    GDBusCXX::EmitSignal1<const std::map<std::string, std::string> &> emitPasswordRequest;

    static string syncStatusToString(SessionCommon::SyncStatus state);

public:
    /**
     * Sessions must always be held in a shared pointer
     * because some operations depend on that. This
     * constructor function here ensures that and
     * also adds a weak pointer to the instance itself,
     * so that it can create more shared pointers as
     * needed.
     */
    static boost::shared_ptr<SessionImpl> createSession(GMainLoop *loop,
                                                        bool &shutdownRequested,
                                                        const GDBusCXX::DBusConnectionPtr &conn,
                                                        const std::string &config_name,
                                                        const std::string &session,
                                                        const std::vector<std::string> &flags = std::vector<std::string>());

    /**
     * automatically marks the session as completed before deleting it
     */
    ~SessionImpl();

    /** access to the GMainLoop reference used by this Session instance */
    GMainLoop *getLoop() { return m_loop; }

    /** explicitly notify Session we're done. */
    void done() { emitDone(); }

private:
    SessionImpl(GMainLoop *loop,
                bool &shutdownRequested,
                const GDBusCXX::DBusConnectionPtr &conn,
                const std::string &config_name,
                const std::string &session,
                const std::vector<std::string> &flags = std::vector<std::string>());

public:
    /**
     * Notifies the helper session that the server is shutting down.
     */
    void serverShutdown();

    bool isServerAlerted() const { return m_serverAlerted; }
    void setServerAlerted(bool serverAlerted) { m_serverAlerted = serverAlerted; }

    void initServer(SharedBuffer data, const std::string &messageType);
    void setStubConnection(const boost::shared_ptr<ConnectionImpl> c) { m_connection = c; m_useConnection = c; }
    boost::weak_ptr<ConnectionImpl> getStubConnection() { return m_connection; }
    bool useStubConnection() { return m_useConnection; }

    /**
     * After the connection closes, the Connection instance is
     * destructed immediately. This is necessary so that the
     * corresponding cleanup can remove all other classes
     * only referenced by the Connection.
     *
     * This leads to the problem that an active sync cannot
     * query the final error code of the connection. This
     * is solved by setting a generic error code here when
     * the sync starts and overwriting it when the connection
     * closes.
     */
    void setStubConnectionError(const std::string error) { m_connectionError = error; }
    std::string getStubConnectionError() { return m_connectionError; }

    std::string getConfigName() { return m_configName; }
    std::string getSessionID() const { return m_sessionID; }

    /**
     * TRUE if the session is ready to take over control
     */
    bool readyToRun() { return (m_syncStatus != SessionCommon::SYNC_DONE) && (m_runOperation != OP_NULL); }

    /**
     * transfer control to the session for the duration of the sync,
     * returns when the sync is done (successfully or unsuccessfully)
     */
    void run(LogRedirect &redirect);

    /**
     * called when the session is ready to run (true) or
     * lost the right to make changes (false)
     */
    void setActive(bool active);

    bool getActive() { return m_active; }

    /**
     * This will be true if the server has called ServerShutdown
     */
    bool getShutdownRequested() { return m_shutdownRequested; }

    void syncProgress(sysync::TProgressEventEnum type,
                      int32_t extra1, int32_t extra2, int32_t extra3);
    void sourceProgress(sysync::TProgressEventEnum type,
                        SyncSource &source,
                        int32_t extra1, int32_t extra2, int32_t extra3);
    string askPassword(const string &passwordName,
                       const string &descr,
                       const ConfigPasswordKey &key);
    /** One-to-one DBus method PasswordResponse() */
    void passwordResponse(bool timed_out, const std::string &password);

    /** Session.SetConfig() */
    void setConfig(bool update, bool temporary,
                   const ReadOperations::Config_t &config);

    /** Session.SetNamedConfig() */
    void setNamedConfig(bool &setConfig, const std::string &configName, bool update,
                        bool temporary, const ReadOperations::Config_t &config);

    /** Session.Sync() */
    void sync(const std::string &mode, const SessionCommon::SourceModes_t &source_modes);
    /** Session.Abort() */
    void abort();
    /** Session.Suspend() */
    void suspend();

    /**
     * step info for engine: whether the engine is blocked by something
     * If yes, 'waiting' will be appended as specifiers in the status string.
     * see GetStatus documentation.
     */
    void setStepInfo(bool isWaiting);

    /** sync is successfully started */
    void syncSuccessStart();

    /**
     * add a listener of the session. Old set listener is returned
     */
    SessionListener* addListener(SessionListener *listener);

    void setRemoteInitiated (bool remote) { m_remoteInitiated = remote;}

private:
    /** set m_syncFilter and m_sourceFilters to config */
    virtual bool setFilters(SyncConfig &config);
};

SE_END_CXX

#endif // SESSION_IMPL_H
