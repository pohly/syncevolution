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

SE_BEGIN_CXX

/**
 * Handles supplying the session info needed by the server and
 * clients.
 */
class SessionResource : public Resource,
                        private boost::noncopyable
{

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

    const char *getPath() const { return m_path.c_str(); }

    /**
     * Turns session into one which will shut down the server, must
     * be called before enqueing it. Will wait for a certain idle period
     * after file modifications before claiming to be ready for running
     * (see SessionCommon::SHUTDOWN_QUIESCENCE_SECONDS).
     */
    void startShutdown();

    /**
     * Called by server to tell shutdown session that a file was modified.
     * Session uses that to determine when the quiescence period is over.
     */
    void shutdownFileModified();

    std::string getConfigName() { return m_configName; }
    std::string getSessionID() const { return m_sessionID; }
    std::string getPeerDeviceID() const { return m_peerDeviceID; }

    /**
     * TRUE if the session is ready to take over control
     */
    bool readyToRun();

    bool getActive();

    std::vector<std::string> getFlags() { return m_flags; }

    void setConfig(bool update, bool temporary, const ReadOperations::Config_t &config);

    void sync(const std::string &mode, const SessionCommon::SourceModes_t &source_modes);

    void abort();
    void suspend();

    /**
     * add a listener of the session. Old set listener is returned
     */
    SessionListener* addListener(SessionListener *listener);

    void activate();

    bool autoSyncManagerHasTask()        { return m_server.getAutoSyncManager().hasTask(); }
    bool autoSyncManagerHasAutoConfigs() { return m_server.getAutoSyncManager().hasAutoConfigs(); }

    void checkPresenceOfServer(const std::string &server,
                               std::string &status,
                               std::vector<std::string> &transports)
    { m_server.checkPresence(server, status, transports); }

    /** access to the GMainLoop reference used by this Session instance */
    GMainLoop *getLoop() { return m_server.getLoop(); }

private:
    SessionResource(Server &server,
                    const std::string &peerDeviceID,
                    const std::string &config_name,
                    const std::string &session,
                    const std::vector<std::string> &flags = std::vector<std::string>());
    boost::weak_ptr<SessionResource> m_me;

    Server &m_server;

    std::vector<std::string> m_flags;
    const std::string m_sessionID;
    std::string m_peerDeviceID;
    const std::string m_path;

    const std::string m_configName;
    bool m_setConfig;

    /**
     * True once done() was called.
     */
    bool m_done;

    void attach(const GDBusCXX::Caller_t &caller);
    void detach(const GDBusCXX::Caller_t &caller);
};

SE_END_CXX

#endif // SESSION_RESOURCE_H
