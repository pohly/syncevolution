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
#include <syncevo/lcs.h>
#include <iterator>
#include <syncevo/BoostHelper.h>

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
    m_parent->m_quiescenceSignal.connect(boost::bind(&FilteredView::parentQuiescent, m_self));
}

void FilteredView::parentQuiescent()
{
    // State of the parent is stable again. Check if we queued a "fill view"
    // operation and do it now, before forwarding the quiescent signal.
    // This gives us the chance to add a contact before a previous remove
    // signal is sent, which then enables the combination of two signals
    // into one.
    if (m_fillViewOnIdle) {
        fillViewCb();
        m_fillViewOnIdle.deactivate();
    }
    m_quiescenceSignal();
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

bool FilteredView::isFull(const Entries_t &local2parent,
                          const boost::shared_ptr<IndividualFilter> &filter)
{
    size_t newEndIndex = local2parent.end() - local2parent.begin();
    return !filter->isIncluded(newEndIndex);
}

void FilteredView::fillViewCb()
{
    // Can we add back contacts which were excluded because of the
    // maximum number of results?
    SE_LOG_DEBUG(NULL, "filtered view %s: fill view on idle", getName());
    int candidate = m_local2parent.empty() ? 0 : m_local2parent.back() + 1;
    while (!isFull() &&
           candidate < m_parent->size()) {
        const IndividualData *data = m_parent->getContact(candidate);
        addIndividual(candidate, *data);
        candidate++;
    }
}

void FilteredView::fillView()
{
    if (!m_fillViewOnIdle) {
        m_fillViewOnIdle.runOnce(-1,
                                 boost::bind(&FilteredView::fillViewCb, this));
    }
}

void FilteredView::replaceFilter(const boost::shared_ptr<IndividualFilter> &individualFilter,
                                 bool refine)
{
    // Keep number of results the same, to avoid additional corner
    // cases.
    if (individualFilter->getMaxResults() != -1 &&
        individualFilter->getMaxResults() != m_filter->getMaxResults()) {
        SE_THROW("refining the search must not change the maximum number of results");
    }
    individualFilter->setMaxResults(m_filter->getMaxResults());

    if (refine) {
        // Take advantage of the hint that the search is more strict:
        // we know that we can limit searching to the contacts which
        // already matched the previous search.
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
            fillView();
        }
    } else {
        // Brute-force approach.
        //
        // Here is an example of old and new mapping:
        // index into local2parent old value     new value
        //    0                      10              10
        //    1                      20              30
        //    2                      30              40
        //    3                      50              50
        //    4                      70              60
        //    5                       -              70
        //    6                       -              80
        //
        // The LCS (see below) is:
        // (0, 0, 10) (2, 1, 30) (3, 3, 50) (4, 5, 70)
        //
        // The expected change signals for this transition are:
        // "removed", 1
        // "added", 2
        // "added", 4
        // "added", 6
        //
        // Note that this example does not include all corner cases.
        // Also relevant is adding or removing multiple entries at the
        // same index.
        //
        // One could also emit a "modified" signal for each index if
        // it is different, but then a single insertion or deletion
        // would led to invalidating the entire view.
        //
        // 1. build new result list.
        Entries_t local2parent;
        int candidate = 0;
        while (!isFull(local2parent, individualFilter) &&
               candidate < m_parent->size()) {
            const IndividualData *data = m_parent->getContact(candidate);
            if (individualFilter->matches(*data)) {
                local2parent.push_back(candidate);
            }
            candidate++;
        }

        // 2. morph existing one into new one.
        //
        // Uses the SyncEvolution longest-common-subsequence
        // algorithm.  Because all entries are different, there can be
        // only one solution and thus there is no need for a cost
        // function to find "better" solutions.
        std::vector< LCS::Entry<int> > common;
        common.reserve(std::min(m_local2parent.size(), local2parent.size()));
        LCS::lcs(m_local2parent, local2parent, std::back_inserter(common), LCS::accessor_sequence<Entries_t>());

        // To emit the discovered changes as "added" and "removed"
        // signals, we need to look at identical entries.

        // The "delta" here always represents the value "new = b" -
        // "old = a" which needs to be added to the old index to get
        // the new one. It summarizes the change signals emitted so
        // far. For each common entry, we need to check if the delta
        // is different from what we have told the agent so far.
        int delta = 0;

        // The "shift" is the "old modified" - "old original" that
        // tells us how many entries were added (positive value) or
        // removed (negative value) via signals.
        int shift = 0;

        // Old and new index represent the indices of the previous
        // common element plus 1; in other words, the expected next
        // common element.
        size_t oldIndex = 0,
            newIndex = 0;

        BOOST_FOREACH (const LCS::Entry<int> &entry, common) {
            int new_delta = (ssize_t)entry.index_b - (ssize_t)entry.index_a;
            if (delta != new_delta) {
                // When emitting "added" or "removed" signals,
                // be careful about getting the original index
                // in the old set right. It needs to be adjusted
                // to include the already sent changes.
                if (delta < new_delta) {
                    size_t change = new_delta - delta;
                    for (size_t i = 0; i < change; i++) {
                        const IndividualData *data = m_parent->getContact(local2parent[newIndex + i]);
                        // Keep adding at new indices, one new element
                        // after the other.
                        m_addedSignal(oldIndex - shift + i, *data);
                    }
                    shift -= change;
                } else if (delta > new_delta) {
                    size_t change = delta - new_delta;
                    shift += change;
                    for (size_t i = 0; i < change; i++) {
                        size_t index = oldIndex - shift;
                        const IndividualData *data = m_parent->getContact(m_local2parent[index + i]);
                        // Keep removing at the same index, because
                        // that's how it'll look to the recipient.
                        m_removedSignal(index, *data);
                    }
                }
                new_delta = delta;
            }
            oldIndex = entry.index_a + 1;
            newIndex = entry.index_b + 1;
        }

        // Now deal with entries after the latest common entry, in
        // both arrays.
        for (size_t index = oldIndex; index < m_local2parent.size(); index++) {
            const IndividualData *data = m_parent->getContact(m_local2parent[index]);
            m_removedSignal(oldIndex - shift, *data);
        }
        for (size_t index = newIndex; index < local2parent.size(); index++) {
            const IndividualData *data = m_parent->getContact(local2parent[index]);
            m_addedSignal(index, *data);
        }

        // - swap
        std::swap(m_local2parent, local2parent);
    }

    // If the parent is currently busy, then we can delay sending the
    // signal until it is no longer busy.
    if (isQuiescent()) {
        parentQuiescent();
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
                SE_LOG_DEBUG(NULL, "%s: removed at #%ld/%ld to make room for new entry", getName(), (long)(newEndIndex - 1), (long)m_local2parent.size());
                m_local2parent.pop_back();
                m_removedSignal(newEndIndex - 1, *data);
                // Iterator might have pointed to removed entry, which may have
                // invalidated it. Get fresh iterator based on index.
                it = m_local2parent.begin() + index;
            }

            m_local2parent.insert(it, parentIndex);
            SE_LOG_DEBUG(NULL, "%s: added at #%ld/%ld", getName(), (long)index, (long)m_local2parent.size());
            m_addedSignal(index, data);
        } else {
            SE_LOG_DEBUG(NULL, "%s: not added at #%ld/%ld because outside of result range", getName(), (long)index, (long)m_local2parent.size());
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
        SE_LOG_DEBUG(NULL, "%s: removed at #%ld/%ld", getName(), (long)index, (long)m_local2parent.size());
        m_local2parent.erase(it);
        m_removedSignal(index, data);
        // Try adding more contacts from the parent once the parent
        // is done with sending us changes - in other words, wait until
        // the process is idle.
        fillView();
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
            SE_LOG_DEBUG(NULL, "%s: modified at #%ld/%ld", getName(), (long)index, (long)m_local2parent.size());
            m_modifiedSignal(index, data);
        } else {
            // Removed.
            SE_LOG_DEBUG(NULL, "%s: removed at #%ld/%ld due to modification", getName(), (long)index, (long)m_local2parent.size());
            m_local2parent.erase(it);
            m_removedSignal(index, data);
            fillView();
        }
    } else if (matches) {
        // Was not matched before and is matched now => add it.
        size_t index = it - m_local2parent.begin();
        if (m_filter->isIncluded(index)) {
            m_local2parent.insert(it, parentIndex);
            SE_LOG_DEBUG(NULL, "%s: added at #%ld/%ld due to modification", getName(), (long)index, (long)m_local2parent.size());
            m_addedSignal(index, data);
        }
    } else {
        // Neither matched before nor now => nothing changed.
    }
}

SE_END_CXX

