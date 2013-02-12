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

#include "manager.h"
#include "individual-traits.h"
#include "persona-details.h"
#include "filtered-view.h"
#include "full-view.h"
#include "merge-view.h"
#include "edsf-view.h"
#include "../resource.h"
#include "../client.h"
#include "../session.h"

#include <syncevo/IniConfigNode.h>
#include <syncevo/BoostHelper.h>

#include <boost/lexical_cast.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/tokenizer.hpp>
#include <deque>

#include <pcrecpp.h>

SE_BEGIN_CXX

static const char * const MANAGER_SERVICE = "org._01.pim.contacts";
static const char * const MANAGER_PATH = "/org/01/pim/contacts";
static const char * const MANAGER_IFACE = "org._01.pim.contacts.Manager";
static const char * const MANAGER_ERROR_ABORTED = "org._01.pim.contacts.Manager.Aborted";
static const char * const MANAGER_ERROR_BAD_STATUS = "org._01.pim.contacts.Manager.BadStatus";
static const char * const AGENT_IFACE = "org._01.pim.contacts.ViewAgent";
static const char * const CONTROL_IFACE = "org._01.pim.contacts.ViewControl";

static const char * const MANAGER_CONFIG_SORT_PROPERTY = "sort";
static const char * const MANAGER_CONFIG_ACTIVE_ADDRESS_BOOKS_PROPERTY = "active";

/**
 * Prefix for peer databases ("peer-<uid>")
 */
static const char * const DB_PEER_PREFIX = "peer-";

/**
 * Name prefix for SyncEvolution config contexts used by PIM manager.
 * Used in combination with the uid string provided by the PIM manager
 * client, like this:
 *
 * eds@pim-manager-<uid> source 'eds' syncs with target-config@pim-manager-<uid>
 * source 'remote' for PBAP.
 *
 * eds@pim-manager-<uid> source 'local' syncs with a SyncML peer directly.
 */
static const char * const MANAGER_PREFIX = "pim-manager-";
static const char * const MANAGER_LOCAL_CONFIG = "eds";
static const char * const MANAGER_LOCAL_SOURCE = "local";
static const char * const MANAGER_REMOTE_CONFIG = "target-config";
static const char * const MANAGER_REMOTE_SOURCE = "remote";

Manager::Manager(const boost::shared_ptr<Server> &server) :
    DBusObjectHelper(server->getConnection(),
                     MANAGER_PATH,
                     MANAGER_IFACE),
    m_mainThread(g_thread_self()),
    m_server(server),
    m_locale(LocaleFactory::createFactory())
{
}

Manager::~Manager()
{
    // Clear the pending queue before self-desctructing, because the
    // entries hold pointers to this instance.
    m_pending.clear();
    if (m_preventingAutoTerm) {
        m_server->autoTermUnref();
    }
}

void Manager::init()
{
    // Restore sort order and active databases.
    m_configNode.reset(new IniFileConfigNode(SubstEnvironment("${XDG_CONFIG_HOME}/syncevolution"),
                                             "pim-manager.ini",
                                             false));
    InitStateString order = m_configNode->readProperty(MANAGER_CONFIG_SORT_PROPERTY);
    m_sortOrder = order.wasSet() ?
        order :
        "last/first";
    InitStateString active = m_configNode->readProperty(MANAGER_CONFIG_ACTIVE_ADDRESS_BOOKS_PROPERTY);
    m_enabledEBooks.clear();
    typedef boost::split_iterator<string::iterator> string_split_iterator;
    BOOST_FOREACH(const std::string &entry,
                  boost::tokenizer< boost::char_separator<char> >(active, boost::char_separator<char>(", \t"))) {
        if (!entry.empty()) {
            m_enabledEBooks.insert(entry);
        }
    }
    initFolks();
    initSorting(m_sortOrder);
    initDatabases();

    add(this, &Manager::start, "Start");
    add(this, &Manager::stop, "Stop");
    add(this, &Manager::isRunning, "IsRunning");
    add(this, &Manager::setSortOrder, "SetSortOrder");
    add(this, &Manager::getSortOrder, "GetSortOrder");
    add(this, &Manager::search, "Search");
    add(this, &Manager::getActiveAddressBooks, "GetActiveAddressBooks");
    add(this, &Manager::setActiveAddressBooks, "SetActiveAddressBooks");
    add(this, &Manager::setPeer, "SetPeer");
    add(this, &Manager::removePeer, "RemovePeer");
    add(this, &Manager::syncPeer, "SyncPeer");
    add(this, &Manager::stopSync, "StopSync");
    add(this, &Manager::getAllPeers, "GetAllPeers");
    add(this, &Manager::addContact, "AddContact");
    add(this, &Manager::modifyContact, "ModifyContact");
    add(this, &Manager::removeContact, "RemoveContact");

    // Ready, make it visible via D-Bus.
    activate();

    // Claim MANAGER_SERVICE name on connection.
    // We don't care about the result.
    GDBusCXX::DBusConnectionPtr(getConnection()).ownNameAsync(MANAGER_SERVICE,
                                                              boost::function<void (bool)>());
}

struct TaskForMain
{
    GMutex m_mutex;
    GCond m_cond;
    bool m_done;
    boost::function<void ()> m_operation;
    boost::function<void ()> m_rethrow;

    void runTaskOnIdle()
    {
        g_mutex_lock(&m_mutex);

        // Exceptions must be reported back to the original thread.
        // This is done by serializing them as string, then using the
        // existing Exception::tryRethrow() to turn that string back
        // into an instance of the right class.
        try {
            m_operation();
        } catch (...) {
            std::string explanation;
            Exception::handle(explanation);
            m_rethrow = boost::bind(Exception::tryRethrow, explanation, true);
        }

        // Wake up task.
        m_done = true;
        g_cond_signal(&m_cond);
        g_mutex_unlock(&m_mutex);
    }
};

template <class R> void AssignResult(const boost::function<R ()> &operation,
                                     R &res)
{
    res = operation();
}

