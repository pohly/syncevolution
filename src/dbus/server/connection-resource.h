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

#ifndef CONNECTION_RESOURCE_H
#define CONNECTION_RESOURCE_H

#include "resource.h"
#include "read-operations.h"

#include <syncevo/ForkExec.h>

SE_BEGIN_CXX

class Server;

class ConnectionProxy : public GDBusCXX::DBusRemoteObject
{
public:
  ConnectionProxy(const GDBusCXX::DBusConnectionPtr &conn, const std::string &sessionID) :
    GDBusCXX::DBusRemoteObject(conn.get(),
                               "/dbushelper",
                               std::string("dbushelper.Connection") + sessionID,
                               "direct.peer",
                               true), // This is a one-to-one connection. Close it.
         m_process     (*this, "Process"),
         m_close       (*this, "Close"),
         m_reply       (*this, "Reply", false),
         m_abort       (*this, "Abort", false),
         m_shutdown    (*this, "Shutdown", false),
         m_killSessions(*this, "KillSessions", false)
    {}

    GDBusCXX::DBusClientCall0                   m_process;
    GDBusCXX::DBusClientCall0                   m_close;
    GDBusCXX::SignalWatch5<const GDBusCXX::DBusArray<uint8_t> &,
                           const std::string &,
                           const StringMap &,
                           bool,
                           const std::string &> m_reply;
    GDBusCXX::SignalWatch0                      m_abort;
    GDBusCXX::SignalWatch0                      m_shutdown;
    GDBusCXX::SignalWatch1<std::string>         m_killSessions;
};

/**
 * The ConnectionResource is held by the Server and facilitates
 * communication between the Server and Connection which runs in a
 * seperate binary.
 */
class ConnectionResource : public GDBusCXX::DBusObjectHelper,
                           public Resource
{
    std::string m_path;

    StringMap m_peer;
    const std::string m_sessionID;
    bool m_mustAuthenticate;

    /** Connection.Process */
    void process(const GDBusCXX::Caller_t &caller, const GDBusCXX::DBusArray<uint8_t> &msg,
                 const std::string &msgType,
                 const boost::shared_ptr<GDBusCXX::Result0> &result);

    /** Connection.Close */
    void close(const GDBusCXX::Caller_t &caller, bool normal, const std::string &error,
               const boost::shared_ptr<GDBusCXX::Result0> &result);

    GDBusCXX::EmitSignal0 emitAbort;
    bool m_abortSent;
    /** Connection.Reply */
    GDBusCXX::EmitSignal5<const GDBusCXX::DBusArray<uint8_t> &,
                          const std::string &,
                          const StringMap &,
                          bool,
                          const std::string &> emitReply;

    /** Signal callbacks */
    void replyCb(const GDBusCXX::DBusArray<uint8_t> &reply, const std::string &replyType,
                 const StringMap &meta, bool final, const std::string &session);
    void abortCb();
    void shutdownCb();
    void killSessionsCb(const std::string &peerDeviceId);

    boost::shared_ptr<SyncEvo::ForkExecParent> m_forkExecParent;
    boost::scoped_ptr<ConnectionProxy> m_connectionProxy;

    // Child process handlers
    void onConnect(const GDBusCXX::DBusConnectionPtr &conn);
    void onQuit(int status);
    void onFailure(const std::string &error);

    /**
     * returns "<description> (<ID> via <transport> <transport_description>)"
     */
    static std::string buildDescription(const StringMap &peer);

public:
    const std::string m_description;

    ConnectionResource(Server &server,
                       const std::string &session_num,
                       const StringMap &peer,
                       bool must_authenticate);

    ~ConnectionResource();

    /**
     * Set up the helper and connection to it. Wait until Conection
     * interface is usable.
     */
    void init();

    /** peer is not trusted, must authenticate as part of SyncML */
    bool mustAuthenticate() const { return m_mustAuthenticate; }
};

SE_END_CXX

#endif
