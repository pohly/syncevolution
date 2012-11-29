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

#ifndef INCL_SYNCEVO_DBUS_SERVER_PIM_FILTERED_VIEW
#define INCL_SYNCEVO_DBUS_SERVER_PIM_FILTERED_VIEW

#include "view.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

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

    bool isFull();
    void fillView(int candidate);

 public:
    /**
     * Creates an idle IndividualAggregator. Configure it and
     * subscribe to signals, then call start().
     */
    static boost::shared_ptr<FilteredView> create(const boost::shared_ptr<IndividualView> &parent,
                                                  const boost::shared_ptr<IndividualFilter> &filter);

    /**
     * Mirrors the quiesent state of the underlying view.
     */
    virtual bool isQuiescent() const { return m_parent->isQuiescent(); }

    /**
     * Add a FolksIndividual if it matches the filter. Tracking of
     * changes to individuals is done in parent view.
     */
    void addIndividual(int parentIndex, const IndividualData &data);

    /**
     * Removes a FolksIndividual. Might not have been added at all.
     */
    void removeIndividual(int parentIndex, const IndividualData &data);

    /**
     * Check whether a changed individual still belongs into the view.
     */
    void modifyIndividual(int parentIndex, const IndividualData &data);

    // from IndividualView
    virtual void doStart();
    virtual void refineFilter(const boost::shared_ptr<IndividualFilter> &individualFilter);
    virtual int size() const { return (int)m_local2parent.size(); }
    virtual const IndividualData *getContact(int index) { return (index >= 0 && (unsigned)index < m_local2parent.size()) ? m_parent->getContact(m_local2parent[index]) : NULL; }
};

SE_END_CXX

#endif // INCL_SYNCEVO_DBUS_SERVER_PIM_FILTERED_VIEW