template <class R> R Manager::runInMainRes(const boost::function<R ()> &operation)
{
    // Prepare task.
    R res;
    TaskForMain task;
    g_mutex_init(&task.m_mutex);
    g_cond_init(&task.m_cond);
    task.m_done = false;
    task.m_operation = boost::bind(&AssignResult<R>, boost::cref(operation), boost::ref(res));

    // Run in main.
    Timeout timeout;
    timeout.runOnce(-1, boost::bind(&TaskForMain::runTaskOnIdle, &task));
    g_main_context_wakeup(NULL);
    g_mutex_lock(&task.m_mutex);
    while (!task.m_done) {
        g_cond_wait(&task.m_cond, &task.m_mutex);
    }
    g_mutex_unlock(&task.m_mutex);

    // Rethrow exceptions (optional) and return result.
    g_cond_clear(&task.m_cond);
    g_mutex_clear(&task.m_mutex);
    if (task.m_rethrow) {
        task.m_rethrow();
    }
    return res;
}

static int Return1(const boost::function<void ()> &operation)
{
    operation();
    return 1;
}

void Manager::runInMainVoid(const boost::function<void ()> &operation)
{
    runInMainRes<int>(boost::bind(Return1, boost::cref(operation)));
}

void Manager::initFolks()
{
    m_folks = IndividualAggregator::create(m_locale);
}

void Manager::initSorting(const std::string &order)
{
    // Mirror sorting order in m_folks main view.
    // Empty string passes NULL pointer to setCompare(),
    // which chooses the builtin sorting in folks.cpp,
    // independent of the locale plugin.
    boost::shared_ptr<IndividualCompare> compare =
        order.empty() ?
        IndividualCompare::defaultCompare() :
        m_locale->createCompare(order);
    m_folks->setCompare(compare);
}

boost::shared_ptr<Manager> Manager::create(const boost::shared_ptr<Server> &server)
{
    boost::shared_ptr<Manager> manager(new Manager(server));
    manager->m_self = manager;
    manager->init();
    return manager;
}

boost::shared_ptr<GDBusCXX::DBusObjectHelper> CreateContactManager(const boost::shared_ptr<Server> &server, bool start)
{
    boost::shared_ptr<Manager> manager = Manager::create(server);
    if (start) {
        manager->start();
    }
    return manager;
}

void Manager::start()
{
    if (!isMain()) {
        runInMainV(&Manager::start);
        return;
    }

    if (!m_preventingAutoTerm) {
        // Prevent automatic shut down during idle times, because we need
        // to keep our unified address book available.
        m_server->autoTermRef();
        m_preventingAutoTerm = true;
    }
    m_folks->start();
}

void Manager::stop()
{
    if (!isMain()) {
        runInMainV(&Manager::stop);
        return;
    }

    // If there are no active searches, then recreate aggregator.
    // Instead of tracking open views, use the knowledge that an
    // idle server has only two references to the main view:
    // one inside m_folks, one given back to us here.
    if (m_folks->getMainView().use_count() <= 2) {
        SE_LOG_DEBUG(NULL, NULL, "restarting due to Manager.Stop()");
        initFolks();
        initDatabases();
        initSorting(m_sortOrder);

        if (m_preventingAutoTerm) {
            // Allow auto shutdown again.
            m_server->autoTermUnref();
            m_preventingAutoTerm = false;
        }
    }
}

bool Manager::isRunning()
{
    if (!isMain()) {
        return runInMainR(&Manager::isRunning);
    }

    return m_folks->isRunning();
}

void Manager::setSortOrder(const std::string &order)
{
    if (!isMain()) {
        runInMainV(&Manager::setSortOrder, order);
        return;
    }

    if (order == getSortOrder()) {
        // Nothing to do.
        return;
    }

    // String is checked as part of initSorting,
    // only store if parsing succeeds.
    initSorting(order);
    m_configNode->writeProperty(MANAGER_CONFIG_SORT_PROPERTY, InitStateString(order, true));
    m_configNode->flush();
    m_sortOrder = order;
}

std::string Manager::getSortOrder()
{
    if (!isMain()) {
        return runInMainR(&Manager::getSortOrder);
    }

    return m_sortOrder;
}

/**
 * Connects a normal IndividualView to a D-Bus client.
 * Provides the org.01.pim.contacts.ViewControl API.
 */
class ViewResource : public Resource, public GDBusCXX::DBusObjectHelper
{
    static unsigned int m_counter;

    boost::weak_ptr<ViewResource> m_self;
    GDBusCXX::DBusRemoteObject m_viewAgent;
    boost::shared_ptr<IndividualView> m_view;
    boost::shared_ptr<LocaleFactory> m_locale;
    boost::weak_ptr<Client> m_owner;
    struct Change {
        Change() : m_start(0), m_call(NULL) {}

        int m_start;
        std::deque<std::string> m_ids;
        const GDBusCXX::DBusClientCall0 *m_call;
    } m_pendingChange, m_lastChange;
    GDBusCXX::DBusClientCall0 m_quiescent;
    GDBusCXX::DBusClientCall0 m_contactsModified,
        m_contactsAdded,
        m_contactsRemoved;

    ViewResource(const boost::shared_ptr<IndividualView> view,
                 const boost::shared_ptr<LocaleFactory> &locale,
                 const boost::shared_ptr<Client> &owner,
                 GDBusCXX::connection_type *connection,
                 const GDBusCXX::Caller_t &ID,
                 const GDBusCXX::DBusObject_t &agentPath) :
        GDBusCXX::DBusObjectHelper(connection,
                                   StringPrintf("%s/view%d", MANAGER_PATH, m_counter++),
                                   CONTROL_IFACE),
        m_viewAgent(connection,
                    agentPath,
                    AGENT_IFACE,
                    ID),
        m_view(view),
        m_locale(locale),
        m_owner(owner),

        // use ViewAgent interface
        m_quiescent(m_viewAgent, "Quiescent"),
        m_contactsModified(m_viewAgent, "ContactsModified"),
        m_contactsAdded(m_viewAgent, "ContactsAdded"),
        m_contactsRemoved(m_viewAgent, "ContactsRemoved")
    {}

    /**
     * Invokes one of m_contactsModified/Added/Removed. A failure of
     * the asynchronous call indicates that the client is dead and
     * that its view can be purged.
     */
    template<class V> void sendChange(const GDBusCXX::DBusClientCall0 &call,
                                      int start,
                                      const V &ids)
    {
        // Changes get aggregated inside handleChange().
        call.start(getObject(),
                   start,
                   ids,
                   boost::bind(ViewResource::sendDone,
                               m_self,
                               _1,
                               &call == &m_contactsModified ? "ContactsModified()" :
                               &call == &m_contactsAdded ? "ContactsAdded()" :
                               &call == &m_contactsRemoved ? "ContactsRemoved()" : "???",
                               true));
    }

