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

#include "filtered-view.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

FilteredView::FilteredView(const boost::shared_ptr<IndividualView> &parent,
                           const boost::shared_ptr<IndividualFilter> &filter) :
    m_parent(parent),
    m_filter(filter)
{
    setName("filtered view");
}

void FilteredView::init(const boost::shared_ptr<FilteredView> &self)
{
    m_self = self;
    m_parent->m_quiescenceSignal.connect(QuiescenceSignal_t::slot_type(boost::bind(boost::cref(m_quiescenceSignal))).track(m_self));
}

boost::shared_ptr<FilteredView> FilteredView::create(const boost::shared_ptr<IndividualView> &parent,
                                                     const boost::shared_ptr<IndividualFilter> &filter)
{
    boost::shared_ptr<FilteredView> view(new FilteredView(parent, filter));
    view->init(view);
    return view;
}

void FilteredView::doStart()
{
    m_parent->start();

    // Add initial content. Our processing of the new contact must not
    // cause changes to the parent view, otherwise the result will not
    // be inconsistent.
    for (int index = 0; !isFull() && index < m_parent->size(); index++) {
        addIndividual(index, *m_parent->getContact(index));
    }

    // Start listening to signals.
    m_parent->m_addedSignal.connect(ChangeSignal_t::slot_type(boost::bind(&FilteredView::addIndividual, this, _1, _2)).track(m_self));
    m_parent->m_modifiedSignal.connect(ChangeSignal_t::slot_type(boost::bind(&FilteredView::modifyIndividual, this, _1, _2)).track(m_self));
    m_parent->m_removedSignal.connect(ChangeSignal_t::slot_type(boost::bind(&FilteredView::removeIndividual, this, _1, _2)).track(m_self));
}

bool FilteredView::isFull()
{
    size_t newEndIndex = m_local2parent.end() - m_local2parent.begin();
    return !m_filter->isIncluded(newEndIndex);
}

void FilteredView::fillView(int candidate)
{
    // Can we add back contacts which were excluded because of the
    // maximum number of results?
    while (!isFull() &&
           candidate < m_parent->size()) {
        const IndividualData *data = m_parent->getContact(candidate);
        addIndividual(candidate, *data);
        candidate++;
    }
}

void FilteredView::refineFilter(const boost::shared_ptr<IndividualFilter> &individualFilter)
{
    // Keep number of results the same, to avoid additional corner
    // cases.
    if (individualFilter->getMaxResults() != -1 &&
        individualFilter->getMaxResults() != m_filter->getMaxResults()) {
        SE_THROW("refining the search must not change the maximum number of results");
    }
    individualFilter->setMaxResults(m_filter->getMaxResults());

    int candidate = m_local2parent.empty() ? 0 : m_local2parent.back() + 1;
    bool removed = false;
    size_t index = 0;
    while (index < m_local2parent.size()) {
        const IndividualData *data = m_parent->getContact(m_local2parent[index]);
        if (individualFilter->matches(*data)) {
            // Still matched, just skip it.
            ++index;
        } else {
            // No longer matched, remove it.
            m_local2parent.erase(m_local2parent.begin() + index);
            m_removedSignal(index, *data);
            removed = true;
        }
    }
    m_filter = individualFilter;

    if (removed) {
        fillView(candidate);
    }

    // If the parent is currently busy, then we can delay sending the
    // signal until it is no longer busy.
    if (isQuiescent()) {
        m_quiescenceSignal();
    }
}

