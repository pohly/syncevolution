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

#include <config.h>
#include "folks.h"
#include "individual-traits.h"
#include "persona-details.h"
#include <boost/bind.hpp>
#include "test.h"
#include <syncevo/BoostHelper.h>
#include <syncevo/LogRedirect.h>

#include <boost/ptr_container/ptr_vector.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * Generic error callback. There really isn't much that can be done if
 * libfolks fails, except for logging the problem.
 */
static void logResult(const GError *gerror, const char *operation)
{
    if (gerror) {
        SE_LOG_ERROR(NULL, NULL, "%s: %s", operation, gerror->message);
    } else {
        SE_LOG_DEBUG(NULL, NULL, "%s: done", operation);
    }
}

class CompareFormattedName : public IndividualCompare {
    bool m_reversed;
    bool m_firstLast;

public:
    CompareFormattedName(bool reversed = false, bool firstLast = false) :
        m_reversed(reversed),
        m_firstLast(firstLast)
    {
    }

    virtual void createCriteria(FolksIndividual *individual, Criteria_t &criteria) const {
        FolksStructuredName *fn =
            folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(individual));
        if (fn) {
            const char *family = folks_structured_name_get_family_name(fn);
            const char *given = folks_structured_name_get_given_name(fn);
            SE_LOG_DEBUG(NULL, NULL, "criteria: formatted name: %s, %s",
                         family, given);
            if (m_firstLast) {
                criteria.push_back(given ? given : "");
                criteria.push_back(family ? family : "");
            } else {
                criteria.push_back(family ? family : "");
                criteria.push_back(given ? given : "");
            }
        } else {
            SE_LOG_DEBUG(NULL, NULL, "criteria: no formatted");
        }
    }

    virtual bool compare(const Criteria_t &a, const Criteria_t &b) const {
        return m_reversed ?
            IndividualCompare::compare(b, a) :
            IndividualCompare::compare(a, b);
    }
};

void IndividualData::init(const boost::shared_ptr<IndividualCompare> &compare,
                          FolksIndividual *individual)
{
    m_individual = individual;
    m_criteria.clear();
    compare->createCriteria(individual, m_criteria);
}

void IndividualView::start()
{
    if (!m_started) {
        m_started = true;
        doStart();
    }
}

bool IndividualView::isRunning() const
{
    return m_started;
}

void IndividualView::findContact(const std::string &id, int hint, int &index, FolksIndividualCXX &individual)
{
    int i;
    int count = size();
    // Start searching at the hint.
    for (i = hint; i < count; i++) {
        individual = getContact(i);
        if (id == folks_individual_get_id(individual.get())) {
            index = i;
            return;
        }
    }
    // Finish search before the hint.
    for (i = 0; i < hint; i++) {
        individual = getContact(i);
        if (id == folks_individual_get_id(individual.get())) {
            index = i;
            return;
        }
    }

    // Nothing found.
    index = -1;
    individual.reset();
}

void IndividualView::readContacts(const std::vector<std::string> &ids, Contacts &contacts)
{
    contacts.clear();
    contacts.reserve(ids.size());

    // The search is optimized for the case where many consecutive
    // contacts in increasing order are requested. For that case, a
    // linear search is needed for the first contact and then the
    // following ones are found in constant time.
    //
    // Randomly requesting contacts performs poorly, due to the O(n)
    // lookup complexity.
    int hint = 0;
    BOOST_FOREACH (const std::string &id, ids) {
        int index;
        FolksIndividualCXX individual;
        findContact(id, hint, index, individual);
        contacts.push_back(std::make_pair(index, individual));
        if (index >= 0) {
            hint = index;
        }
    }
}

bool IndividualCompare::compare(const Criteria_t &a, const Criteria_t &b) const
{
    Criteria_t::const_iterator ita = a.begin(),
        itb = b.begin();

    while (itb != b.end()) {
        if (ita == a.end()) {
            // a is shorter
            return true;
        }
        int cmp = ita->compare(*itb);
        if (cmp < 0) {
            // String comparison shows that a is less than b.
            return true;
        } else if (cmp > 0) {
            // Is greater, so definitely not less => don't compare
            // rest of the criteria.
            return false;
        } else {
            // Equal, continue comparing.
            ++ita;
            ++itb;
        }
    }

    // a is not less b
    return false;
}