    /**
     * Merge local changes as much as possible, to avoid excessive
     * D-Bus traffic. Pending changes get flushed each time the
     * view reports that it is stable again or contact data
     * needs to be sent back to the client. Flushing in the second
     * case is necessary, because otherwise the client will not have
     * an up-to-date view when the requested data arrives.
     */
    void handleChange(const GDBusCXX::DBusClientCall0 &call,
                      int start, const IndividualData &data)
    {
        FolksIndividual *individual = data.m_individual.get();
        const char *id = folks_individual_get_id(individual);
        SE_LOG_DEBUG(NULL, NULL, "handle change %s: %s, #%d, %s = %s",
                     getPath(),
                     &call == &m_contactsModified ? "modified" :
                     &call == &m_contactsAdded ? "added" :
                     &call == &m_contactsRemoved ? "remove" : "???",
                     start,
                     id,
                     folks_name_details_get_full_name(FOLKS_NAME_DETAILS(individual))
                     );

        int pendingCount = m_pendingChange.m_ids.size();
        if (pendingCount == 0) {
            // Sending a "contact modified" twice for the same range is redundant.
            // If the recipient read the data since the last signal, m_lastChange
            // got cleared and we don't do this optimization.
            if (m_lastChange.m_call == &m_contactsModified &&
                &call == &m_contactsModified &&
                start >= m_lastChange.m_start &&
                start < m_lastChange.m_start + (int)m_lastChange.m_ids.size()) {
                SE_LOG_DEBUG(NULL, NULL, "handle change %s: redundant 'modified' signal, ignore",
                             getPath());

                return;
            }

            // Nothing pending, delay sending.
            m_pendingChange.m_call = &call;
            m_pendingChange.m_start = start;
            m_pendingChange.m_ids.push_back(id);
            SE_LOG_DEBUG(NULL, NULL, "handle change %s: stored as pending change",
                         getPath());
            return;
        }

        if (m_pendingChange.m_call == &call) {
            // Same operation. Can we extend it?
            if (&call == &m_contactsModified) {
                // Modification, indices are unchanged.
                if (start + 1 == m_pendingChange.m_start) {
                    // New modified element at the front.
                    SE_LOG_DEBUG(NULL, NULL, "handle change %s: insert modification, #%d + %d and #%d => #%d + %d",
                                 getPath(),
                                 m_pendingChange.m_start, pendingCount,
                                 start,
                                 m_pendingChange.m_start - 1, pendingCount + 1);
                    m_pendingChange.m_ids.push_front(id);
                    m_pendingChange.m_start--;
                    return;
                } else if (start == m_pendingChange.m_start + pendingCount) {
                    // New modified element at the end.
                    SE_LOG_DEBUG(NULL, NULL, "handle change %s: append modification, #%d + %d and #%d => #%d + %d",
                                 getPath(),
                                 m_pendingChange.m_start, pendingCount,
                                 start,
                                 m_pendingChange.m_start, pendingCount + 1);
                    m_pendingChange.m_ids.push_back(id);
                    return;
                } else if (start >= m_pendingChange.m_start &&
                           start < m_pendingChange.m_start + pendingCount) {
                    // Element modified again => no change, except perhaps for the ID.
                    SE_LOG_DEBUG(NULL, NULL, "handle change %s: modification of already modified contact, #%d + %d and #%d => #%d + %d",
                                 getPath(),
                                 m_pendingChange.m_start, pendingCount,
                                 start,
                                 m_pendingChange.m_start, pendingCount);
                    m_pendingChange.m_ids[start - m_pendingChange.m_start] = id;
                    return;
                }
            } else if (&call == &m_contactsAdded) {
                // Added individuals. The new start index already includes
                // the previously added individuals.
                int newCount = m_pendingChange.m_ids.size() + 1;
                if (start >= m_pendingChange.m_start) {
                    // Adding in the middle or at the end?
                    int end = m_pendingChange.m_start + pendingCount;
                    if (start == end) {
                        SE_LOG_DEBUG(NULL, NULL, "handle change %s: increase count of 'added' individuals at end, #%d + %d and #%d new => #%d + %d",
                                     getPath(),
                                     m_pendingChange.m_start, pendingCount,
                                     start,
                                     m_pendingChange.m_start, newCount);
                        m_pendingChange.m_ids.push_back(id);
                        return;
                    } else if (start < end) {
                        SE_LOG_DEBUG(NULL, NULL, "handle change %s: increase count of 'added' individuals in the middle, #%d + %d and #%d new => #%d + %d",
                                     getPath(),
                                     m_pendingChange.m_start, pendingCount,
                                     start,
                                     m_pendingChange.m_start, newCount);
                        m_pendingChange.m_ids.insert(m_pendingChange.m_ids.begin() + (start - m_pendingChange.m_start),
                                                     id);
                        return;
                    }
                } else {
                    // Adding directly before the previous start?
                    if (start + 1 == m_pendingChange.m_start) {
                        SE_LOG_DEBUG(NULL, NULL, "handle change %s: reduce start and increase count of 'added' individuals, #%d + %d and #%d => #%d + %d",
                                     getPath(),
                                     m_pendingChange.m_start, pendingCount,
                                     start,
                                     start, newCount);
                        m_pendingChange.m_start = start;
                        m_pendingChange.m_ids.push_front(id);
                        return;
                    }
                }
            } else {
                // Removed individuals. The new start was already reduced by
                // previously removed individuals.
                int newCount = m_pendingChange.m_ids.size() + 1;
                if (start == m_pendingChange.m_start) {
                    // Removing directly at end.
                    SE_LOG_DEBUG(NULL, NULL, "handle change %s: increase count of 'removed' individuals, #%d + %d and #%d => #%d + %d",
                                 getPath(),
                                 m_pendingChange.m_start, pendingCount,
                                 start,
                                 m_pendingChange.m_start, newCount);
                    m_pendingChange.m_ids.push_back(id);
                    return;
                } else if (start + 1 == m_pendingChange.m_start) {
                    // Removing directly before the previous start.
                    SE_LOG_DEBUG(NULL, NULL, "handle change %s: reduce start and increase count of 'removed' individuals, #%d + %d and #%d => #%d + %d",
                                 getPath(),
                                 m_pendingChange.m_start, pendingCount,
                                 start,
                                 start, newCount);
                    m_pendingChange.m_start = start;
                    m_pendingChange.m_ids.push_front(id);
                    return;
                }
            }
        }

        // More complex merging is possible. For example, "removed 1
        // at #10" and "added 1 at #10" can be turned into "modified 1
        // at #10", if the ID is the same. This happens when a contact
        // gets modified and folks decides to recreate the
        // FolksIndividual instead of modifying it.
        if (m_pendingChange.m_call == &m_contactsRemoved &&
            &call == &m_contactsAdded &&
            start == m_pendingChange.m_start &&
            1 == m_pendingChange.m_ids.size() &&
            m_pendingChange.m_ids.front() == id) {
            SE_LOG_DEBUG(NULL, NULL, "handle change %s: removed individual was re-added => #%d modified",
                         getPath(),
                         start);
            m_pendingChange.m_call = &m_contactsModified;
            return;
        }

        // Cannot merge changes.
        flushChanges();

        // Now remember requested change.
        m_pendingChange.m_call = &call;
        m_pendingChange.m_start = start;
        m_pendingChange.m_ids.clear();
        m_pendingChange.m_ids.push_back(id);
    }

