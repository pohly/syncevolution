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

#ifndef CONNECTION_H
#define CONNECTION_H

#include "session.h"

SE_BEGIN_CXX

/**
 * Represents and implements the Connection interface.
 *
 * The connection interacts with a Session by creating the Session and
 * exchanging data with it. For that, the connection registers itself
 * with the Session and unregisters again when it goes away.
 *
 * In contrast to clients, the Session only keeps a weak_ptr, which
 * becomes invalid when the referenced object gets deleted. Typically
 * this means the Session has to abort, unless reconnecting is
 * supported.
 */
class Connection : public GDBusCXX::DBusObjectHelper
{
    StringMap m_peer;
    bool m_mustAuthenticate;

    std::string m_failure;

    /**
     * True if a shutdown signal was received.
     */
    bool m_shutdownRequested;

    /** first parameter for Session::sync() */
    std::string m_syncMode;
    /** second parameter for Session::sync() */
    SessionCommon::SourceModes_t m_sourceModes;

    const std::string m_sessionID;
    boost::shared_ptr<Session> m_session;

    /**
     * main loop that our DBusTransportAgent is currently waiting in,
     * NULL if not waiting
     */
    GMainLoop *m_loop;

    /**
     * get our peer session out of the DBusTransportAgent,
     * if it is currently waiting for us (indicated via m_loop)
     */
    void wakeupSession();

    /**
     * buffer for received data, waiting here for engine to ask
     * for it via DBusTransportAgent::getReply().
     */
    SharedBuffer m_incomingMsg;
    std::string m_incomingMsgType;

    struct SANContent {
        std::vector <string> m_syncType;
        std::vector <uint32_t> m_contentType;
        std::vector <string> m_serverURI;
    };

    /**
     * The content of a parsed SAN package to be processed via
     * connection.ready
     */
    boost::shared_ptr <SANContent> m_SANContent;
    std::string m_peerBtAddr;

    /**
     * records the reason for the failure, sends Abort signal and puts
     * the connection into the FAILED state.
     */
    void failed(const std::string &reason);

    /** Connection.Process() */
    void process(const GDBusCXX::DBusArray<uint8_t> &message,
                 const std::string &message_type,
                 const StringMap &peer,
                 bool must_authenticate);
    /** Connection.Close() */
    void close(bool normal,
               const std::string &error);
    /** wrapper around emitAbort */
    void abort();
    /** Connection.Abort */
    GDBusCXX::EmitSignal0 emitAbort;
    bool m_abortSent;
    /** Connection.Reply */
    GDBusCXX::EmitSignal5<const GDBusCXX::DBusArray<uint8_t> &,
                          const std::string &,
                          const StringMap &,
                          bool,
                          const std::string &> emitReply;
    GDBusCXX::EmitSignal0 emitShutdown;
    GDBusCXX::EmitSignal1<std::string> emitKillSessions;

    friend class DBusTransportAgent;

public:
    /**
     * Connections must always be held in a shared pointer to ensure
     * that we have a weak pointer to the instance itself, so
     * that it can create more shared pointers as needed. This is
     * needed, for instance, to set the session's connection stub.
     */
    static boost::shared_ptr<Connection> createConnection(GMainLoop *loop,
                                                          bool &shutdownRequested,
                                                          const GDBusCXX::DBusConnectionPtr &conn,
                                                          const std::string &sessionID);

    enum State {
        SETUP,          /**< ready for first message */
        PROCESSING,     /**< received message, waiting for engine's reply */
        WAITING,        /**< waiting for next follow-up message */
        FINAL,          /**< engine has sent final reply, wait for ACK by peer */
        DONE,           /**< peer has closed normally after the final reply */
        FAILED          /**< in a failed state, no further operation possible */
    };

private:
    Connection(GMainLoop *loop,
               bool &shutdownRequested,
               const GDBusCXX::DBusConnectionPtr &conn,
               const std::string &sessionID);
    boost::weak_ptr<Connection> m_me;

    State m_state;

public:
    ~Connection();

    State getState() { return m_state; }

    /** session requested by us is ready to run a sync */
    void ready();

    bool isSessionReadyToRun();

    void runSession(LogRedirect &redirect);

    void shutdown();

    /** peer is not trusted, must authenticate as part of SyncML */
    bool mustAuthenticate() const { return m_mustAuthenticate; }
};

SE_END_CXX

#endif // CONNECTION_H
