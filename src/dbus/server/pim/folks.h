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
 * The classes in this file implement sorting, searching and
 * configuring of the unified address book based on libfolks.
 *
 * This is pure C++ code. The D-Bus IPC binding for it is
 * implemented separately in pim-manager.h/cpp.
 */

#ifndef INCL_SYNCEVO_DBUS_SERVER_IVI_FOLKS
#define INCL_SYNCEVO_DBUS_SERVER_IVI_FOLKS

#include <folks/folks.h>

#include "../timeout.h"

#include <syncevo/GLibSupport.h>
#include <syncevo/GeeSupport.h>
#include <syncevo/GValueSupport.h>

#include <boost/shared_ptr.hpp>
#include <boost/signals2.hpp>

SE_GOBJECT_TYPE(FolksIndividualAggregator)
SE_GOBJECT_TYPE(FolksIndividual)
SE_GOBJECT_TYPE(FolksEmailFieldDetails)
SE_GOBJECT_TYPE(FolksBackendStore)
SE_GOBJECT_TYPE(FolksBackend)
SE_GOBJECT_TYPE(GeeCollection)
SE_GOBJECT_TYPE(GeeHashSet)

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * Abstract interface for comparing two FolksIndividual instances.
 * The properties of a folks individual may change at any time.
 * Therefore the key properties which determine the sort order
 * must be copied from the individual. They will be updated when
 * the individual changes.
 *
 * The other advantage is that complex, derived keys only need
 * to be computed once instead of each time during a comparison.
 * For example, boost::locale::collator::transform() can be used
 * to generate the keys.
 */
class IndividualCompare
{
 public:
    virtual ~IndividualCompare() {}

    typedef std::vector<std::string> Criteria_t;

    /**
     * Calculate an ordered set of criteria for comparing
     * individuals. The default comparison will start with the initial
     * criteria and move on until a difference is found.
     *
     * This is necessary because a single string of "Doe, John" is
     * treated differently in a collation than the pair of strings
     * "Doe" and "John".
     *
     * @param individual    the individual for which sort keys are to be created
     * @retval criteria     cleared before the call, filled afterwards
     */
    virtual void createCriteria(FolksIndividual *individual, Criteria_t &criteria) const = 0;

    /**
     * Partial sort order: true if a smaller than b.
     *
     * Default implementation uses normal std::string::compare().
     */
    virtual bool compare(const Criteria_t &a, const Criteria_t &b) const;
};

/**
 * A FolksIndividual plus its sort criteria.
 */
struct IndividualData
{
    /**
     * Sets all members to match the given individual, using the
     * compare instance to compute values.
     */
    void init(const boost::shared_ptr<IndividualCompare> &compare,
              FolksIndividual *individual);

    FolksIndividualCXX m_individual;
    IndividualCompare::Criteria_t m_criteria;
};

/**
 * wraps an IndividualCompare for std algorithms on IndividualData
 */
class IndividualDataCompare : public std::binary_function<IndividualData, IndividualData, bool>
{
    boost::shared_ptr<IndividualCompare> m_compare;

 public:
    IndividualDataCompare(const boost::shared_ptr<IndividualCompare> &compare) :
       m_compare(compare)
    {}
    IndividualDataCompare(const IndividualDataCompare &other) :
       m_compare(other.m_compare)
    {}

    bool operator () (const IndividualData &a, const IndividualData &b) const
    {
        return m_compare->compare(a.m_criteria, b.m_criteria);
    }
};

/**
 * Abstract interface for filtering (aka searching) FolksIndividual
 * instances.
 */
class IndividualFilter
{
 public:
    virtual ~IndividualFilter() {}

    /** true if the contact matches the filter */
    virtual bool matches(FolksIndividual *individual) const = 0;
};

class IndividualAggregator;

/**
 * A view on a sorted list of individuals. Entries are numbered from
 * #0 to #n - 1, where n is the number of entries. Change
 * notifications are based upon those numbers and will be triggered
 * immediately.
 */
class IndividualView
{
    Bool m_started;

 public:
    typedef boost::signals2::signal<void (int, FolksIndividual *)> ChangeSignal_t;
    typedef boost::signals2::signal<void (void)> QuiesenceSignal_t;

    /**
     * Triggered each time the view reaches a quiesence state, meaning
     * that its current content is stable, at least for now.
     */
    QuiesenceSignal_t m_quiesenceSignal;

