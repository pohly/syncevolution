/*
 * Copyright(C) 2011 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or(at your option) version 3.
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

#include "server.h"
#include "client.h"
#include "connection-resource.h"

#include <synthesis/san.h>
#include <syncevo/TransportAgent.h>
#include <syncevo/SyncContext.h>

#include <boost/lexical_cast.hpp>

using namespace GDBusCXX;

SE_BEGIN_CXX

std::string ConnectionResource::buildDescription(const StringMap &peer)
{
    StringMap::const_iterator
        desc = peer.find("description"),
        id = peer.find("id"),
        trans = peer.find("transport"),
        trans_desc = peer.find("transport_description");
    std::string buffer;
    buffer.reserve(256);
    if (desc != peer.end()) {
        buffer += desc->second;
    }
    if (id != peer.end() || trans != peer.end()) {
        if (!buffer.empty()) {
            buffer += " ";
        }
        buffer += "(";
        if (id != peer.end()) {
            buffer += id->second;
            if (trans != peer.end()) {
                buffer += " via ";
            }
        }
        if (trans != peer.end()) {
            buffer += trans->second;
            if (trans_desc != peer.end()) {
                buffer += " ";
                buffer += trans_desc->second;
            }
        }
        buffer += ")";
    }
    return buffer;
}

void ConnectionResource::process(const Caller_t &caller,
                                 const GDBusCXX::DBusArray<uint8_t> &msg,
                                 const std::string &msgType)
{
    boost::shared_ptr<Client> client(m_server.findClient(caller));
    if (!client) {
        throw runtime_error("unknown client");
    }

    boost::shared_ptr<ConnectionResource> myself =
        boost::static_pointer_cast<ConnectionResource, Resource>(client->findResource(this));
    if (!myself) {
        throw runtime_error("client does not own connection");
    }

    resetReplies();
    // Passing in the peer and mustAuthenticate to complete connection initialization.
    m_connectionProxy->m_process.block(msg, msgType, m_peer, m_mustAuthenticate,
                                       boost::bind(&ConnectionResource::processCb, this, _1));

    if(!m_result) {
        throwExceptionFromString(m_resultError);
    }
}

void ConnectionResource::processCb(const string &error)
{
    if(setResult(error)) {
        SE_LOG_INFO(NULL, NULL, "Connection.Process callback successfull");
    }
    replyInc();
}

void ConnectionResource::close(const GDBusCXX::Caller_t &caller, bool normal, const std::string &error)
{
    SE_LOG_DEBUG(NULL, NULL, "D-Bus client %s closes connection %s %s%s%s",
                 caller.c_str(),
                 getPath(),
                 normal ? "normally" : "with error",
                 error.empty() ? "" : ": ",
                 error.c_str());

    boost::shared_ptr<Client> client(m_server.findClient(caller));
    if (!client) {
        throw runtime_error("unknown client");
    }

    resetReplies();
    m_connectionProxy->m_close.block(normal, error,
                                     boost::bind(&ConnectionResource::closeCb, this, client, _1));

    if(!m_result) {
        throwExceptionFromString(m_resultError);
    }
}

void ConnectionResource::closeCb(boost::shared_ptr<Client> &client, const string &error)
{
    if(setResult(error)) {
    SE_LOG_INFO(NULL, NULL, "Connection.Close callback successfull");
    }
    replyInc();

    client->detach(this);
}

void ConnectionResource::replyCb(const GDBusCXX::DBusArray<uint8_t> &reply, const std::string &replyType,
                                 const StringMap &meta, bool final, const std::string &session)
{
    SE_LOG_INFO(NULL, NULL, "Connection.Reply signal received: replyType=%s, final=%s, session=%s",
                replyType.c_str(), final ? "T" : "F", session.c_str());
    return;
}

void ConnectionResource::shutdownCb()
{
    SE_LOG_INFO(NULL, NULL, "Connection.Shutdown signal received: detaching connection from server.");
    m_server.detach(this);
    return;
}

void ConnectionResource::killSessionsCb(const string &peerDeviceId)
{
    SE_LOG_INFO(NULL, NULL, "Connection.KillSessions signal received: peerDeviceId=%s.", peerDeviceId.c_str());
    m_server.killSessions(peerDeviceId);
    return;
}

void ConnectionResource::sendAbortCb()
{
    SE_LOG_INFO(NULL, NULL, "Connection.Abort signal received");
    return;
}

void ConnectionResource::init()
{
    SE_LOG_INFO(NULL, NULL, "ConnectionResource (%s) forking...", getPath());

    m_forkExecParent->m_onConnect.connect(boost::bind(&ConnectionResource::onConnect, this, _1));
    m_forkExecParent->m_onQuit.connect(boost::bind(&ConnectionResource::onQuit, this, _1));
    m_forkExecParent->m_onFailure.connect(boost::bind(&ConnectionResource::onFailure, this, _2));
    m_forkExecParent->addEnvVar("SYNCEVO_START_CONNECTION", "TRUE");
    m_forkExecParent->addEnvVar("SYNCEVO_SESSION_ID", m_sessionID);
    m_forkExecParent->start();

    // Wait for onSessionConnect to be called so that the dbus
    // interface is ready to be used.
    resetReplies();
    waitForReply();
}

void ConnectionResource::onConnect(const GDBusCXX::DBusConnectionPtr &conn)
{
    SE_LOG_INFO(NULL, NULL, "ConnectionProxy interface ending with: %s", m_sessionID.c_str());
    m_connectionProxy.reset(new ConnectionProxy(conn, m_sessionID));

    /* Enable public dbus interface for Connection. */
    activate();
    replyInc(); // Init is waiting on a reply.

    // Activate signal watch on helper signals.
    m_connectionProxy->m_reply.activate       (boost::bind(&ConnectionResource::replyCb,
                                                           this, _1, _2, _3 ,_4, _5));
    m_connectionProxy->m_abort.activate       (boost::bind(&ConnectionResource::sendAbortCb, this));
    m_connectionProxy->m_shutdown.activate    (boost::bind(&ConnectionResource::shutdownCb, this));
    m_connectionProxy->m_killSessions.activate(boost::bind(&ConnectionResource::killSessionsCb, this, _1));

    SE_LOG_INFO(NULL, NULL, "onConnect called in ConnectionResource (path: %s interface: %s)",
                m_connectionProxy->getPath(), m_connectionProxy->getInterface());
}