void FilteredView::addIndividual(int parentIndex, const IndividualData &data)
{
    // We can use binary search to find the insertion point.
    // Check last entry first, because that is going to be
    // very common when adding via doStart().
    Entries_t::iterator it;
    if (!m_local2parent.empty() &&
        m_local2parent.back() < parentIndex) {
        it = m_local2parent.end();
    } else {
        it =
            std::lower_bound(m_local2parent.begin(),
                             m_local2parent.end(),
                             parentIndex);
    }

    // Adding a contact in the parent changes values in our
    // mapping array, regardless whether the new contact also
    // gets and entry in it. Shift all following indices.
    for (Entries_t::iterator it2 = it;
         it2 != m_local2parent.end();
         ++it2) {
        (*it2)++;
    }

    if (m_filter->matches(data)) {
        size_t index = it - m_local2parent.begin();
        if (m_filter->isIncluded(index)) {
            // Remove first if necessary, to ensure that recipient
            // never has more entries in its view than requested.
            size_t newEndIndex = m_local2parent.end() - m_local2parent.begin();
            if (newEndIndex > index &&
                !m_filter->isIncluded(newEndIndex)) {
                const IndividualData *data = m_parent->getContact(m_local2parent.back());
                SE_LOG_DEBUG(NULL, NULL, "%s: removed at #%ld/%ld to make room for new entry", getName(), (long)(newEndIndex - 1), (long)m_local2parent.size());
                m_local2parent.pop_back();
                m_removedSignal(newEndIndex - 1, *data);
                // Iterator might have pointed to removed entry, which may have
                // invalidated it. Get fresh iterator based on index.
                it = m_local2parent.begin() + index;
            }

            m_local2parent.insert(it, parentIndex);
            SE_LOG_DEBUG(NULL, NULL, "%s: added at #%ld/%ld", getName(), (long)index, (long)m_local2parent.size());
            m_addedSignal(index, data);
        } else {
            SE_LOG_DEBUG(NULL, NULL, "%s: not added at #%ld/%ld because outside of result range", getName(), (long)index, (long)m_local2parent.size());
        }
    }
}

void FilteredView::removeIndividual(int parentIndex, const IndividualData &data)
{
    // The entries are sorted. Therefore we can use a binary search
    // to find the parentIndex or the first entry after it.
    Entries_t::iterator it =
        std::lower_bound(m_local2parent.begin(),
                         m_local2parent.end(),
                         parentIndex);
    // Removing a contact in the parent changes values in our mapping
    // array, regardless whether the removed contact is part of our
    // view. Shift all following indices, including the removed entry
    // if it is part of the view.
    bool found = it != m_local2parent.end() && *it == parentIndex;
    for (Entries_t::iterator it2 = it;
         it2 != m_local2parent.end();
         ++it2) {
        (*it2)--;
    }

    if (found) {
        size_t index = it - m_local2parent.begin();
        SE_LOG_DEBUG(NULL, NULL, "%s: removed at #%ld/%ld", getName(), (long)index, (long)m_local2parent.size());
        m_local2parent.erase(it);
        m_removedSignal(index, data);
        // Try adding more contacts from the parent if there is room now.
        fillView(parentIndex);
    }
}

void FilteredView::modifyIndividual(int parentIndex, const IndividualData &data)
{
    Entries_t::iterator it =
        std::lower_bound(m_local2parent.begin(),
                         m_local2parent.end(),
                         parentIndex);
    bool matches = m_filter->matches(data);
    if (it != m_local2parent.end() && *it == parentIndex) {
        // Was matched before the change.
        size_t index = it - m_local2parent.begin();
        if (matches) {
            // Still matched, merely pass on modification signal.
            SE_LOG_DEBUG(NULL, NULL, "%s: modified at #%ld/%ld", getName(), (long)index, (long)m_local2parent.size());
            m_modifiedSignal(index, data);
        } else {
            // Removed.
            SE_LOG_DEBUG(NULL, NULL, "%s: removed at #%ld/%ld due to modification", getName(), (long)index, (long)m_local2parent.size());
            int candidate = m_local2parent.back() + 1;
            m_local2parent.erase(it);
            m_removedSignal(index, data);
            fillView(candidate);
        }
    } else if (matches) {
        // Was not matched before and is matched now => add it.
        size_t index = it - m_local2parent.begin();
        if (m_filter->isIncluded(index)) {
            m_local2parent.insert(it, parentIndex);
            SE_LOG_DEBUG(NULL, NULL, "%s: added at #%ld/%ld due to modification", getName(), (long)index, (long)m_local2parent.size());
            m_addedSignal(index, data);
        }
    } else {
        // Neither matched before nor now => nothing changed.
    }
}

SE_END_CXX

