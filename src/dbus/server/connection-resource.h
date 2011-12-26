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

SE_BEGIN_CXX

class Server;

/**
 * The ConnectionResource is held by the Server and facilitates
 * communication between the Server and Connection which runs in a
 * seperate binary.
 */
class ConnectionResource : public Resource
{
    Server &m_server;    
    std::string m_path;

    StringMap m_peer;
    const std::string m_sessionID;
    bool m_mustAuthenticate;

    /**
     * returns "<description> (<ID> via <transport> <transport_description>)"
     */
    static std::string buildDescription(const StringMap &peer);

public:
    const std::string m_description;

    const char *getPath() const { return m_path.c_str(); }

    ConnectionResource(Server &server,
                       const std::string &session_num,
                       const StringMap &peer,
                       bool must_authenticate);

    ~ConnectionResource();

    /** peer is not trusted, must authenticate as part of SyncML */
    bool mustAuthenticate() const { return m_mustAuthenticate; }
};

SE_END_CXX

#endif
