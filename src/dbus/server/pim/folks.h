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

#include <syncevo/GLibSupport.h>
#include <syncevo/GeeSupport.h>
#include <syncevo/GValueSupport.h>

#include <boost/shared_ptr.hpp>
#include <boost/signals2.hpp>

SE_GOBJECT_TYPE(FolksIndividualAggregator)
SE_GOBJECT_TYPE(FolksIndividual)
SE_GOBJECT_TYPE(FolksEmailFieldDetails)

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
    FolksIndividualCXX m_individual;
    IndividualCompare::Criteria_t m_criteria;
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
 public:
    typedef boost::signals2::signal<void (int, FolksIndividual *)> ChangeSignal_t;

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

    /** current number of entries */
    virtual int size() const = 0;
};

/**
 * The view which takes input directly from IndividualAggregator
 * and maintains a sorted set of contacts as result.
 */
class FullView : public IndividualView
{
    boost::weak_ptr<FullView> m_self;

    /**
     * Sorted vector. Sort order is maintained by this class.
     */
    std::vector<IndividualData> m_entries;

    /**
     * The sort object to be used.
     */
    boost::shared_ptr<IndividualCompare> m_compare;

    FullView() {}

 public:
    static boost::shared_ptr<FullView> create();

    /**
     * Add a FolksIndividual. Starts monitoring it for changes.
     */
    void addIndividual(FolksIndividual *individual);

    /**
     * Remove a FolksIndividual.
     */
    void removeIndividual(FolksIndividual *individual);

    /**
     * Set new sort method. Reorders current set of entries on the
     * fly. Default is lexicographical comparison of the single-string
     * full name.
     */
    void setCompare(const boost::shared_ptr<IndividualCompare> &compare);

    // from IndividualView
    virtual int size() const { return (int)m_entries.size(); }
};

/**
 * A subset of some other view. Takes input from that view and thus
 * can rely on individuals being sorted by their index number in the
 * other view.
 */
class FilteredView : public IndividualView
{
    boost::weak_ptr<FilteredView> m_self;
    boost::shared_ptr<IndividualFilter> m_filter;
    boost::shared_ptr<IndividualView> m_parent;

    /**
     * Maps local indices to indices in parent view. Could be be
     * optimized to map entire ranges, but for the sake of simplicitly
     * let's use a 1:1 mapping for now.
     */
    std::vector<int> m_local2parent;

 public:
    /**
     * Creates an idle IndividualAggregator. Configure it and
     * subscribe to signals, then call start().
     */
    static boost::shared_ptr<FilteredView> create(const boost::shared_ptr<IndividualView> &parent,
                                                  const boost::shared_ptr<IndividualFilter> &filter);

    /**
     * Populates view from current content of parent, then
     * updates it based on incoming signals.
     */
    void start();

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
    void changeIndividual(int parentIndex, FolksIndividual *individual);

    // from IndividualView
    virtual int size() const { return (int)m_local2parent.size(); }
};

/**
 * Is a normal FolksIndividualAggregator and adds sorting, searching
 * and browsing to it. At least the full sorted view always exists.
 */
class IndividualAggregator
{
    boost::shared_ptr<FullView> m_view;
    boost::weak_ptr<IndividualAggregator> m_self;
    FolksIndividualAggregatorCXX m_folks;

    IndividualAggregator() {}

    void individualsChanged(GeeSet *added,
                            GeeSet *removed,
                            gchar *message) throw ();

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
     * Starts pulling and sorting of contacts.
     * Populates m_view and all other, derived views.
     */
    void start();

    /** TODO: configure active databases */

    /**
     * Each aggregator has exactly one full view on the data. This
     * method grants access to the change signals.
     */
    boost::shared_ptr<IndividualView> getMainView() const { return m_view; }
};


SE_END_CXX

#endif // INCL_SYNCEVO_DBUS_SERVER_IVI_FOLKS