    /**
     * A new FolksIndividual was added at a specific index. This
     * increased the index of all individuals it was inserted in front
     * off by one.
     */
    ChangeSignal_t m_addedSignal;

    /**
     * A FolksIndividual was removed at a specific index. This
     * increased the index of all individuals after it by one.
     */
    ChangeSignal_t m_removedSignal;

    /**
     * A FolksIndividual was modified at a specific index, without
     * affecting its position in the view. If changing a FolksIndividual
     * affects its position, m_removedSignal followed by m_addedSignal
     * will be emitted.
     */
    ChangeSignal_t m_modifiedSignal;

    /**
     * Start filling the view. Gives the user a chance to connect
     * to the signals first. May be called multiple times.
     */
    void start();

    /**
     * start() was called.
     */
    bool isRunning() const;

    /** current number of entries */
    virtual int size() const = 0;

    /** read a range of contacts - see org.01.pim.contacts.ViewControl.ReadContacts() */
    virtual void readContacts(int start, int count, std::vector<FolksIndividualCXX> &contacts);

    /** returns access to one individual or an empty pointer if outside of the current range */
    virtual FolksIndividualCXX getContact(int index) = 0;

 protected:
    /**
     * Start filling the view. Will only be called once by start().
     */
    virtual void doStart() = 0;
};

/**
 * The view which takes input directly from IndividualAggregator
 * and maintains a sorted set of contacts as result.
 */
class FullView : public IndividualView
{
    FolksIndividualAggregatorCXX m_folks;
    boost::weak_ptr<FullView> m_self;
    Timeout m_waitForIdle;
    std::set<FolksIndividualCXX> m_pendingModifications;

    /**
     * Sorted vector. Sort order is maintained by this class.
     */
    typedef std::vector<IndividualData> Entries_t;
    Entries_t m_entries;

    /**
     * The sort object to be used.
     */
    boost::shared_ptr<IndividualCompare> m_compare;

    FullView(const FolksIndividualAggregatorCXX &folks);
    void init(const boost::shared_ptr<FullView> &self);

    /**
     * Run via m_waitForIdle if (and only if) something
     * changed.
     */
    void onIdle();

    /**
     * Ensure that onIdle() gets invoked.
     */
    void waitForIdle();

    void doAddIndividual(const IndividualData &data);

 public:
    /**
     * @param folks     the aggregator to use, may be empty for testing
     */
    static boost::shared_ptr<FullView> create(const FolksIndividualAggregatorCXX &folks =
                                              FolksIndividualAggregatorCXX());

    /** FolksIndividualAggregator "individuals-changed" slot */
    void individualsChanged(GeeSet *added,
                            GeeSet *removed,
                            gchar *message,
                            FolksPersona *actor,
                            FolksGroupDetailsChangeReason reason = FOLKS_GROUP_DETAILS_CHANGE_REASON_NONE);

    /** GObject "notify" slot */
    void individualModified(gpointer gobject,
                            GParamSpec *pspec);

    /**
     * FolksIndividualAggregator "is-quiesent" property change slot.
     *
     * It turned out that "quiesence" is only set to true once in
     * FolksIndividualAggregator. The code which watches that signal
     * is still in place, but it will only get invoked once.
     *
     * Therefore the main mechanism for emitting m_quiesenceSignal in
     * FullView is an idle callback which gets invoked each time the
     * daemon has nothing to do, which implies that (at least for now)
     * libfolks has no pending work to do.
     */
    void quiesenceChanged();

    /**
     * Add a FolksIndividual. Starts monitoring it for changes.
     */
    void addIndividual(FolksIndividual *individual);

    /**
     * Deal with FolksIndividual modification.
     */
    void modifyIndividual(FolksIndividual *individual);

   /**
     * Remove a FolksIndividual.
     */
    void removeIndividual(FolksIndividual *individual);

    /**
     * Set new sort method. Reorders current set of entries on the
     * fly. Default is lexicographical comparison of the single-string
     * full name.
     *
     * @param compare   the new ordering or NULL for the builtin default (last/first with ASCII lexicographic comparison)
     */
    void setCompare(const boost::shared_ptr<IndividualCompare> &compare);

    // from IndividualView
    virtual void doStart();
    virtual int size() const { return (int)m_entries.size(); }
    virtual FolksIndividualCXX getContact(int index) { return (index >= 0 && (unsigned)index < m_entries.size()) ? m_entries[index].m_individual : FolksIndividualCXX(); }
};