FullView::FullView(const FolksIndividualAggregatorCXX &folks) :
    m_folks(folks),
    // Ensure that there is a sort criteria.
    m_compare(new CompareFormattedName())
{
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
        data.init(m_compare, entry.value());
        individuals.push_back(new IndividualData(data));
    }
    individuals.sort(IndividualDataCompare(m_compare));

    // Copy the sorted data into the view in one go.
    m_entries.insert(m_entries.begin(), individuals.begin(), individuals.end());
    // Avoid loop if no-one is listening.
    if (!m_addedSignal.empty()) {
        for (size_t index = 0; index < m_entries.size(); index++) {
            m_addedSignal(index, m_entries[index].m_individual);
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
    m_folks.connectSignal<void (GObject *gobject,
                                GParamSpec *pspec)>("notify::is-quiescent",
                                                    boost::bind(&FullView::quiesenceChanged,
                                                                m_self));


}

boost::shared_ptr<FullView> FullView::create(const FolksIndividualAggregatorCXX &folks)
{
    boost::shared_ptr<FullView> view(new FullView(folks));
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

void FullView::quiesenceChanged()
{
    bool quiesent = folks_individual_aggregator_get_is_quiescent(m_folks);
    SE_LOG_DEBUG(NULL, NULL, "aggregator is %s", quiesent ? "quiesent" : "busy");
    // In practice, libfolks only switches from "busy" to "quiesent"
    // once. See https://bugzilla.gnome.org/show_bug.cgi?id=684766
    // "enter and leave quiesence state".
    if (quiesent) {
        m_quiesenceSignal();
    }
}

void FullView::doAddIndividual(const IndividualData &data)
{
    // Binary search to find insertion point.
    Entries_t::iterator it =
        std::lower_bound(m_entries.begin(),
                         m_entries.end(),
                         data,
                         IndividualDataCompare(m_compare));
    size_t index = it - m_entries.begin();
    it = m_entries.insert(it, data);
    SE_LOG_DEBUG(NULL, NULL, "full view: added at #%ld/%ld", index, m_entries.size());
    m_addedSignal(index, it->m_individual);
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
    IndividualData data;
    data.init(m_compare, individual);
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

            IndividualData data;
            data.init(m_compare, individual);
            if (data.m_criteria != it->m_criteria &&
                ((it != m_entries.begin() && !m_compare->compare((it - 1)->m_criteria, data.m_criteria)) ||
                 (it + 1 != m_entries.end() && !m_compare->compare(data.m_criteria, (it + 1)->m_criteria)))) {
                // Sort criteria changed in such a way that the old
                // sorting became invalid => move the entry. Do it
                // as simple as possible, because this is not expected
                // to happen often.
                SE_LOG_DEBUG(NULL, NULL, "full view: temporarily removed at #%ld/%ld", index, m_entries.size());
                m_entries.erase(it);
                m_removedSignal(index, individual);
                doAddIndividual(data);
            } else {
                SE_LOG_DEBUG(NULL, NULL, "full view: modified at #%ld/%ld", index, m_entries.size());
                m_modifiedSignal(index, individual);
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
            m_entries.erase(it);
            m_removedSignal(index, individual);
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

    m_quiesenceSignal();
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
    if (!compare) {
        // Fall back to debug ordering.
        m_compare.reset(new CompareFormattedName());
    } else {
        m_compare = compare;
    }

    // Reorder a copy of the current data.
    Entries_t entries(m_entries);
    BOOST_FOREACH (IndividualData &data, entries) {
        data.init(m_compare, data.m_individual);
    }
    std::sort(entries.begin(), entries.end(), IndividualDataCompare(m_compare));

    // Now update real array.
    for (size_t index = 0; index < entries.size(); index++) {
        IndividualData &previous = m_entries[index],
            &current = entries[index];
        if (previous.m_individual != current.m_individual) {
            // Contact at the index changed. Don't try to find out
            // where it came from now. The effect is that temporarily
            // the same contact might be shown at two different
            // indices.
            m_modifiedSignal(index, current.m_individual);
        }
        // Ensure that m_entries is up-to-date, whatever the change
        // may have been.
        std::swap(previous, current);
    }

    // Current status is stable again, send out all modifications.
    m_quiesenceSignal();
}

FilteredView::FilteredView(const boost::shared_ptr<IndividualView> &parent,
                           const boost::shared_ptr<IndividualFilter> &filter) :
    m_parent(parent),
    m_filter(filter)
{
}

void FilteredView::init(const boost::shared_ptr<FilteredView> &self)
{
    m_self = self;
    m_parent->m_quiesenceSignal.connect(QuiesenceSignal_t::slot_type(boost::bind(boost::cref(m_quiesenceSignal))).track(m_self));
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
    // Add initial content. Our processing of the new contact must not
    // cause changes to the parent view, otherwise the result will not
    // be inconsistent.
    for (int index = 0; index < m_parent->size(); index++) {
        addIndividual(index, m_parent->getContact(index));
    }

    // No more changes expected for a while, because usually a FilteredView
    // is a search on a stable FullView. Notify listeners. Necessary to
    // get changes flushed.
    m_quiesenceSignal();

    // Start listening to signals.
    m_parent->m_addedSignal.connect(ChangeSignal_t::slot_type(boost::bind(&FilteredView::addIndividual, this, _1, _2)).track(m_self));
    m_parent->m_modifiedSignal.connect(ChangeSignal_t::slot_type(boost::bind(&FilteredView::modifyIndividual, this, _1, _2)).track(m_self));
    m_parent->m_removedSignal.connect(ChangeSignal_t::slot_type(boost::bind(&FilteredView::removeIndividual, this, _1, _2)).track(m_self));
}

void FilteredView::refineFilter(const boost::shared_ptr<IndividualFilter> &individualFilter)
{
    size_t index = 0;
    while (index < m_local2parent.size()) {
        FolksIndividualCXX individual = m_parent->getContact(index);
        if (individualFilter->matches(individual)) {
            // Still matched, just skip it.
            ++index;
        } else {
            // No longer matched, remove it.
            m_removedSignal(index, individual);
            m_local2parent.erase(m_local2parent.begin() + index);
        }
    }
    m_filter = individualFilter;
    m_quiesenceSignal();
}

void FilteredView::addIndividual(int parentIndex, FolksIndividual *individual)
{
    if (!individual) {
        // sanity check
        return;
    }

    if (m_filter->matches(individual)) {
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
        size_t index = it - m_local2parent.begin();
        it = m_local2parent.insert(it, parentIndex);
        ++it;
        // Shift all following indices.
        while (it != m_local2parent.end()) {
            (*it)++;
            ++it;
        }

        SE_LOG_DEBUG(NULL, NULL, "filtered view: added at #%ld/%ld", index, m_local2parent.size());
        m_addedSignal(index, individual);
    }
}

void FilteredView::removeIndividual(int parentIndex, FolksIndividual *individual)
{
    if (!individual) {
        // sanity check
        return;
    }

    // The entries are sorted. Therefore we can use a binary search
    // to find the parentIndex or the first entry after it.
    Entries_t::iterator it =
        std::lower_bound(m_local2parent.begin(),
                         m_local2parent.end(),
                         parentIndex);
    if (it != m_local2parent.end() && *it == parentIndex) {
        size_t index = it - m_local2parent.begin();
        SE_LOG_DEBUG(NULL, NULL, "filtered view: removed at #%ld/%ld", index, m_local2parent.size());
        it = m_local2parent.erase(it);
        m_removedSignal(index, individual);
    }

    // Now reduce the index in our mapping for all following entries.
    // Not particularly efficient when multiple individuals get
    // removed.
    while (it != m_local2parent.end()) {
        (*it)--;
        ++it;
    }
}

void FilteredView::modifyIndividual(int parentIndex, FolksIndividual *individual)
{
    if (!individual) {
        // sanity check
        return;
    }

    Entries_t::iterator it =
        std::lower_bound(m_local2parent.begin(),
                         m_local2parent.end(),
                         parentIndex);
    bool matches = m_filter->matches(individual);
    if (it != m_local2parent.end() && *it == parentIndex) {
        // Was matched before the change.
        size_t index = it - m_local2parent.begin();
        if (matches) {
            // Still matched, merely pass on modification signal.
            SE_LOG_DEBUG(NULL, NULL, "filtered view: modified at #%ld/%ld", index, m_local2parent.size());
            m_modifiedSignal(index, individual);
        } else {
            // Removed.
            SE_LOG_DEBUG(NULL, NULL, "filtered view: removed at #%ld/%ld due to modification", index, m_local2parent.size());
            m_local2parent.erase(it);
            m_removedSignal(index, individual);
        }
    } else if (matches) {
        // Was not matched before and is matched now => add it.
        size_t index = it - m_local2parent.begin();
        m_local2parent.insert(it, parentIndex);
        SE_LOG_DEBUG(NULL, NULL, "filtered view: added at #%ld/%ld due to modification", index, m_local2parent.size());
        m_addedSignal(index, individual);
    } else {
        // Neither matched before nor now => nothing changed.
    }
}

IndividualAggregator::IndividualAggregator() :
    m_databases(gee_hash_set_new(G_TYPE_STRING, (GBoxedCopyFunc) g_strdup, g_free, NULL, NULL), false)
{
}

void IndividualAggregator::init(boost::shared_ptr<IndividualAggregator> &self)
{
    m_self = self;
    m_backendStore =
        FolksBackendStoreCXX::steal(folks_backend_store_dup());

    // Ignore some known harmless messages from folks.
    LogRedirect::addIgnoreError("Error preparing Backend 'ofono'");
    LogRedirect::addIgnoreError("Error preparing Backend 'telepathy'");

    // Have to hard-code the list of known backends that we don't want.
    SYNCEVO_GLIB_CALL_ASYNC(folks_backend_store_disable_backend,
                            boost::bind(logResult, (const GError *)NULL,
                                        "folks_backend_store_disable_backend"),
                            m_backendStore, "telepathy");
    SYNCEVO_GLIB_CALL_ASYNC(folks_backend_store_disable_backend,
                            boost::bind(logResult, (const GError *)NULL,
                                        "folks_backend_store_disable_backend"),
                            m_backendStore, "tracker");
    SYNCEVO_GLIB_CALL_ASYNC(folks_backend_store_disable_backend,
                            boost::bind(logResult, (const GError *)NULL,
                                        "folks_backend_store_disable_backend"),
                            m_backendStore, "key-file");
    SYNCEVO_GLIB_CALL_ASYNC(folks_backend_store_disable_backend,
                            boost::bind(logResult, (const GError *)NULL,
                                        "folks_backend_store_disable_backend"),
                            m_backendStore, "libsocialweb");
    // Explicitly enable EDS, just to be sure.
    SYNCEVO_GLIB_CALL_ASYNC(folks_backend_store_enable_backend,
                            boost::bind(logResult, (const GError *)NULL,
                                        "folks_backend_store_enable_backend"),
                            m_backendStore, "eds");

    // Start loading backends right away.
    SYNCEVO_GLIB_CALL_ASYNC(folks_backend_store_load_backends,
                            boost::bind(&IndividualAggregator::backendsLoaded, m_self),
                            m_backendStore);

    m_folks =
        FolksIndividualAggregatorCXX::steal(folks_individual_aggregator_new_with_backend_store(m_backendStore));
}

boost::shared_ptr<IndividualAggregator> IndividualAggregator::create()
{
    boost::shared_ptr<IndividualAggregator> aggregator(new IndividualAggregator);
    aggregator->init(aggregator);
    return aggregator;
}

std::string IndividualAggregator::dumpDatabases()
{
    std::string res;

    BOOST_FOREACH (const gchar *tmp, GeeStringCollection(GEE_COLLECTION(m_databases.get()))) {
        if (!res.empty()) {
            res += ", ";
        }
        res += tmp;
    }
    return res;
}

void IndividualAggregator::backendsLoaded()
{
    SE_LOG_DEBUG(NULL, NULL, "backend store has loaded backends");
    GeeCollectionCXX coll(folks_backend_store_list_backends(m_backendStore));
    BOOST_FOREACH (FolksBackend *backend, GeeCollCXX<FolksBackend *>(coll.get())) {
        SE_LOG_DEBUG(NULL, NULL, "folks backend: %s", folks_backend_get_name(backend));
    }
    m_eds =
        FolksBackendCXX::steal(folks_backend_store_dup_backend_by_name(m_backendStore, "eds"));
    if (m_eds) {
        // Remember system store, for writing contacts.
        GeeMap *stores = folks_backend_get_persona_stores(m_eds);
        FolksPersonaStore *systemStore = static_cast<FolksPersonaStore *>(gee_map_get(stores, "system-address-book"));
        m_systemStore = systemStore;

        // Tell the backend which databases we want.
        SE_LOG_DEBUG(NULL, NULL, "backends loaded: setting EDS persona stores: [%s]",
                     dumpDatabases().c_str());
        folks_backend_set_persona_stores(m_eds, GEE_SET(m_databases.get()));

        if (m_view) {
            // We were started, prepare aggregator.
            SYNCEVO_GLIB_CALL_ASYNC(folks_individual_aggregator_prepare,
                                    boost::bind(logResult, _1,
                                                "folks_individual_aggregator_prepare"),
                                    getFolks());
        }

        // Execute delayed work.
        m_backendsLoadedSignal();
    } else {
        SE_LOG_ERROR(NULL, NULL, "EDS backend not active?!");
    }
}

void IndividualAggregator::setDatabases(std::set<std::string> &databases)
{
    gee_collection_clear(GEE_COLLECTION(m_databases.get()));
    BOOST_FOREACH (const std::string &database, databases) {
        gee_collection_add(GEE_COLLECTION(m_databases.get()), database.c_str());
    }

    if (m_eds) {
        // Backend is loaded, tell it about the change.
        folks_backend_set_persona_stores(m_eds, GEE_SET(m_databases.get()));
    }
}

void IndividualAggregator::setCompare(const boost::shared_ptr<IndividualCompare> &compare)
{
    // Don't start main view. Instead rememeber the compare instance
    // for start().
    m_compare = compare;
    if (m_view) {
        m_view->setCompare(compare);
    }
}

void IndividualAggregator::start()
{
    if (!m_view) {
        m_view = FullView::create(m_folks);
        if (m_compare) {
            m_view->setCompare(m_compare);
        }
        if (m_eds) {
            // Backend was loaded and configured, we can prepare the aggregator.
            SYNCEVO_GLIB_CALL_ASYNC(folks_individual_aggregator_prepare,
                                    boost::bind(logResult, _1,
                                                "folks_individual_aggregator_prepare"),
                                    getFolks());
        }
    }
}

bool IndividualAggregator::isRunning() const
{
    return m_view;
}

boost::shared_ptr<FullView> IndividualAggregator::getMainView()
{
    if (!m_view) {
        start();
    }
    return m_view;
}

void IndividualAggregator::addContact(const Result<void (const std::string &)> &result,
                                      const PersonaDetails &details)
{
    // Called directly by D-Bus client. Need fully functional system address book.
    runWithAddressBook(boost::bind(&IndividualAggregator::doAddContact,
                                   this,
                                   result,
                                   details),
                       result.getOnError());
}

void IndividualAggregator::doAddContact(const Result<void (const std::string &)> &result,
                                        const PersonaDetails &details)
{
    SYNCEVO_GLIB_CALL_ASYNC(folks_persona_store_add_persona_from_details,
                            boost::bind(&IndividualAggregator::addContactDone,
                                        this,
                                        _1, _2,
                                        result),
                            m_systemStore,
                            details.get());
}

void IndividualAggregator::addContactDone(const GError *gerror,
                                          FolksPersona *persona,
                                          const Result<void (const std::string &)> &result) throw()
{
    try {
        // Handle result of folks_persona_store_add_persona_from_details().
        if (gerror) {
            GErrorCXX::throwError("add contact", gerror);
        }

        const gchar *uid = folks_persona_get_uid(persona);
        if (uid) {
            gchar *backend, *storeID, *personaID;
            folks_persona_split_uid(uid, &backend, &storeID, &personaID);
            PlainGStr tmp1(backend), tmp2(storeID), tmp3(personaID);
            result.done(personaID);
        } else {
            SE_THROW("new persona has empty UID");
        }
    } catch (...) {
        result.failed();
    }
}

void IndividualAggregator::modifyContact(const Result<void ()> &result,
                                         const std::string &localID,
                                         const PersonaDetails &details)
{
    runWithPersona(boost::bind(&IndividualAggregator::doModifyContact,
                               this,
                               result,
                               _1,
                               details),
                   localID,
                   result.getOnError());
}

void IndividualAggregator::doModifyContact(const Result<void ()> &result,
                                           FolksPersona *persona,
                                           const PersonaDetails &details) throw()
{
    try {
        // Asynchronously modify the persona. This will be turned into
        // EDS updates by folks.
        Details2Persona(result, details, persona);
    } catch (...) {
        result.failed();
    }
}

void IndividualAggregator::removeContact(const Result<void ()> &result,
                                         const std::string &localID)
{
    runWithPersona(boost::bind(&IndividualAggregator::doRemoveContact,
                               this,
                               result,
                               _1),
                   localID,
                   result.getOnError());
}

void IndividualAggregator::doRemoveContact(const Result<void ()> &result,
                                           FolksPersona *persona) throw()
{
    try {
        SYNCEVO_GLIB_CALL_ASYNC(folks_persona_store_remove_persona,
                                boost::bind(&IndividualAggregator::removeContactDone,
                                            this,
                                            _1,
                                            result),
                                m_systemStore,
                                persona);
    } catch (...) {
        result.failed();
    }
}

void IndividualAggregator::removeContactDone(const GError *gerror,
                                             const Result<void ()> &result) throw()
{
    try {
        if (gerror) {
            GErrorCXX::throwError("remove contact", gerror);
        }
        result.done();
    } catch (...) {
        result.failed();
    }
}


void IndividualAggregator::runWithAddressBook(const boost::function<void ()> &operation,
                                              const ErrorCb_t &onError) throw()
{
    try {
        if (m_eds) {
            runWithAddressBookHaveEDS(boost::signals2::connection(),
                                      operation,
                                      onError);
        } else {
            // Do it later.
            m_backendsLoadedSignal.connect_extended(boost::bind(&IndividualAggregator::runWithAddressBookHaveEDS,
                                                                this,
                                                                _1,
                                                                operation,
                                                                onError));
        }
    } catch (...) {
        onError();
    }
}

void IndividualAggregator::runWithAddressBookHaveEDS(const boost::signals2::connection &conn,
                                                     const boost::function<void ()> &operation,
                                                     const ErrorCb_t &onError) throw()
{
    try {
        // Called after we obtained EDS backend. Need system store
        // which is prepared.
        m_backendsLoadedSignal.disconnect(conn);
        if (!m_systemStore) {
            SE_THROW("system address book not found");
        }
        if (folks_persona_store_get_is_prepared(m_systemStore)) {
            runWithAddressBookPrepared(NULL,
                                       operation,
                                       onError);
        } else {
            SYNCEVO_GLIB_CALL_ASYNC(folks_persona_store_prepare,
                                    boost::bind(&IndividualAggregator::runWithAddressBookPrepared,
                                                this,
                                                _1,
                                                operation,
                                                onError),
                                    m_systemStore);
        }
    } catch (...) {
        onError();
    }
}

void IndividualAggregator::runWithAddressBookPrepared(const GError *gerror,
                                                      const boost::function<void ()> &operation,
                                                      const ErrorCb_t &onError) throw()
{
    try {
        // Called after EDS system store is prepared, successfully or unsuccessfully.
        if (gerror) {
            GErrorCXX::throwError("prepare EDS system address book", gerror);
        }
        operation();
    } catch (...) {
        onError();
    }
}

void IndividualAggregator::runWithPersona(const boost::function<void (FolksPersona *)> &operation,
                                          const std::string &localID,
                                          const ErrorCb_t &onError) throw()
{
    try {
        runWithAddressBook(boost::bind(&IndividualAggregator::doRunWithPersona,
                                       this,
                                       operation,
                                       localID,
                                       onError),
                           onError);
    } catch (...) {
        onError();
    }
}

void IndividualAggregator::doRunWithPersona(const boost::function<void (FolksPersona *)> &operation,
                                            const std::string &localID,
                                            const ErrorCb_t &onError) throw()
{
    try {
        GeeMap *personas = folks_persona_store_get_personas(m_systemStore);
        typedef GeeCollCXX< GeeMapEntryWrapper<const gchar *, FolksPersona *> > Coll;
        BOOST_FOREACH (const Coll::value_type &entry, Coll(personas)) {
            // key seems to be <store id>:<persona ID>
            const gchar *key = entry.key();
            const gchar *colon = strchr(key, ':');
            if (colon && localID == colon + 1) {
                operation(entry.value());
                return;
            }
        }
        SE_THROW(StringPrintf("contact with local ID '%s' not found in system address book", localID.c_str()));
    } catch (...) {
        onError();
    }
}

#ifdef ENABLE_UNIT_TESTS

class FolksTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(FolksTest);
    CPPUNIT_TEST(open);
    CPPUNIT_TEST(asyncError);
    CPPUNIT_TEST(gvalue);

    // The tests are not working because it is currently not possible
    // to create FolksIndividual instances directly with some chosen
    // properties. Testing the FullView and IndividualFilter thus must
    // be done by putting real data into EDS and reading it via libfolks.
    // See testpim.py.
    // CPPUNIT_TEST(fullview);
    // CPPUNIT_TEST(filter);

    // aggregate() works, but doesn't do anything. Keeping it and the
    // other two tests as compile test for the API.
    CPPUNIT_TEST(aggregate);
    CPPUNIT_TEST_SUITE_END();

    FolksIndividualCXX m_contactA,
        m_contactB,
        m_contactC;

public:
    void setUp()
    {
        FolksIndividualCXX contact;
        FolksStructuredName *fn;

        contact = FolksIndividualCXX::steal(folks_individual_new(NULL));
        FolksNameDetails *nd = FOLKS_NAME_DETAILS(contact.get());

        // Crashes due to bug in Vala:
        // https://bugzilla.gnome.org/show_bug.cgi?id=684557
        // GErrorCXX gerror;
        // SYNCEVO_GLIB_CALL_SYNC(NULL, gerror,
        //                        folks_name_details_change_full_name,
        //                        nd,
        //                        "Abraham Zzz");
        // if (gerror) {
        //     gerror.throwError("folks_name_details_change_full_name(Abraham Zzz)");
        // }

        // Doesn't have any effect: without an associated store,
        // properties in a FolksIndividual are not writable.
        folks_name_details_set_full_name(nd, "Abraham Zzz");

        fn = folks_name_details_get_structured_name(nd);
        folks_structured_name_set_family_name(fn, "Zzz");
        folks_structured_name_set_given_name(fn, "Abraham");
        m_contactA = contact;

        contact = FolksIndividualCXX::steal(folks_individual_new(NULL));
        folks_name_details_set_full_name(FOLKS_NAME_DETAILS(contact.get()), "Benjamin Yyy");
        fn = folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(contact.get()));
        folks_structured_name_set_family_name(fn, "Yyy");
        folks_structured_name_set_given_name(fn, "Benjamin");
        m_contactB = contact;

        contact = FolksIndividualCXX::steal(folks_individual_new(NULL));
        folks_name_details_set_full_name(FOLKS_NAME_DETAILS(contact.get()), "Charly Xxx");
        fn = folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(contact.get()));
        folks_structured_name_set_family_name(fn, "Xxx");
        folks_structured_name_set_given_name(fn, "Charly");
        m_contactC = contact;
    }

private:
    static void asyncCB(const GError *gerror, const char *func, bool &failed, bool &done) {
        done = true;
        if (gerror) {
            failed = true;
            SE_LOG_ERROR(NULL, NULL, "%s: %s", func, gerror->message);
        }
    }

    void open() {
        FolksIndividualAggregatorCXX aggregator(folks_individual_aggregator_new(), false);
        bool done = false, failed = false;
        SYNCEVO_GLIB_CALL_ASYNC(folks_individual_aggregator_prepare,
                                boost::bind(asyncCB, _1,
                                            "folks_individual_aggregator_prepare",
                                            boost::ref(failed), boost::ref(done)),
                                aggregator);

        while (!done) {
            g_main_context_iteration(NULL, true);
        }
        CPPUNIT_ASSERT(!failed);

        while (!folks_individual_aggregator_get_is_quiescent(aggregator)) {
            g_main_context_iteration(NULL, true);
        }

        GeeMap *individuals = folks_individual_aggregator_get_individuals(aggregator);
        SE_LOG_DEBUG(NULL, NULL, "%d individuals", gee_map_get_size(individuals));

        GeeMapIteratorCXX it(gee_map_map_iterator(individuals), false);
        while (gee_map_iterator_next(it)) {
            PlainGStr id(reinterpret_cast<gchar *>(gee_map_iterator_get_key(it)));
            FolksIndividualCXX individual(reinterpret_cast<FolksIndividual *>(gee_map_iterator_get_value(it)),
                                          false);
            GValueStringCXX fullname;
            g_object_get_property(G_OBJECT(individual.get()), "full-name", &fullname);
            SE_LOG_DEBUG(NULL, NULL, "map: id %s name %s = %s",
                         id.get(),
                         fullname.toString().c_str(),
                         fullname.get());
        }

        GeeIteratorCXX it2(gee_iterable_iterator(GEE_ITERABLE(individuals)), false);
        while (gee_iterator_next(it2)) {
            GeeMapEntryCXX entry(reinterpret_cast<GeeMapEntry *>(gee_iterator_get(it2)), false);
            gchar *id(reinterpret_cast<gchar *>(const_cast<gpointer>(gee_map_entry_get_key(entry))));
            FolksIndividual *individual(reinterpret_cast<FolksIndividual *>(const_cast<gpointer>(gee_map_entry_get_value(entry))));
            GValueStringCXX fullname;
            g_object_get_property(G_OBJECT(individual), "full-name", &fullname);
            SE_LOG_DEBUG(NULL, NULL, "iterable: id %s name %s = %s",
                         id,
                         fullname.toString().c_str(),
                         fullname.get());
        }

        typedef GeeCollCXX< GeeMapEntryWrapper<const gchar *, FolksIndividual *> > Coll;
        Coll coll(individuals);
        Coll::const_iterator curr = coll.begin();
        Coll::const_iterator end = coll.end();
        if (curr != end) {
            const gchar *id = (*curr).key();
            FolksIndividual *individual((*curr).value());
            GValueStringCXX fullname;
            g_object_get_property(G_OBJECT(individual), "full-name", &fullname);

            SE_LOG_DEBUG(NULL, NULL, "first: id %s name %s = %s",
                         id,
                         fullname.toString().c_str(),
                         fullname.get());
            ++curr;
        }

        BOOST_FOREACH (Coll::value_type &entry, Coll(individuals)) {
            const gchar *id = entry.key();
            FolksIndividual *individual(entry.value());
            // GValueStringCXX fullname;
            // g_object_get_property(G_OBJECT(individual), "full-name", &fullname);
            const gchar *fullname = folks_name_details_get_full_name(FOLKS_NAME_DETAILS(individual));

            SE_LOG_DEBUG(NULL, NULL, "boost: id %s %s name %s",
                         id,
                         fullname ? "has" : "has no",
                         fullname);

            GeeSet *emails = folks_email_details_get_email_addresses(FOLKS_EMAIL_DETAILS(individual));
            SE_LOG_DEBUG(NULL, NULL, "     %d emails", gee_collection_get_size(GEE_COLLECTION(emails)));
            typedef GeeCollCXX<FolksEmailFieldDetails *> EmailColl;
            BOOST_FOREACH (FolksEmailFieldDetails *email, EmailColl(emails)) {
                SE_LOG_DEBUG(NULL, NULL, "     %s",
                             reinterpret_cast<const gchar *>(folks_abstract_field_details_get_value(FOLKS_ABSTRACT_FIELD_DETAILS(email))));
            }
        }

        aggregator.reset();
    }

    void gvalue() {
        GValueBooleanCXX b(true);
        SE_LOG_DEBUG(NULL, NULL, "GValueBooleanCXX(true) = %s", b.toString().c_str());
        GValueBooleanCXX b2(b);
        CPPUNIT_ASSERT_EQUAL(b.get(), b2.get());
        b2.set(false);
        CPPUNIT_ASSERT_EQUAL(b.get(), (gboolean)!b2.get());
        b2 = b;
        CPPUNIT_ASSERT_EQUAL(b.get(), b2.get());

        GValueStringCXX str("foo bar");
        SE_LOG_DEBUG(NULL, NULL, "GValueStringCXX(\"foo bar\") = %s", str.toString().c_str());
        CPPUNIT_ASSERT(!strcmp(str.get(), "foo bar"));
        GValueStringCXX str2(str);
        CPPUNIT_ASSERT(!strcmp(str.get(), str2.get()));
        CPPUNIT_ASSERT(str.get() != str2.get());
        str2.set("foo");
        CPPUNIT_ASSERT(strcmp(str.get(), str2.get()));
        CPPUNIT_ASSERT(str.get() != str2.get());
        str2 = str;
        CPPUNIT_ASSERT(!strcmp(str.get(), str2.get()));
        CPPUNIT_ASSERT(str.get() != str2.get());
        str2.take(g_strdup("bar"));
        CPPUNIT_ASSERT(strcmp(str.get(), str2.get()));
        CPPUNIT_ASSERT(str.get() != str2.get());
        const char *fixed = "fixed";
        str2.setStatic(fixed);
        CPPUNIT_ASSERT(!strcmp(str2.get(), fixed));
        CPPUNIT_ASSERT(str2.get() == fixed);
    }

    void asyncError() {
        bool done = false, failed = false;
        SYNCEVO_GLIB_CALL_ASYNC(folks_individual_aggregator_remove_individual,
                                boost::bind(asyncCB, _1,
                                            "folks_individual_aggregator_remove_individual",
                                            boost::ref(failed), boost::ref(done)),
                                NULL, NULL);
        while (!done) {
            g_main_context_iteration(NULL, true);
        }
        // Invalid parameters are not reported!
        CPPUNIT_ASSERT(!failed);

        // using simpler macro
        GErrorCXX gerror;
        SYNCEVO_GLIB_CALL_SYNC(NULL,
                               gerror,
                               folks_individual_aggregator_remove_individual,
                               NULL, NULL);
        // Invalid parameters are not reported!
        CPPUNIT_ASSERT(!gerror);
    }

    static void individualSignal(std::ostringstream &out,
                                 const char *action,
                                 int index,
                                 FolksIndividual *individual) {
        out << action << ": " << index << " " <<
            folks_name_details_get_full_name(FOLKS_NAME_DETAILS(individual)) <<
            std::endl;
    }

    static void monitorView(IndividualView &view, std::ostringstream &out) {
        view.m_addedSignal.connect(boost::bind(individualSignal, boost::ref(out), "added", _1, _2));
        view.m_removedSignal.connect(boost::bind(individualSignal, boost::ref(out), "removed", _1, _2));
        view.m_modifiedSignal.connect(boost::bind(individualSignal, boost::ref(out), "modified", _1, _2));
    }

    void fullview() {
        boost::shared_ptr<FullView> view(FullView::create());
        std::ostringstream out;
        monitorView(*view, out);

        // add and remove
        view->addIndividual(m_contactA.get());
        CPPUNIT_ASSERT_EQUAL(std::string("added: 0 Abraham Zzz\n"), out.str());
        CPPUNIT_ASSERT_EQUAL(1, view->size());
        out.str("");
        view->removeIndividual(m_contactA);
        CPPUNIT_ASSERT_EQUAL(std::string("removed: 0 Abraham Zzz\n"), out.str());
        CPPUNIT_ASSERT_EQUAL(0, view->size());
        out.str("");

        // add three, in inverse order
        view->addIndividual(m_contactC.get());
        CPPUNIT_ASSERT_EQUAL(std::string("added: 0 Charly Xxx\n"), out.str());
        CPPUNIT_ASSERT_EQUAL(1, view->size());
        out.str("");
        view->addIndividual(m_contactB.get());
        CPPUNIT_ASSERT_EQUAL(std::string("added: 0 Benjamin Yyy\n"), out.str());
        CPPUNIT_ASSERT_EQUAL(2, view->size());
        out.str("");
        view->addIndividual(m_contactA.get());
        CPPUNIT_ASSERT_EQUAL(std::string("added: 0 Abraham Zzz\n"), out.str());
        CPPUNIT_ASSERT_EQUAL(3, view->size());
        out.str("");

        // change sorting: last,first inverse
        boost::shared_ptr<IndividualCompare> compare(new CompareFormattedName(true, false));
        view->setCompare(compare);
        // Exact sequence of events is not specified. The current
        // implementation prefers to announce changes at the end of
        // the array first.
        CPPUNIT_ASSERT_EQUAL(std::string("removed: 2 Charly Xxx\n"
                                         "removed: 1 Benjamin Yyy\n"
                                         "removed: 0 Abraham Zzz\n"
                                         "added: 0 Abraham Zzz\n"
                                         "added: 0 Benjamin Yyy\n"
                                         "added: 0 Charly Xxx\n"),
                             out.str());
        CPPUNIT_ASSERT_EQUAL(3, view->size());

        // TODO: insert sorted

    }

    void filter() {
        class FilterFullName : public IndividualFilter {
            std::string m_name;
            bool m_negated;
        public:
            FilterFullName(const std::string &name,
                           bool negated = false) :
                m_name(name),
                m_negated(negated)
            {
            }

            virtual bool matches(FolksIndividual *individual) const {
                return (m_name == folks_name_details_get_full_name(FOLKS_NAME_DETAILS(individual)))
                    ^ m_negated;
            }
        };

        // start with full view
        boost::shared_ptr<FullView> view(FullView::create());
        view->addIndividual(m_contactA.get());
        view->addIndividual(m_contactB.get());
        view->addIndividual(m_contactC.get());
        CPPUNIT_ASSERT_EQUAL(3, view->size());
        boost::shared_ptr<IndividualFilter> filterFullName(static_cast<IndividualFilter *>(new FilterFullName("Benjamin Yyy")));
        boost::shared_ptr<FilteredView> filter(FilteredView::create(view,
                                                                    filterFullName));
        std::ostringstream out;
        monitorView(*filter, out);
        filter->start();
        CPPUNIT_ASSERT_EQUAL(std::string("added: 0 Benjamin Yyy"), out.str());
        CPPUNIT_ASSERT_EQUAL(1, filter->size());

        // remove all individuals
        out.str("");
        view->removeIndividual(m_contactA.get());
        CPPUNIT_ASSERT_EQUAL(std::string(""), out.str());
        CPPUNIT_ASSERT_EQUAL(2, view->size());
        CPPUNIT_ASSERT_EQUAL(1, filter->size());
        view->removeIndividual(m_contactB.get());
        CPPUNIT_ASSERT_EQUAL(std::string("removed: 0 Benjamin Yyy"), out.str());
        CPPUNIT_ASSERT_EQUAL(1, view->size());
        CPPUNIT_ASSERT_EQUAL(0, filter->size());
        out.str("");
        view->removeIndividual(m_contactC.get());
        CPPUNIT_ASSERT_EQUAL(std::string(""), out.str());
        CPPUNIT_ASSERT_EQUAL(0, view->size());
        CPPUNIT_ASSERT_EQUAL(0, filter->size());

        // add again
        view->addIndividual(m_contactA.get());
        CPPUNIT_ASSERT_EQUAL(std::string(""), out.str());
        CPPUNIT_ASSERT_EQUAL(1, view->size());
        CPPUNIT_ASSERT_EQUAL(0, filter->size());
        view->addIndividual(m_contactB.get());
        CPPUNIT_ASSERT_EQUAL(std::string("added: 0 Benjamin Yyy"), out.str());
        CPPUNIT_ASSERT_EQUAL(2, view->size());
        CPPUNIT_ASSERT_EQUAL(1, filter->size());
        out.str("");
        view->addIndividual(m_contactC.get());
        CPPUNIT_ASSERT_EQUAL(std::string(""), out.str());
        CPPUNIT_ASSERT_EQUAL(3, view->size());
        CPPUNIT_ASSERT_EQUAL(1, filter->size());

        // TODO: same as above, with inverted filter

        // TODO: same as above, with inverted filter and reversed order

        // TODO: change sorting while we have a filter view open
    }

    void aggregate() {
        // TODO: run with well-defined set of databases and database content
        boost::shared_ptr<IndividualAggregator> aggregator(IndividualAggregator::create());
        boost::shared_ptr<IndividualView> view(aggregator->getMainView());
        std::ostringstream out;
        monitorView(*view, out);
        aggregator->start();
        while (!folks_individual_aggregator_get_is_quiescent(aggregator->getFolks())) {
            g_main_context_iteration(NULL, true);
        }
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(FolksTest);

#endif

SE_END_CXX

