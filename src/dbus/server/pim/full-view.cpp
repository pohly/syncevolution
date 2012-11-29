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

#include "full-view.h"

#include <syncevo/BoostHelper.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

FullView::FullView(const FolksIndividualAggregatorCXX &folks,
                   const boost::shared_ptr<LocaleFactory> &locale) :
    m_folks(folks),
    m_locale(locale),
    m_isQuiescent(false),
    // Ensure that there is a sort criteria.
    m_compare(IndividualCompare::defaultCompare())
{
    setName("full view");
}

void FullView::init(const boost::shared_ptr<FullView> &self)
{
    m_self = self;
}

void FullView::doStart()
{
    // Populate view from current set of data. Usually FullView
    // gets instantiated when the aggregator is idle, in which
    // case there won't be any contacts yet.
    //
    // Optimize the initial loading by filling a vector and sorting it
    // more efficiently, then adding it all in one go.

    // Use pointers in array, to speed up sorting.
    boost::ptr_vector<IndividualData> individuals;
    IndividualData data;
    typedef GeeCollCXX< GeeMapEntryWrapper<const gchar *, FolksIndividual *> > Coll;
    GeeMap *map = folks_individual_aggregator_get_individuals(m_folks);
    Coll coll(map);
    guint size = gee_map_get_size(map);
    individuals.reserve(size);
    SE_LOG_DEBUG(NULL, NULL, "starting with %u individuals", size);
    BOOST_FOREACH (const Coll::value_type &entry, coll) {
        FolksIndividual *individual = entry.value();
        data.init(m_compare.get(), m_locale.get(), individual);
        individuals.push_back(new IndividualData(data));
    }
    individuals.sort(IndividualDataCompare(m_compare));

    // Copy the sorted data into the view in one go.
    m_entries.insert(m_entries.begin(), individuals.begin(), individuals.end());
    // Avoid loop if no-one is listening.
    if (!m_addedSignal.empty()) {
        for (size_t index = 0; index < m_entries.size(); index++) {
            m_addedSignal(index, m_entries[index]);
        }
    }

    // Connect to changes. Aggregator might live longer than we do, so
    // bind to weak pointer and check our existence at runtime.
    m_folks.connectSignal<void (FolksIndividualAggregator *folks,
                                GeeSet *added,
                                GeeSet *removed,
                                gchar  *message,
                                FolksPersona *actor,
                                FolksGroupDetailsChangeReason reason)>("individuals-changed",
                                                                       boost::bind(&FullView::individualsChanged,
                                                                                   m_self,
                                                                                   _2, _3, _4, _5, _6));
    // Track state as part of normal event processing. Don't check the
    // state directly, because then we might get into an inconsistent
    // state (changes still pending in our queue, function call
    // already returns true).
    m_isQuiescent = folks_individual_aggregator_get_is_quiescent(m_folks.get());
    m_folks.connectSignal<void (GObject *gobject,
                                GParamSpec *pspec)>("notify::is-quiescent",
                                                    boost::bind(&FullView::quiescenceChanged,
                                                                m_self));
}

boost::shared_ptr<FullView> FullView::create(const FolksIndividualAggregatorCXX &folks,
                                             const boost::shared_ptr<LocaleFactory> &locale)
{
    boost::shared_ptr<FullView> view(new FullView(folks, locale));
    view->init(view);
    return view;
}

void FullView::individualsChanged(GeeSet *added,
                                  GeeSet *removed,
                                  gchar *message,
                                  FolksPersona *actor,
                                  FolksGroupDetailsChangeReason reason)
{
    SE_LOG_DEBUG(NULL, NULL, "individuals changed, %s, %d added, %d removed, message: %s",
                 actor ? folks_persona_get_display_id(actor) : "<<no actor>>",
                 added ? gee_collection_get_size(GEE_COLLECTION(added)) : 0,
                 removed ? gee_collection_get_size(GEE_COLLECTION(removed)) : 0,
                 message);
    typedef GeeCollCXX<FolksIndividual *> Coll;
    // Remove first, to match the "remove + added = modified" change optimization
    // in Manager::handleChange().
    if (removed) {
        BOOST_FOREACH (FolksIndividual *individual, Coll(removed)) {
            removeIndividual(individual);
        }
    }
    if (added) {
        // TODO (?): Optimize adding many new individuals by pre-sorting them,
        // then using that information to avoid comparisons in addIndividual().
        BOOST_FOREACH (FolksIndividual *individual, Coll(added)) {
            addIndividual(individual);
        }
    }
}

void FullView::individualModified(gpointer gobject,
                                  GParamSpec *pspec)
{
    SE_LOG_DEBUG(NULL, NULL, "individual %p modified",
                 gobject);
    FolksIndividual *individual = FOLKS_INDIVIDUAL(gobject);
    // Delay the expensive modification check until the process is
    // idle, because in practice we get several change signals for
    // each contact change in EDS.
    //
    // See https://bugzilla.gnome.org/show_bug.cgi?id=684764
    // "too many FolksIndividual modification signals"
    m_pendingModifications.insert(individual);
    waitForIdle();
}

void FullView::quiescenceChanged()
{
    bool quiescent = folks_individual_aggregator_get_is_quiescent(m_folks);
    SE_LOG_DEBUG(NULL, NULL, "aggregator is %s", quiescent ? "quiescent" : "busy");
    // In practice, libfolks only switches from "busy" to "quiescent"
    // once. See https://bugzilla.gnome.org/show_bug.cgi?id=684766
    // "enter and leave quiescence state".
    if (quiescent) {
        int seconds = atoi(getEnv("SYNCEVOLUTION_PIM_DELAY_FOLKS", "0"));
        if (seconds > 0) {
            // Delay the quiescent state change as requested.
            SE_LOG_DEBUG(NULL, NULL, "delay aggregrator quiescence by %d seconds", seconds);
            m_quiescenceDelay.runOnce(seconds,
                                      boost::bind(&FullView::quiescenceChanged,
                                                  this));
            unsetenv("SYNCEVOLUTION_PIM_DELAY_FOLKS");
            return;
        }

        m_isQuiescent = true;
        m_quiescenceSignal();
    }
}