    /** Clear pending state and flush. */
    void flushChanges()
    {
        int count = m_pendingChange.m_ids.size();
        if (count) {
            SE_LOG_DEBUG(NULL, NULL, "send change %s: %s, #%d + %d",
                         getPath(),
                         m_pendingChange.m_call == &m_contactsModified ? "modified" :
                         m_pendingChange.m_call == &m_contactsAdded ? "added" :
                         m_pendingChange.m_call == &m_contactsRemoved ? "remove" : "???",
                         m_pendingChange.m_start, count);
            m_lastChange = m_pendingChange;
            m_pendingChange.m_ids.clear();
            sendChange(*m_lastChange.m_call,
                       m_lastChange.m_start,
                       m_lastChange.m_ids);
        }
    }

    /** Current state is stable. Flush and tell agent. */
    void quiescent()
    {
        flushChanges();
        m_quiescent.start(getObject(),
                          boost::bind(ViewResource::sendDone,
                                      m_self,
                                      _1,
                                      "Quiescent()",
                                      false));
    }

    /**
     * Used as callback for sending changes to the ViewAgent. Only
     * holds weak references and thus does not prevent deleting view
     * or client.
     */
    static void sendDone(const boost::weak_ptr<ViewResource> &self,
                         const std::string &error,
                         const char *method,
                         bool required)
    {
        if (required && !error.empty()) {
            // remove view because it is no longer needed
            SE_LOG_DEBUG(NULL, NULL, "ViewAgent %s method call failed, deleting view: %s", method, error.c_str());
            boost::shared_ptr<ViewResource> r = self.lock();
            if (r) {
                r->close();
            }
        }
    }

    void init(boost::shared_ptr<ViewResource> self)
    {
        m_self = self;

        // activate D-Bus interface
        add(this, &ViewResource::readContacts, "ReadContacts");
        add(this, &ViewResource::close, "Close");
        add(this, &ViewResource::refineSearch, "RefineSearch");
        activate();

        // The view might have been started already, for example when
        // reconnecting a ViewResource to the persistent full view.
        // Therefore tell the agent about the current content before
        // starting, then connect to signals, and finally start.
        int size = m_view->size();
        if (size) {
            std::vector<std::string> ids;
            ids.reserve(size);
            const IndividualData *data;
            for (int i = 0; i < size; i++) {
                data = m_view->getContact(i);
                ids.push_back(folks_individual_get_id(data->m_individual.get()));
            }
            sendChange(m_contactsAdded, 0, ids);
        }
        m_view->m_quiescenceSignal.connect(IndividualView::QuiescenceSignal_t::slot_type(&ViewResource::quiescent,
                                                                                         this).track(self));
        m_view->m_modifiedSignal.connect(IndividualView::ChangeSignal_t::slot_type(&ViewResource::handleChange,
                                                                                   this,
                                                                                   boost::cref(m_contactsModified),
                                                                                   _1,
                                                                                   _2).track(self));
        m_view->m_addedSignal.connect(IndividualView::ChangeSignal_t::slot_type(&ViewResource::handleChange,
                                                                                this,
                                                                                boost::cref(m_contactsAdded),
                                                                                _1,
                                                                                _2).track(self));
        m_view->m_removedSignal.connect(IndividualView::ChangeSignal_t::slot_type(&ViewResource::handleChange,
                                                                                  this,
                                                                                  boost::cref(m_contactsRemoved),
                                                                                  _1,
                                                                                  _2).track(self));
        m_view->start();

        // start() did all the initial work, no further changes expected
        // if state is considered stable => tell users.
        if (m_view->isQuiescent()) {
            quiescent();
        }
    }

public:
    /** returns the integer number that will be used for the next view resource */
    static unsigned getNextViewNumber() { return m_counter; }

    static boost::shared_ptr<ViewResource> create(const boost::shared_ptr<IndividualView> &view,
                                                  const boost::shared_ptr<LocaleFactory> &locale,
                                                  const boost::shared_ptr<Client> &owner,
                                                  GDBusCXX::connection_type *connection,
                                                  const GDBusCXX::Caller_t &ID,
                                                  const GDBusCXX::DBusObject_t &agentPath)
    {
        boost::shared_ptr<ViewResource> viewResource(new ViewResource(view,
                                                                      locale,
                                                                      owner,
                                                                      connection,
                                                                      ID,
                                                                      agentPath));
        viewResource->init(viewResource);
        return viewResource;
    }

    /** ViewControl.ReadContacts() */
    void readContacts(const std::vector<std::string> &ids, IndividualView::Contacts &contacts)
    {
        if (!ids.empty()) {
            // Ensure that client's view is up-to-date, then prepare the
            // data for it.
            flushChanges();
            m_view->readContacts(ids, contacts);
            // Discard the information about the previous 'modified' signal
            // if the client now has data in that range. Necessary because
            // otherwise future 'modified' signals for that range might get
            // suppressed in handleChange().
            int modifiedCount = m_lastChange.m_ids.size();
            if (m_lastChange.m_call == &m_contactsModified &&
                modifiedCount) {
                BOOST_FOREACH (const IndividualView::Contacts::value_type &entry, contacts) {
                    int index = entry.first;
                    if (index >= m_lastChange.m_start && index < m_lastChange.m_start + modifiedCount) {
                        m_lastChange.m_ids.clear();
                        break;
                    }
                }
            }
        }
    }

