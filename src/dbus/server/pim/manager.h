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
    /**
     * Contains the UIDs of all peers contributing to the current
     * unified address book.
     */
    std::set<std::string> m_enabledPeers;

    typedef std::list< std::pair< boost::shared_ptr<GDBusCXX::Result>, boost::shared_ptr<Session> > > Pending_t;
    /** holds the references to pending session requests, see runInSession() */
    Pending_t m_pending;

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

    /** Manager.SetPeer() */
    void setPeer(const boost::shared_ptr<GDBusCXX::Result0> &result,
                 const std::string &uid, const StringMap &properties);
    void doSetPeer(const boost::shared_ptr<Session> &session,
                   const boost::shared_ptr<GDBusCXX::Result0> &result,
                   const std::string &uid, const StringMap &properties);

    /** Manager.RemovePeer() */
    void removePeer(const boost::shared_ptr<GDBusCXX::Result0> &result,
                    const std::string &uid);
    void doRemovePeer(const boost::shared_ptr<Session> &session,
                      const boost::shared_ptr<GDBusCXX::Result0> &result,
                      const std::string &uid);

    /** Manager.SyncPeer() */
    void syncPeer(const boost::shared_ptr<GDBusCXX::Result0> &result,
                  const std::string &uid);
    void doSyncPeer(const boost::shared_ptr<Session> &session,
                    const boost::shared_ptr<GDBusCXX::Result0> &result,
                    const std::string &uid);

    /** Manager.StopSync() */
    void stopSync(const boost::shared_ptr<GDBusCXX::Result0> &result,
                  const std::string &uid);

    typedef std::map<std::string, StringMap> PeersMap;
    /** Manager.GetAllPeers() */
    PeersMap getAllPeers();

    /**
     * Starts a session for the given config and with the
     * given flags, then when it is active, invokes the callback.
     * Failures will be reported back to via the result
     * pointer.
     */
    void runInSession(const std::string &config,
                      Server::SessionFlags flags,
                      const boost::shared_ptr<GDBusCXX::Result> &result,
                      const boost::function<void (const boost::shared_ptr<Session> &session)> &callback);

    /**
     * Common boilerplate code for anything that runs inside
     * an active session in response to some D-Bus method call
     * (like doSetPeer).
     */
    void doSession(const boost::weak_ptr<Session> &session,
                   const boost::shared_ptr<GDBusCXX::Result> &result,
                   const boost::function<void (const boost::shared_ptr<Session> &session)> &callback);

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
