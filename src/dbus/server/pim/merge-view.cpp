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

#include "merge-view.h"

#include <syncevo/BoostHelper.h>

SE_BEGIN_CXX

MergeView::MergeView(const boost::shared_ptr<IndividualView> &view,
                     const Searches &searches,
                     const boost::shared_ptr<LocaleFactory> &locale,
                     const boost::shared_ptr<IndividualCompare> &compare) :
    m_view(view),
    m_searches(searches),
    m_locale(locale),
    m_compare(compare)
{
}

void MergeView::init(const boost::shared_ptr<MergeView> &self)
{
    m_self = self;
}

boost::shared_ptr<MergeView> MergeView::create(const boost::shared_ptr<IndividualView> &view,
                                               const Searches &searches,
                                               const boost::shared_ptr<LocaleFactory> &locale,
                                               const boost::shared_ptr<IndividualCompare> &compare)
{
    boost::shared_ptr<MergeView> merge(new MergeView(view, searches, locale, compare));
    merge->init(merge);
    return merge;
}

void MergeView::doStart()
{
    BOOST_FOREACH (const Searches::value_type &search, m_searches) {
        search->m_quiescenceSignal.connect(boost::bind(&MergeView::edsDone,
                                                       m_self,
                                                       std::string(search->getName())));
        search->m_addedSignal.connect(boost::bind(&MergeView::addEDSIndividual,
                                                  m_self,
                                                  _1));
        search->start();
    }
    m_view->m_quiescenceSignal.connect(boost::bind(&MergeView::viewReady,
                                                   m_self));
    m_view->start();
    if (m_view->isQuiescent()) {
        // Switch to view directly.
        viewReady();
    }
}

void MergeView::addEDSIndividual(const FolksIndividualCXX &individual) throw ()
{
    try {
        Entries::auto_type data(new IndividualData);
        data->init(m_compare.get(), m_locale.get(), individual);
        // Binary search to find insertion point.
        Entries::iterator it =
            std::lower_bound(m_entries.begin(),
                             m_entries.end(),
                             *data,
                             IndividualDataCompare(m_compare));
        size_t index = it - m_entries.begin();
        it = m_entries.insert(it, data.release());
        SE_LOG_DEBUG(NULL, NULL, "%s: added at #%ld/%ld", getName(), index, m_entries.size());
        m_addedSignal(index, *it);
    } catch (...) {
        Exception::handle(HANDLE_EXCEPTION_NO_ERROR);
    }
}

void MergeView::edsDone(const std::string &uuid) throw ()
{
    try {
        SE_LOG_DEBUG(NULL, NULL, "%s: %s is done", getName(), uuid.c_str());
        BOOST_FOREACH (const Searches::value_type &search, m_searches) {
            if (!search->isQuiescent()) {
                SE_LOG_DEBUG(NULL, NULL, "%s: still waiting for %s", getName(), search->getName());
                return;
            }
        }
        SE_LOG_DEBUG(NULL, NULL, "%s: all EDS searches done, %s", getName(), m_viewReady ? "folks also done" : "still waiting for folks, send quiescent now");
        if (!m_viewReady) {
            // folks is still busy, this may take a while. Therefore
            // flush current status.
            //
            // TODO (?): it would be good to have a way to signal "done
            // for now, better results coming" to the client. As
            // things stand at the moment, it might conclude that
            // incomplete resuls from EDS is all that there is to show
            // to the user. Not much of a problem, though, if the
            // quality of those results is good.
            m_quiescenceSignal();
        }
    } catch (...) {
        Exception::handle(HANDLE_EXCEPTION_NO_ERROR);
    }
}

static void GetPersonaUIDs(FolksIndividual *individual, std::set<std::string> &uids)
{
    GeeSet *personas = folks_individual_get_personas(individual);
    BOOST_FOREACH (FolksPersona *persona, GeeCollCXX<FolksPersona *>(personas)) {
        // Includes backend, address book, and UID inside address book.
        uids.insert(folks_persona_get_uid(persona));
    }
}

static bool SamePersonas(FolksIndividual *a, FolksIndividual *b)
{
    std::set<std::string> a_uids, b_uids;
    GetPersonaUIDs(a, a_uids);
    GetPersonaUIDs(b, b_uids);
    if (a_uids.size() == b_uids.size()) {
        BOOST_FOREACH (const std::string &uid, a_uids) {
            if (b_uids.find(uid) == b_uids.end()) {
                break;
            }
        }
        return true;
    }
    return false;
}

void MergeView::viewReady() throw ()
{
    try {
        if (!m_viewReady) {
            m_viewReady = true;

            SE_LOG_DEBUG(NULL, NULL, "%s: folks is ready: %d entries from EDS, %d from folks",
                         getName(),
                         (int)m_entries.size(),
                         (int)m_view->size());

            // Change signals which transform the current view into the final one.
            int index;
            for (index = 0; index < m_view->size() && index < (int)m_entries.size(); index++) {
                const IndividualData &oldData = m_entries[index];
                const IndividualData *newData = m_view->getContact(index);
                // Minimize changes if old and new data are
                // identical. Instead of checking all data, assume
                // that if the underlying contacts are identical, then
                // so must be the data.
                if (!SamePersonas(oldData.m_individual, newData->m_individual)) {
                    SE_LOG_DEBUG(NULL, NULL, "%s: entry #%d modified",
                                 getName(),
                                 index);
                    m_modifiedSignal(index, *newData);
                }
            }
            for (; index < m_view->size(); index++) {
                const IndividualData *newData = m_view->getContact(index);
                SE_LOG_DEBUG(NULL, NULL, "%s: entry #%d added",
                             getName(),
                             index);
                m_addedSignal(index, *newData);
            }
            // Index stays the same when removing, because the following
            // entries get shifted.
            int removeAt = index;
            for (; index < (int)m_entries.size(); index++) {
                const IndividualData &oldData = m_entries[index];
                SE_LOG_DEBUG(NULL, NULL, "%s: entry #%d removed",
                             getName(),
                             index);
                m_removedSignal(removeAt, oldData);
            }

            // Free resources which are no longer needed.
            // The expectation is that this will abort loading
            // from EDS.
            try {
                m_searches.clear();
                m_entries.clear();
            } catch (...) {
                Exception::handle(HANDLE_EXCEPTION_NO_ERROR);
            }
            SE_LOG_DEBUG(NULL, NULL, "%s: switched to folks, quiescent", getName());
            m_quiescenceSignal();
        }
    } catch (...) {
        Exception::handle(HANDLE_EXCEPTION_NO_ERROR);
    }
}

SE_END_CXX