    /** ViewControl.Close() */
    void close()
    {
        // Removing the resource from its owner will drop the last
        // reference and delete it when we return.
        boost::shared_ptr<ViewResource> r = m_self.lock();
        if (r) {
            boost::shared_ptr<Client> c = m_owner.lock();
            if (c) {
                c->detach(r.get());
            }
        }
    }

    /** ViewControl.RefineSearch() */
    void refineSearch(const LocaleFactory::Filter_t &filter)
    {
        if (filter.empty()) {
            SE_THROW("New filter is empty. It must be more restrictive than the old filter.");
        }
        boost::shared_ptr<IndividualFilter> individualFilter = m_locale->createFilter(filter);
        m_view->refineFilter(individualFilter);
    }
};
unsigned int ViewResource::m_counter;

void Manager::search(const boost::shared_ptr< GDBusCXX::Result1<GDBusCXX::DBusObject_t> > &result,
                     const GDBusCXX::Caller_t &ID,
                     const boost::shared_ptr<GDBusCXX::Watch> &watch,
                     const LocaleFactory::Filter_t &filter,
                     const GDBusCXX::DBusObject_t &agentPath)
{
    // TODO: figure out a native, thread-safe API for this.

    // Start folks in parallel with asking for an ESourceRegistry.
    start();

    // We don't know for sure whether we'll need the ESourceRegistry.
    // Ask for it, just to be sure. If we need to hurry because we are
    // doing a caller ID lookup during startup, then we'll need it.
    EDSRegistryLoader::getESourceRegistryAsync(boost::bind(&Manager::searchWithRegistry,
                                                           m_self,
                                                           _1,
                                                           _2,
                                                           result,
                                                           ID,
                                                           watch,
                                                           filter,
                                                           agentPath));
}

void Manager::searchWithRegistry(const ESourceRegistryCXX &registry,
                                 const GError *gerror,
                                 const boost::shared_ptr< GDBusCXX::Result1<GDBusCXX::DBusObject_t> > &result,
                                 const GDBusCXX::Caller_t &ID,
                                 const boost::shared_ptr<GDBusCXX::Watch> &watch,
                                 const LocaleFactory::Filter_t &filter,
                                 const GDBusCXX::DBusObject_t &agentPath) throw()
{
    try {
        if (!registry) {
            GErrorCXX::throwError("create ESourceRegistry", gerror);
        }
        doSearch(registry,
                 result,
                 ID,
                 watch,
                 filter,
                 agentPath);
    } catch (...) {
        // Tell caller about specific error.
        dbusErrorCallback(result);
    }
}

void Manager::doSearch(const ESourceRegistryCXX &registry,
                       const boost::shared_ptr< GDBusCXX::Result1<GDBusCXX::DBusObject_t> > &result,
                       const GDBusCXX::Caller_t &ID,
                       const boost::shared_ptr<GDBusCXX::Watch> &watch,
                       const LocaleFactory::Filter_t &filter,
                       const GDBusCXX::DBusObject_t &agentPath)
{
    // Create and track view which is owned by the caller.
    boost::shared_ptr<Client> client = m_server->addClient(ID, watch);

    boost::shared_ptr<IndividualView> view;
    view = m_folks->getMainView();
    bool quiescent = view->isQuiescent();
    std::string ebookFilter;
    if (!filter.empty()) {
        boost::shared_ptr<IndividualFilter> individualFilter = m_locale->createFilter(filter);
        ebookFilter = individualFilter->getEBookFilter();
        if (quiescent) {
            // Don't search via EDS directly because the unified
            // address book is ready.
            ebookFilter.clear();
        }
        view = FilteredView::create(view, individualFilter);
        view->setName(StringPrintf("filtered view%u", ViewResource::getNextViewNumber()));
    }

    SE_LOG_DEBUG(NULL, NULL, "preparing %s: EDS search term is '%s', active address books %s",
                 view->getName(),
                 ebookFilter.c_str(),
                 boost::join(m_enabledEBooks, " ").c_str());
    if (!ebookFilter.empty() && !m_enabledEBooks.empty()) {
        // Set up direct searching in all active address books.
        // These searches are done once, so don't bother to deal
        // with future changes to the active address books or
        // the sort order.
        MergeView::Searches searches;
        searches.reserve(m_enabledEBooks.size());
        boost::shared_ptr<IndividualCompare> compare =
            m_sortOrder.empty() ?
            IndividualCompare::defaultCompare() :
            m_locale->createCompare(m_sortOrder);

        BOOST_FOREACH (const std::string &uuid, m_enabledEBooks) {
            searches.push_back(EDSFView::create(registry,
                                                uuid,
                                                ebookFilter));
            searches.back()->setName(StringPrintf("eds view %s %s", uuid.c_str(), ebookFilter.c_str()));
        }
        boost::shared_ptr<MergeView> merge(MergeView::create(view,
                                                             searches,
                                                             m_locale,
                                                             compare));
        merge->setName(StringPrintf("merge view%u", ViewResource::getNextViewNumber()));
        view = merge;
    }

    boost::shared_ptr<ViewResource> viewResource(ViewResource::create(view,
                                                                      m_locale,
                                                                      client,
                                                                      getConnection(),
                                                                      ID,
                                                                      agentPath));
    client->attach(boost::shared_ptr<Resource>(viewResource));

    // created local resource
    result->done(viewResource->getPath());
}

void Manager::runInSession(const std::string &config,
                           Server::SessionFlags flags,
                           const boost::shared_ptr<GDBusCXX::Result> &result,
                           const boost::function<void (const boost::shared_ptr<Session> &session)> &callback)
{
    try {
        boost::shared_ptr<Session> session = m_server->startInternalSession(config,
                                                                            flags,
                                                                            boost::bind(&Manager::doSession,
                                                                                        this,
                                                                                        _1,
                                                                                        result,
                                                                                        callback));
        if (session->getSyncStatus() == Session::SYNC_QUEUEING) {
            // Must continue to wait instead of dropping the last reference.
            m_pending.push_back(std::make_pair(result, session));
        }
    } catch (...) {
        // Tell caller about specific error.
        dbusErrorCallback(result);
    }
}