void ConnectionResource::onQuit(int status)
{
    SE_LOG_INFO(NULL, NULL, "dbus-helper quit with status: %d", status);
}

void ConnectionResource::onFailure(const std::string &error)
{
    SE_LOG_INFO(NULL, NULL, "dbus-helper failed with error: %s", error.c_str());
}

ConnectionResource::ConnectionResource(Server &server,
                                       const std::string &sessionID,
                                       const StringMap &peer,
                                       bool must_authenticate) :
    DBusObjectHelper(server.getConnection(),
                     std::string("/org/syncevolution/Connection/") + sessionID,
                     "org.syncevolution.Connection",
                     boost::bind(&Server::autoTermCallback, &server)),
    m_server(server),
    m_path(std::string("/org/syncevolution/Connection/") + sessionID),
    m_peer(peer),
    m_sessionID(sessionID),
    m_mustAuthenticate(must_authenticate),
    emitAbort(*this, "Abort"),
    m_abortSent(false),
    emitReply(*this, "Reply"),
    m_forkExecParent(SyncEvo::ForkExecParent::create("syncevo-dbus-helper")),
    m_description(buildDescription(peer))
{
    m_priority = Resource::PRI_CONNECTION;
    // FIXME: A Connection is always marked as running for now as we
    // have no way of knowing the state of the session started by the
    // Connection.
    m_isRunning = true;

    add(this, &ConnectionResource::process, "Process");
    add(this, &ConnectionResource::close, "Close");
    add(emitAbort);
    add(emitReply);

    m_server.autoTermRef();
}

ConnectionResource::~ConnectionResource()
{
    m_server.autoTermUnref();
}

SE_END_CXX
