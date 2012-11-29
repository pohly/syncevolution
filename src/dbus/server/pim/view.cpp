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

#include "view.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

void View::start()
{
    SE_LOG_DEBUG(NULL, NULL, "%s: start() %s",
                 getName(),
                 m_started ? "already done" : "doing it now");
    if (!m_started) {
        m_started = true;
        doStart();
    }
}

bool View::isRunning() const
{
    return m_started;
}

void IndividualView::findContact(const std::string &id, int hint, int &index, FolksIndividualCXX &individual)
{
    int i;
    int count = size();
    // Start searching at the hint.
    for (i = hint; i < count; i++) {
        individual = getContact(i)->m_individual;
        if (id == folks_individual_get_id(individual.get())) {
            index = i;
            return;
        }
    }
    // Finish search before the hint.
    for (i = 0; i < hint; i++) {
        individual = getContact(i)->m_individual;
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

SE_END_CXX