void Manager::doSession(const boost::weak_ptr<Session> &weakSession,
                        const boost::shared_ptr<GDBusCXX::Result> &result,
                        const boost::function<void (const boost::shared_ptr<Session> &session)> &callback)
{
    try {
        boost::shared_ptr<Session> session = weakSession.lock();
        if (!session) {
            // Destroyed already?
            return;
        }
        // Drop permanent reference, session will be destroyed when we
        // return.
        m_pending.remove(std::make_pair(result, session));

        // Now run the operation.
        callback(session);
    } catch (...) {
        // Tell caller about specific error.
        dbusErrorCallback(result);
    }
}

void Manager::getActiveAddressBooks(std::vector<std::string> &dbIDs)
{
    BOOST_FOREACH (const std::string &uuid, m_enabledEBooks) {
        if (uuid == "system-address-book") {
            dbIDs.push_back("");
        } else {
            dbIDs.push_back(DB_PEER_PREFIX + uuid.substr(strlen(MANAGER_PREFIX)));
        }
    }
}

void Manager::setActiveAddressBooks(const std::vector<std::string> &dbIDs)
{
    // Build list of EDS UUIDs.
    std::set<std::string> uuids;
    BOOST_FOREACH (const std::string &dbID, dbIDs) {
        if (boost::starts_with(dbID, DB_PEER_PREFIX)) {
            // Database of a specific peer.
            std::string uid = dbID.substr(strlen(DB_PEER_PREFIX));
            uuids.insert(MANAGER_PREFIX + uid);
        } else if (dbID.empty()) {
            // System database. It's UUID is hard-coded here because
            // it is fixed in practice and managing an ESourceRegistry
            // just to get the value seems overkill.
            uuids.insert("system-address-book");
        } else {
            SE_THROW("invalid address book ID: " + dbID);
        }
    }

    // Swap and set in aggregator.
    std::swap(uuids, m_enabledEBooks);
    initDatabases();
    m_configNode->writeProperty(MANAGER_CONFIG_ACTIVE_ADDRESS_BOOKS_PROPERTY,
                                InitStateString(boost::join(m_enabledEBooks, " "), true));
    m_configNode->flush();
}

void Manager::initDatabases()
{
    m_folks->setDatabases(m_enabledEBooks);
}

static void checkPeerUID(const std::string &uid)
{
    const pcrecpp::RE re("[-a-z0-9]*");
    if (!re.FullMatch(uid)) {
        SE_THROW(StringPrintf("invalid peer uid: %s", uid.c_str()));
    }
}

void Manager::setPeer(const boost::shared_ptr<GDBusCXX::Result0> &result,
                      const std::string &uid, const StringMap &properties)
{
    checkPeerUID(uid);
    runInSession(StringPrintf("@%s%s",
                              MANAGER_PREFIX,
                              uid.c_str()),
                 Server::SESSION_FLAG_NO_SYNC,
                 result,
                 boost::bind(&Manager::doSetPeer, this, _1, result, uid, properties));
}

static const char * const PEER_KEY_PROTOCOL = "protocol";
static const char * const PEER_SYNCML_PROTOCOL = "SyncML";
static const char * const PEER_PBAP_PROTOCOL = "PBAP";
static const char * const PEER_FILES_PROTOCOL = "files";
static const char * const PEER_KEY_TRANSPORT = "transport";
static const char * const PEER_BLUETOOTH_TRANSPORT = "Bluetooth";
static const char * const PEER_IP_TRANSPORT = "IP";
static const char * const PEER_DEF_TRANSPORT = PEER_BLUETOOTH_TRANSPORT;
static const char * const PEER_KEY_ADDRESS = "address";
static const char * const PEER_KEY_DATABASE = "database";
static const char * const PEER_KEY_LOGDIR = "logdir";
static const char * const PEER_KEY_MAXSESSIONS = "maxsessions";

static std::string GetEssential(const StringMap &properties, const char *key,
                                bool allowEmpty = false)
{
    InitStateString entry = GetWithDef(properties, key);
    if (!entry.wasSet() ||
        (!allowEmpty && entry.empty())) {
        SE_THROW(StringPrintf("peer config: '%s' must be set%s",
                              key,
                              allowEmpty ? "" : " to a non-empty value"));
    }
    return entry;
}