void FullView::doAddIndividual(Entries_t::auto_type &data)
{
    // Binary search to find insertion point.
    Entries_t::iterator it =
        std::lower_bound(m_entries.begin(),
                         m_entries.end(),
                         *data,
                         IndividualDataCompare(m_compare));
    size_t index = it - m_entries.begin();
    it = m_entries.insert(it, data.release());
    SE_LOG_DEBUG(NULL, NULL, "full view: added at #%ld/%ld", index, m_entries.size());
    m_addedSignal(index, *it);
    waitForIdle();

    // Monitor individual for changes.
    it->m_individual.connectSignal<void (GObject *gobject,
                                         GParamSpec *pspec)>("notify",
                                                             boost::bind(&FullView::individualModified,
                                                                         m_self,
                                                                         _1, _2));
}

void FullView::addIndividual(FolksIndividual *individual)
{
    Entries_t::auto_type data(new IndividualData);
    data->init(m_compare.get(), m_locale.get(), individual);
    doAddIndividual(data);
}

void FullView::modifyIndividual(FolksIndividual *individual)
{
    // Brute-force search for the individual. Pointer comparison is
    // sufficient, libfolks will not change instances without
    // announcing it.
    for (Entries_t::iterator it = m_entries.begin();
         it != m_entries.end();
         ++it) {
        if (it->m_individual.get() == individual) {
            size_t index = it - m_entries.begin();

            Entries_t::auto_type data(new IndividualData);
            data->init(m_compare.get(), m_locale.get(), individual);
            if (data->m_criteria != it->m_criteria &&
                ((it != m_entries.begin() && !m_compare->compare((it - 1)->m_criteria, data->m_criteria)) ||
                 (it + 1 != m_entries.end() && !m_compare->compare(data->m_criteria, (it + 1)->m_criteria)))) {
                // Sort criteria changed in such a way that the old
                // sorting became invalid => move the entry. Do it
                // as simple as possible, because this is not expected
                // to happen often.
                SE_LOG_DEBUG(NULL, NULL, "full view: temporarily removed at #%ld/%ld", index, m_entries.size());
                Entries_t::auto_type old = m_entries.release(it);
                m_removedSignal(index, *old);
                doAddIndividual(data);
            } else {
                SE_LOG_DEBUG(NULL, NULL, "full view: modified at #%ld/%ld", index, m_entries.size());
                // Use potentially modified pre-computed data.
                m_entries.replace(it, data.release());
                m_modifiedSignal(index, *it);
                waitForIdle();
            }
            return;
        }
    }
    // Not a bug: individual might have been removed before we got
    // around to processing the modification notification.
    SE_LOG_DEBUG(NULL, NULL, "full view: modified individual not found");
}

void FullView::removeIndividual(FolksIndividual *individual)
{
    for (Entries_t::iterator it = m_entries.begin();
         it != m_entries.end();
         ++it) {
        if (it->m_individual.get() == individual) {
            size_t index = it - m_entries.begin();
            SE_LOG_DEBUG(NULL, NULL, "full view: removed at #%ld/%ld", index, m_entries.size());
            Entries_t::auto_type data = m_entries.release(it);
            m_removedSignal(index, *data);
            waitForIdle();
            return;
        }
    }
    // A bug?!
    SE_LOG_DEBUG(NULL, NULL, "full view: individual to be removed not found");
}

void FullView::onIdle()
{
    SE_LOG_DEBUG(NULL, NULL, "process is idle");

    // Process delayed contact modifications.
    BOOST_FOREACH (const FolksIndividualCXX &individual,
                   m_pendingModifications) {
        modifyIndividual(const_cast<FolksIndividual *>(individual.get()));
    }
    m_pendingModifications.clear();

    // If not quiescent at the moment, then we can rely on getting
    // that signal triggered by folks and don't need to send it now.
    if (isQuiescent()) {
        m_quiescenceSignal();
    }
    m_waitForIdle.deactivate();
}

void FullView::waitForIdle()
{
    if (!m_waitForIdle) {
        m_waitForIdle.runOnce(-1, boost::bind(&FullView::onIdle, this));
    }
}

void FullView::setCompare(const boost::shared_ptr<IndividualCompare> &compare)
{
    m_compare = compare ?
        compare :
        IndividualCompare::defaultCompare();

    // Make a copy of the original order. The actual instances
    // continue to be owned by m_entries.
    boost::scoped_array<IndividualData *> old(new IndividualData *[m_entries.size()]);
    memcpy(old.get(), m_entries.c_array(), sizeof(IndividualData *) * m_entries.size());

    // Change sort criteria and sort.
    BOOST_FOREACH (IndividualData &data, m_entries) {
        data.init(m_compare.get(), NULL, data.m_individual);
    }
    m_entries.sort(IndividualDataCompare(m_compare));

    // Now check for changes.
    for (size_t index = 0; index < m_entries.size(); index++) {
        IndividualData &previous = *old[index],
            &current = m_entries[index];
        if (previous.m_individual != current.m_individual) {
            // Contact at the index changed. Don't try to find out
            // where it came from now. The effect is that temporarily
            // the same contact might be shown at two different
            // indices.
            m_modifiedSignal(index, current);
        }
    }

    // Current status is stable again (?), send out all modifications.
    if (isQuiescent()) {
        m_quiescenceSignal();
    }
}

SE_END_CXX

