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

#ifndef SERVER_READ_OPERATIONS_H
#define SERVER_READ_OPERATIONS_H

#include "read-operations.h"

SE_BEGIN_CXX

class Server;

/**
 * Implements the read-only methods in a Server.
 * Only data is the server configuration name, everything else
 * is created and destroyed inside the methods.
 */
class ServerReadOperations : public ReadOperations
{
public:
    ServerReadOperations(const std::string &config_name, Server &server);

    /** implementation of D-Bus GetConfigs() */
    void getConfigs(bool getTemplates, std::vector<std::string> &configNames);
private:
    Server &m_server;
};

SE_END_CXX

#endif // SERVER_READ_OPERATIONS_H
