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
#include "locale-factory.h"
#include "../server.h"
#include <syncevo/EDSClient.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * Implementation of org._01.pim.contacts.Manager.
 */
class Manager : public GDBusCXX::DBusObjectHelper
{
    GThread *m_mainThread;
    boost::weak_ptr<Manager> m_self;
    boost::shared_ptr<Server> m_server;
    boost::shared_ptr<IndividualAggregator> m_folks;
    boost::shared_ptr<LocaleFactory> m_locale;
    /** Stores "sort" property in XDG ~/.config/syncevolution/pim-manager.ini'. */
    boost::shared_ptr<ConfigNode> m_configNode;
    std::string m_sortOrder;
    Bool m_preventingAutoTerm;

    /**
     * Contains the EDS UUIDs of all address books contributing to the current
     * unified address book.
     */
    std::set<std::string> m_enabledEBooks;

    typedef std::list< std::pair< boost::shared_ptr<GDBusCXX::Result>, boost::shared_ptr<Session> > > Pending_t;
    /** holds the references to pending session requests, see runInSession() */
    Pending_t m_pending;

    Manager(const boost::shared_ptr<Server> &server);
    void init();
    void initFolks();
    void initDatabases();
    void initSorting(const std::string &order);

 public:
    /** Manager.Start() */
    void start();
    /** Manager.Stop() */
    void stop();
    /** Manager.IsRunning() */
    bool isRunning();
    /** Manager.SetSortOrder() */
    void setSortOrder(const std::string &order);
    /** Manager.GetSortOrder() */
    std::string getSortOrder();
    /** Manager.Search() */
    void search(const boost::shared_ptr< GDBusCXX::Result1<GDBusCXX::DBusObject_t> > &result,
                const GDBusCXX::Caller_t &ID,
                const boost::shared_ptr<GDBusCXX::Watch> &watch,
                const LocaleFactory::Filter_t &filter,
                const GDBusCXX::DBusObject_t &agentPath);
 private:
    void searchWithRegistry(const ESourceRegistryCXX &registry,
                            const GError *gerror,
                            const boost::shared_ptr< GDBusCXX::Result1<GDBusCXX::DBusObject_t> > &result,
                            const GDBusCXX::Caller_t &ID,
                            const boost::shared_ptr<GDBusCXX::Watch> &watch,
                            const LocaleFactory::Filter_t &filter,
                            const GDBusCXX::DBusObject_t &agentPath) throw();
    void doSearch(const ESourceRegistryCXX &registry,
                  const boost::shared_ptr< GDBusCXX::Result1<GDBusCXX::DBusObject_t> > &result,
                  const GDBusCXX::Caller_t &ID,
                  const boost::shared_ptr<GDBusCXX::Watch> &watch,
                  const LocaleFactory::Filter_t &filter,
                  const GDBusCXX::DBusObject_t &agentPath);

 public:
    /** Manager.GetActiveAddressBooks() */
    void getActiveAddressBooks(std::vector<std::string> &dbIDs);

    /** Manager.SetActiveAddressBooks() */
    void setActiveAddressBooks(const std::vector<std::string> &dbIDs);

    /** Manager.CreatePeer() */
    void createPeer(const boost::shared_ptr<GDBusCXX::Result0> &result,
                    const std::string &uid, const StringMap &properties);
    /** Manager.ModifyPeer() */
    void modifyPeer(const boost::shared_ptr<GDBusCXX::Result0> &result,
                    const std::string &uid, const StringMap &properties);
 private:
    enum ConfigureMode {
        SET_PEER,
        CREATE_PEER,
        MODIFY_PEER
    };

    void setPeer(const boost::shared_ptr<GDBusCXX::Result0> &result,
                 const std::string &uid, const StringMap &properties,
                 ConfigureMode mode);
    void doSetPeer(const boost::shared_ptr<Session> &session,
                   const boost::shared_ptr<GDBusCXX::Result0> &result,
                   const std::string &uid, const StringMap &properties,
                   ConfigureMode mode);

 public:
    /** Manager.RemovePeer() */
    void removePeer(const boost::shared_ptr<GDBusCXX::Result0> &result,
                    const std::string &uid);

 private:
    void doRemovePeer(const boost::shared_ptr<Session> &session,
                      const boost::shared_ptr<GDBusCXX::Result0> &result,
                      const std::string &uid);

 public:
    /** Manager.SyncPeer() */
    void syncPeer(const boost::shared_ptr<GDBusCXX::Result0> &result,
                  const std::string &uid);
 private:
    void doSyncPeer(const boost::shared_ptr<Session> &session,
                    const boost::shared_ptr<GDBusCXX::Result0> &result,
                    const std::string &uid);

 public:
    /** Manager.StopSync() */
    void stopSync(const boost::shared_ptr<GDBusCXX::Result0> &result,
                  const std::string &uid);

    typedef std::map<std::string, StringMap> PeersMap;
    /** Manager.GetAllPeers() */
    PeersMap getAllPeers();

    /** Manager.AddContact() */
    void addContact(const boost::shared_ptr< GDBusCXX::Result1<std::string> > &result,
                    const std::string &addressbook,
                    const PersonaDetails &details);
    /** Manager.ModifyContact() */
    void modifyContact(const boost::shared_ptr<GDBusCXX::Result0> &result,
                       const std::string &addressbook,
                       const std::string &localID,
                       const PersonaDetails &details);
    /** Manager.RemoveContact() */
    void removeContact(const boost::shared_ptr<GDBusCXX::Result0> &result,
                       const std::string &addressbook,
                       const std::string &localID);

 private:
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

    /** true if the current thread is the one handling the event loop and running all operations */
    bool isMain() { return g_thread_self() == m_mainThread; }

    /**
     * Runs the operation inside the main thread and returns once the
     * main thread is done with it.
     */
    void runInMainVoid(const boost::function<void ()> &operation);
    template <class R> R runInMainRes(const boost::function<R ()> &operation);

    void runInMainV(void (Manager::*method)()) { runInMainVoid(boost::bind(method, this)); }
    template <class R> R runInMainR(R (Manager::*method)()) { return runInMainRes<R>(boost::bind(method, this)); }
    template <class A1, class B1> void runInMainV(void (Manager::*method)(B1), A1 a1) { runInMainVoid(boost::bind(method, this, a1)); }
    template <class R, class A1, class B1> R runInMainR(R (Manager::*method)(B1), A1 a1) { return runInMainRes<R>(boost::bind(method, this, a1)); }

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
