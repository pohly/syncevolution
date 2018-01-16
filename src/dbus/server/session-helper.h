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

#ifndef SESSION_HELPER_H
#define SESSION_HELPER_H

#include "session-common.h"
#include "dbus-sync.h"

#include <gdbus-cxx-bridge.h>
#include <glib.h>

#include <memory>
#include <functional>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

class LogRedirect;
class ForkExecChild;
class SessionHelperLogger;

/**
 * Waits for requests via the internal D-Bus connection in run(), sent
 * by the Session class in syncevo-dbus-server. Then for each request
 * it remembers what to do in m_operation and returns from the event
 * loop and executes the requested operation, pretty much like the
 * traditional syncevo-dbus-server did.
 */
class SessionHelper : public GDBusCXX::DBusObjectHelper,
    private boost::noncopyable
{
    GMainLoop *m_loop;
    GDBusCXX::DBusConnectionPtr m_conn;
    std::shared_ptr<ForkExecChild> m_forkexec;
    std::function<bool ()> m_operation;
    std::shared_ptr<SessionHelperLogger> m_logger;
    PushLogger<Logger> m_pushLogger;

    /** valid during doSync() */
    boost::scoped_ptr<DBusSync> m_sync;

    /** called by main event loop: initiate a sync operation */
    void sync(const SessionCommon::SyncParams &params,
              const std::shared_ptr< GDBusCXX::Result<bool, SyncReport> > &result);

    /**
     * called by run(): do the sync operation
     * @return true if the helper is meant to terminate
     */
    bool doSync(const SessionCommon::SyncParams &params,
                const std::shared_ptr< GDBusCXX::Result<bool, SyncReport> > &result);

    void restore(const std::string &configName,
                 const string &dir, bool before, const std::vector<std::string> &sources,
                 const std::shared_ptr< GDBusCXX::Result<bool> > &result);
    bool doRestore(const std::string &configName,
                   const string &dir, bool before, const std::vector<std::string> &sources,
                   const std::shared_ptr< GDBusCXX::Result<bool> > &result);

    void execute(const vector<string> &args, const map<string, string> &vars,
                 const std::shared_ptr< GDBusCXX::Result<bool> > &result);
    bool doExecute(const vector<string> &args, const map<string, string> &vars,
                   const std::shared_ptr< GDBusCXX::Result<bool> > &result);

    /** SessionHelper.PasswordResponse */
    void passwordResponse(bool timedOut, bool aborted, const std::string &password);

    /** SessionHelper.SetFreeze */
    bool setFreeze(bool frozen);

 public:
    SessionHelper(GMainLoop *loop,
                  const GDBusCXX::DBusConnectionPtr &conn,
                  const std::shared_ptr<ForkExecChild> &forkexec);
    ~SessionHelper();

    void setDBusLogLevel(Logger::Level level);
    Logger::Level getDBusLogLevel() const;

    void activate();
    void run();
    GMainLoop *getLoop() const { return m_loop; }

    /** Still have connection to parent. Shortcut which asks the ForkExecChild class. */
    bool connected();

    std::shared_ptr<ForkExecChild> getForkExecChild() { return m_forkexec; }

    /** Server.LogOutput for the session D-Bus object */
    GDBusCXX::EmitSignalOptional<std::string,
        std::string, std::string> emitLogOutput;

    /** SyncContext::displaySyncProgress */
    GDBusCXX::EmitSignalOptional<sysync::TProgressEventEnum,
        int32_t, int32_t, int32_t> emitSyncProgress;

    /** SyncContext::displaySourceProgress */
    GDBusCXX::EmitSignalOptional<sysync::TProgressEventEnum,
        std::string, SyncMode,
        int32_t, int32_t, int32_t> emitSourceProgress;

    /** SyncContext::m_sourceSyncedSignal */
    GDBusCXX::EmitSignalOptional<std::string, SyncSourceReport> emitSourceSynced;

    /** SyncContext::reportStepCmd -> true/false for "waiting on IO" */
    GDBusCXX::EmitSignalOptional<bool> emitWaiting;

    /** SyncContext::syncSuccessStart */
    GDBusCXX::EmitSignalOptional<> emitSyncSuccessStart;

    /** Cmdline::configWasModified() */
    GDBusCXX::EmitSignalOptional<> emitConfigChanged;

    /** SyncContext::askPassword */
    GDBusCXX::EmitSignal<std::string, ConfigPasswordKey> emitPasswordRequest;

    /** send message to parent's connection (buffer, type, url) */
    GDBusCXX::EmitSignal<GDBusCXX::DBusArray<uint8_t>, std::string, std::string> emitMessage;

    /** tell parent's connection to shut down */
    GDBusCXX::EmitSignal<> emitShutdown;

    /** store the next message received by the session's connection */
    void storeMessage(const GDBusCXX::DBusArray<uint8_t> &message,
                      const std::string &type) {
        m_messageSignal(message, type);
    }
    typedef boost::signals2::signal<void (const GDBusCXX::DBusArray<uint8_t> &,
                                          const std::string &)> MessageSignal_t;
    MessageSignal_t m_messageSignal;

    /** store the latest connection state information */
    void connectionState(const std::string &error)
    {
        m_connectionStateSignal(error);
    }
    typedef boost::signals2::signal<void (const std::string &)> ConnectionStateSignal_t;
    ConnectionStateSignal_t m_connectionStateSignal;
};

SE_END_CXX

#endif // SESSION_HELPER_H