/**
 * A subset of some other view. Takes input from that view and thus
 * can rely on individuals being sorted by their index number in the
 * other view.
 */
class FilteredView : public IndividualView
{
    boost::weak_ptr<FilteredView> m_self;
    boost::shared_ptr<IndividualView> m_parent;
    boost::shared_ptr<IndividualFilter> m_filter;

    /**
     * Maps local indices to indices in parent view. Could be be
     * optimized to map entire ranges, but for the sake of simplicitly
     * let's use a 1:1 mapping for now.
     */
    typedef std::vector<int> Entries_t;
    Entries_t m_local2parent;

    FilteredView(const boost::shared_ptr<IndividualView> &parent,
                 const boost::shared_ptr<IndividualFilter> &filter);
    void init(const boost::shared_ptr<FilteredView> &self);

 public:
    /**
     * Creates an idle IndividualAggregator. Configure it and
     * subscribe to signals, then call start().
     */
    static boost::shared_ptr<FilteredView> create(const boost::shared_ptr<IndividualView> &parent,
                                                  const boost::shared_ptr<IndividualFilter> &filter);

    /**
     * Add a FolksIndividual if it matches the filter. Tracking of
     * changes to individuals is done in parent view.
     */
    void addIndividual(int parentIndex, FolksIndividual *individual);

    /**
     * Removes a FolksIndividual. Might not have been added at all.
     */
    void removeIndividual(int parentIndex, FolksIndividual *individual);

    /**
     * Check whether a changed individual still belongs into the view.
     */
    void modifyIndividual(int parentIndex, FolksIndividual *individual);

    // from IndividualView
    virtual void doStart();
    virtual int size() const { return (int)m_local2parent.size(); }
    virtual FolksIndividualCXX getContact(int index) { return (index >= 0 && (unsigned)index < m_local2parent.size()) ? m_parent->getContact(m_local2parent[index]) : FolksIndividualCXX(); }
};

/**
 * Is a normal FolksIndividualAggregator and adds sorting, searching
 * and browsing to it. At least the full sorted view always exists.
 */
class IndividualAggregator
{
    /** empty when not started yet */
    boost::shared_ptr<FullView> m_view;
    boost::shared_ptr<IndividualCompare> m_compare;
    boost::weak_ptr<IndividualAggregator> m_self;
    FolksIndividualAggregatorCXX m_folks;
    FolksBackendStoreCXX m_backendStore;
    /**
     * NULL when backends haven't been loaded yet.
     * Set by backendsLoaded().
     */
    FolksBackendCXX m_eds;

    /**
     * The set of enabled EDS databases, referenced by the UUID.
     * Empty by default.
     */
    GeeHashSetCXX m_databases;
    /** string representation for debugging */
    std::string dumpDatabases();

    IndividualAggregator();
    void init(boost::shared_ptr<IndividualAggregator> &self);

    /**
     * Called when all Folks backends are loaded, before the
     * aggregator does its work. Now is the right time to initialize
     * the set of databases and prepare the aggregator, if start() was
     * already called.
     */
    void backendsLoaded();

 public:
    /**
     * Creates an idle IndividualAggregator. Configure it and
     * subscribe to signals, then call start().
     */
    static boost::shared_ptr<IndividualAggregator> create();

    /**
     * Access to FolksIndividualAggregator which is owned by
     * the aggregator.
     */
    FolksIndividualAggregator *getFolks() const { return m_folks.get(); }

    /**
     * Set sorting without starting the view just yet.
     */
    void setCompare(const boost::shared_ptr<IndividualCompare> &compare);

    /**
     * Starts pulling and sorting of contacts.
     * Creates m_view and starts populating it.
     * Can be called multiple times.
     *
     * See also org.01.pim.contacts.Manager.Start().
     */
    void start();

    /**
     * start() was called, or something caused it to be called.
     */
    bool isRunning() const;

    /**
     * configure active databases
     *
     * @param set of EDS database UUIDs, empty string for the default
     * system address book
     */
    void setDatabases(std::set<std::string> &databases);

    /**
     * Each aggregator has exactly one full view on the data. This
     * method grants access to it and its change signals.
     *
     * @return never empty, start() will be called if necessary
     */
    boost::shared_ptr<FullView> getMainView();
};


SE_END_CXX

#endif // INCL_SYNCEVO_DBUS_SERVER_IVI_FOLKS
