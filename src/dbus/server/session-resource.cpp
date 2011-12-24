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

SE_BEGIN_CXX

void SessionResource::setPriority(int priority)
{
    m_session->setPriority(priority);
}

int SessionResource::getPriority() const
{
    return m_session->getPriority();
}

void SessionResource::startShutdown()
{
    m_session->startShutdown();
}

void SessionResource::shutdownFileModified()
{
    m_session->shutDownFileModified();
}

bool SessionResource::readyToRun()
{
    return m_session->readyToRun();
}

bool SessionResource::getActive()
{
    return m_session->getActive();
}

void SessionResource::setConfig(bool update, bool temporary, const ReadOperations::Config_t &config)
{
    m_session->setConfig(update, temporary, config);
}

void SessionResource::sync(const std::string &mode, const SessionCommon::SourceModes_t &source_modes)
{
    m_session->sync(mode, source_modes);
}

void SessionResource::abort()
{
    m_session->abort();
}

void SessionResource::suspend()
{
    m_session->suspend();
}

SessionListener* SessionResource::addListener(SessionListener *listener)
{
    return m_session->addListener(listener);
}

void SessionResource::activate()
{
    m_session->activate();
}

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
    m_server(server),
    m_session(Session::createSession(this->m_server, peerDeviceID, config_name, session, flags));
    m_flags(flags),
    m_sessionID(session),
    m_peerDeviceID(peerDeviceID),
    m_path(std::string("/org/syncevolution/Session/") + session),
    m_configName(configName)
{
    SE_LOG_DEBUG(NULL, NULL, "session resource %s created", getPath());
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
    m_server.dequeue(this);

    // now tell other clients about config change?
    if (m_setConfig) {
        m_server.configChanged();
    }

    // typically set by m_server.dequeue(), but let's really make sure...
    m_active = false;

    m_done = true;
}

SessionResource::~SessionResource()
{
    SE_LOG_DEBUG(NULL, NULL, "session resource %s deconstructing", getPath());
    done();
}

}

SE_END_CXX