void Manager::doSetPeer(const boost::shared_ptr<Session> &session,
                        const boost::shared_ptr<GDBusCXX::Result0> &result,
                        const std::string &uid, const StringMap &properties)
{
    // The session is active now, we have exclusive control over the
    // databases and the config. Create or update config.
    std::string protocol = GetEssential(properties, PEER_KEY_PROTOCOL);
    std::string transport = GetWithDef(properties, PEER_KEY_TRANSPORT, PEER_DEF_TRANSPORT);
    std::string address = GetEssential(properties, PEER_KEY_ADDRESS);
    std::string database = GetWithDef(properties, PEER_KEY_DATABASE);
    std::string logdir = GetWithDef(properties, PEER_KEY_LOGDIR);
    std::string maxsessions = GetWithDef(properties, PEER_KEY_MAXSESSIONS);
    unsigned maxLogDirs = 0;
    if (!maxsessions.empty()) {
        // https://svn.boost.org/trac/boost/ticket/5494
        if (boost::starts_with(maxsessions, "-")) {
            SE_THROW(StringPrintf("negative 'maxsessions' not allowed: %s", maxsessions.c_str()));
        }
        maxLogDirs = boost::lexical_cast<unsigned>(maxsessions);
    }

    std::string localDatabaseName = MANAGER_PREFIX + uid;
    std::string context = StringPrintf("@%s%s", MANAGER_PREFIX, uid.c_str());


    SE_LOG_DEBUG(NULL, NULL, "%s: creating config for protocol %s",
                 uid.c_str(),
                 protocol.c_str());

    if (protocol == PEER_PBAP_PROTOCOL) {
        if (!database.empty()) {
            SE_THROW(StringPrintf("peer config: %s=%s: choosing database not supported for %s=%s",
                                  PEER_KEY_ADDRESS, address.c_str(),
                                  PEER_KEY_PROTOCOL, protocol.c_str()));
        }
        if (transport != PEER_BLUETOOTH_TRANSPORT) {
            SE_THROW(StringPrintf("peer config: %s=%s: only transport %s is supported for %s=%s",
                                  PEER_KEY_TRANSPORT, transport.c_str(),
                                  PEER_BLUETOOTH_TRANSPORT,
                                  PEER_KEY_PROTOCOL, protocol.c_str()));
        }
    }

    if (protocol == PEER_PBAP_PROTOCOL ||
        protocol == PEER_FILES_PROTOCOL) {
        // Create or set local config.
        boost::shared_ptr<SyncConfig> config(new SyncConfig(MANAGER_LOCAL_CONFIG + context));
        config->setDefaults();
        config->prepareConfigForWrite();
        config->setPreventSlowSync(false);
        config->setSyncURL("local://" + context);
        config->setPeerIsClient(true);
        config->setDumpData(false);
        config->setPrintChanges(false);
        if (!logdir.empty()) {
            config->setLogDir(logdir);
        }
        if (!maxsessions.empty()) {
            config->setMaxLogDirs(maxLogDirs);
        }
        boost::shared_ptr<PersistentSyncSourceConfig> source(config->getSyncSourceConfig(MANAGER_LOCAL_SOURCE));
        source->setBackend("evolution-contacts");
        source->setDatabaseID(localDatabaseName);
        source->setSync("local-cache");
        source->setURI(MANAGER_REMOTE_SOURCE);
        config->flush();
        // Ensure that database exists.
        SyncSourceParams params(MANAGER_LOCAL_SOURCE,
                                config->getSyncSourceNodes(MANAGER_LOCAL_SOURCE),
                                config,
                                context);
        boost::scoped_ptr<SyncSource> syncSource(SyncSource::createSource(params));
        SyncSource::Databases databases = syncSource->getDatabases();
        bool found = false;
        BOOST_FOREACH (const SyncSource::Database &database, databases) {
            if (database.m_uri == localDatabaseName) {
                found = true;
                break;
            }
        }
        if (!found) {
            syncSource->createDatabase(SyncSource::Database(localDatabaseName, localDatabaseName));
        }

        // Now also create target config, in the same context.
        config.reset(new SyncConfig(MANAGER_REMOTE_CONFIG + context));
        config->setDefaults();
        config->prepareConfigForWrite();
        config->setPreventSlowSync(false);
        config->setDumpData(false);
        config->setPrintChanges(false);
        if (!logdir.empty()) {
            config->setLogDir(logdir);
        }
        if (!maxsessions.empty()) {
            config->setMaxLogDirs(maxLogDirs);
        }
        source = config->getSyncSourceConfig(MANAGER_REMOTE_SOURCE);
        if (protocol == PEER_PBAP_PROTOCOL) {
            // PBAP
            source->setDatabaseID("obex-bt://" + address);
            source->setBackend("pbap");
        } else {
            // Local sync with files on the target side.
            // Format is hard-coded to vCard 3.0.
            source->setDatabaseID("file://" + address);
            source->setDatabaseFormat("text/vcard");
            source->setBackend("file");
        }
        config->flush();
    } else {
        SE_THROW(StringPrintf("peer config: %s=%s not supported",
                              PEER_KEY_PROTOCOL,
                              protocol.c_str()));
    }

    // Report success.
    SE_LOG_DEBUG(NULL, NULL, "%s: created config for protocol %s",
                 uid.c_str(),
                 protocol.c_str());
    result->done();
}

Manager::PeersMap Manager::getAllPeers()
{
    PeersMap peers;

    SyncConfig::ConfigList configs = SyncConfig::getConfigs();
    std::string prefix = StringPrintf("%s@%s",
                                      MANAGER_LOCAL_CONFIG,
                                      MANAGER_PREFIX);

    BOOST_FOREACH (const StringPair &entry, configs) {
        if (boost::starts_with(entry.first, prefix)) {
            // One of our configs.
            std::string uid = entry.first.substr(prefix.size());
            StringMap &properties = peers[uid];
            // Extract relevant properties from configs.
            SyncConfig localConfig(entry.first);
            InitState< std::vector<std::string> > syncURLs = localConfig.getSyncURL();
            std::string syncURL;
            if (!syncURLs.empty()) {
                syncURL = syncURLs[0];
            }
            if (boost::starts_with(syncURL, "local://")) {
                // Look at target source to determine protocol.
                SyncConfig targetConfig(StringPrintf("%s@%s%s",
                                                     MANAGER_REMOTE_CONFIG,
                                                     MANAGER_PREFIX,
                                                     uid.c_str()));
                boost::shared_ptr<PersistentSyncSourceConfig> source(targetConfig.getSyncSourceConfig(MANAGER_REMOTE_SOURCE));
                std::string backend = source->getBackend();
                std::string database = source->getDatabaseID();
                if (backend == "PBAP Address Book") {
                    properties[PEER_KEY_PROTOCOL] = PEER_PBAP_PROTOCOL;
                    if (boost::starts_with(database, "obex-bt://")) {
                        properties[PEER_KEY_ADDRESS] = database.substr(strlen("obex-bt://"));
                    }
                } else if (backend == "file") {
                    properties[PEER_KEY_PROTOCOL] = PEER_FILES_PROTOCOL;
                    if (boost::starts_with(database, "file://")) {
                        properties[PEER_KEY_ADDRESS] = database.substr(strlen("file://"));
                    }
                }
            }
        }
    }

    return peers;
}


void Manager::removePeer(const boost::shared_ptr<GDBusCXX::Result0> &result,
                         const std::string &uid)
{
    checkPeerUID(uid);
    runInSession(StringPrintf("@%s%s",
                              MANAGER_PREFIX,
                              uid.c_str()),
                 Server::SESSION_FLAG_NO_SYNC,
                 result,
                 boost::bind(&Manager::doRemovePeer, this, _1, result, uid));
}

