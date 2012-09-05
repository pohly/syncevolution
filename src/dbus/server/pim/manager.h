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

/**
 * The D-Bus IPC binding for folks.h and SyncEvolution's
 * PBAP support.
 */

#ifndef INCL_SYNCEVO_DBUS_SERVER_PIM_MANAGER
#define INCL_SYNCEVO_DBUS_SERVER_PIM_MANAGER

#include "folks.h"
#include "../server.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * Implementation of org._01.pim.contacts.Manager.
 */
class Manager : public GDBusCXX::DBusObjectHelper
{
    boost::weak_ptr<Manager> m_self;
    boost::shared_ptr<Server> m_server;
    boost::shared_ptr<IndividualAggregator> m_folks;
    std::string m_sortOrder;

    Manager(const boost::shared_ptr<Server> &server);
    void init();
    void initFolks();
    void initSorting();

    /** Manager.Start() */
    void start();
    /** Manager.Stop() */
    void stop();
    /** Manager.SetSortOrder() */
    void setSortOrder(const std::string &order);
    /** Manager.GetSortOrder() */
    std::string getSortOrder() { return m_sortOrder; }
    /** Manager.Search() */
    GDBusCXX::DBusObject_t search(const GDBusCXX::Caller_t &ID,
                                  const boost::shared_ptr<GDBusCXX::Watch> &watch,
                                  const StringMap &filter,
                                  const GDBusCXX::DBusObject_t &agentPath);

 public:
    /**
     * Creates an instance of the Manager which runs as part
     * of the given server, using the same D-Bus connection
     * and using the server's services (tracking clients,
     * startup/shutdown).
     *
     * While the Manager exists, it blocks auto-termination
     * of the server and serves method calls as part of the
     * main event loop.
     */
    static boost::shared_ptr<Manager> create(const boost::shared_ptr<Server> &server);

    ~Manager();
};

SE_END_CXX

#endif // INCL_SYNCEVO_DBUS_SERVER_PIM_MANAGER