void Manager::doRemovePeer(const boost::shared_ptr<Session> &session,
                           const boost::shared_ptr<GDBusCXX::Result0> &result,
                           const std::string &uid)
{
    std::string localDatabaseName = MANAGER_PREFIX + uid;
    std::string context = StringPrintf("@%s%s", MANAGER_PREFIX, uid.c_str());

    // Remove database. This is expected to be noticed by libfolks
    // once we delete the database without us having to tell it, but
    // doing so doesn't hurt.
    m_enabledEBooks.erase(localDatabaseName);
    initDatabases();
    m_configNode->writeProperty(MANAGER_CONFIG_SORT_PROPERTY,
                                InitStateString(boost::join(m_enabledEBooks, " "), true));
    m_configNode->flush();

    // Access config via context (includes sync and target config).
    boost::shared_ptr<SyncConfig> config(new SyncConfig(context));

    // Remove database, if it exists.
    if (config->exists(CONFIG_LEVEL_CONTEXT)) {
        boost::shared_ptr<PersistentSyncSourceConfig> source(config->getSyncSourceConfig(MANAGER_LOCAL_SOURCE));
        SyncSourceNodes nodes = config->getSyncSourceNodes(MANAGER_LOCAL_SOURCE);
        if (nodes.dataConfigExists()) {
            SyncSourceParams params(MANAGER_LOCAL_SOURCE,
                                    nodes,
                                    config,
                                    context);
            boost::scoped_ptr<SyncSource> syncSource(SyncSource::createSource(params));
            SyncSource::Databases databases = syncSource->getDatabases();
            bool found = false;
            BOOST_FOREACH (const SyncSource::Database &database, databases) {
                if (database.m_uri == localDatabaseName) {
                    found = true;
                    break;
                }
            }
            if (found) {
                syncSource->deleteDatabase(localDatabaseName);
            }
        }
    }

    // Remove entire context, just in case. Placing the code here also
    // ensures that nothing except the config itself has the config
    // nodes open, which would prevent removing them. For the same
    // reason the SyncConfig is recreated: to clear all references to
    // sources that were opened via it.
    config.reset(new SyncConfig(context));
    config->remove();
    config->flush();

    // Report success.
    result->done();
}

void Manager::syncPeer(const boost::shared_ptr<GDBusCXX::Result0> &result,
                       const std::string &uid)
{
    checkPeerUID(uid);
    runInSession(StringPrintf("%s@%s%s",
                              MANAGER_LOCAL_CONFIG,
                              MANAGER_PREFIX,
                              uid.c_str()),
                 Server::SESSION_FLAG_NO_SYNC,
                 result,
                 boost::bind(&Manager::doSyncPeer, this, _1, result, uid));
}

static void doneSyncPeer(const boost::shared_ptr<GDBusCXX::Result0> &result,
                         SyncMLStatus status)
{
    if (status == STATUS_OK ||
        status == STATUS_HTTP_OK) {
        result->done();
    } else if (status == (SyncMLStatus)sysync::LOCERR_USERABORT) {
        result->failed(GDBusCXX::dbus_error(MANAGER_ERROR_ABORTED, "running sync aborted, probably by StopSync()"));
    } else {
        result->failed(GDBusCXX::dbus_error(MANAGER_ERROR_BAD_STATUS, Status2String(status)));
    }
}

void Manager::doSyncPeer(const boost::shared_ptr<Session> &session,
                         const boost::shared_ptr<GDBusCXX::Result0> &result,
                         const std::string &uid)
{
    // After sync(), the session is tracked as the active sync session
    // by the server. It was removed from our own m_pending list by
    // doSession().
    session->sync("ephemeral", SessionCommon::SourceModes_t());
    // Relay result to caller when done.
    session->m_doneSignal.connect(boost::bind(doneSyncPeer, result, _1));
}

void Manager::stopSync(const boost::shared_ptr<GDBusCXX::Result0> &result,
                       const std::string &uid)
{
    checkPeerUID(uid);

    // Fully qualified peer config name. Only used for sync sessions
    // and thus good enough to identify them.
    std::string syncConfigName = StringPrintf("%s@%s%s",
                                              MANAGER_LOCAL_CONFIG,
                                              MANAGER_PREFIX,
                                              uid.c_str());

    // Remove all pending sessions of the peer. Make a complete
    // copy of the list, to avoid issues with modifications of the
    // underlying list while we iterate over it.
    BOOST_FOREACH (const Pending_t::value_type &entry, Pending_t(m_pending)) {
        std::string configName = entry.second->getConfigName();
        if (configName == syncConfigName) {
            entry.first->failed(GDBusCXX::dbus_error(MANAGER_ERROR_ABORTED, "pending sync aborted by StopSync()"));
            m_pending.remove(entry);
        }
    }

    // Stop the currently running sync if it is for the peer.
    // It may or may not complete, depending on what it is currently
    // doing. We'll check in doneSyncPeer().
    boost::shared_ptr<Session> session = m_server->getSyncSession();
    bool aborting = false;
    if (session) {
        std::string configName = session->getConfigName();
        if (configName == syncConfigName) {
            // Return to caller later, when aborting is done.
            session->abortAsync(SimpleResult(boost::bind(&GDBusCXX::Result0::done, result),
                                             createDBusErrorCb(result)));
            aborting = true;
        }
    }
    if (!aborting) {
        result->done();
    }
}

void Manager::addContact(const boost::shared_ptr< GDBusCXX::Result1<std::string> > &result,
                         const std::string &addressbook,
                         const PersonaDetails &details)
{
    try {
        if (!addressbook.empty()) {
            SE_THROW("only the system address book is writable");
        }
        m_folks->addContact(createDBusCb(result), details);
    } catch (...) {
        dbusErrorCallback(result);
    }
}

void Manager::modifyContact(const boost::shared_ptr<GDBusCXX::Result0> &result,
                            const std::string &addressbook,
                            const std::string &localID,
                            const PersonaDetails &details)
{
    try {
        if (!addressbook.empty()) {
            SE_THROW("only the system address book is writable");
        }
        m_folks->modifyContact(createDBusCb(result),
                               localID,
                               details);
    } catch (...) {
        dbusErrorCallback(result);
    }
}

void Manager::removeContact(const boost::shared_ptr<GDBusCXX::Result0> &result,
                            const std::string &addressbook,
                            const std::string &localID)
{
    try {
        if (!addressbook.empty()) {
            SE_THROW("only the system address book is writable");
        }
        m_folks->removeContact(createDBusCb(result),
                               localID);
    } catch (...) {
        dbusErrorCallback(result);
    }
}

SE_END_CXX
