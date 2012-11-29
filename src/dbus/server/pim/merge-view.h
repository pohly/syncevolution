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
 * Combines results from multiple independent views ("unified address
 * book light") until the main view is quiescent. Then this view
 * switches over to mirroring the main view. When switching, it tries
 * to minimize change signals.
 *
 * The independent views don't have to do their own sorting and don't
 * need store individuals.
 */

#ifndef INCL_SYNCEVO_DBUS_SERVER_PIM_MERGE_VIEW
#define INCL_SYNCEVO_DBUS_SERVER_PIM_MERGE_VIEW

#include "view.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

class MergeView : public IndividualView
{
 public:
    typedef std::vector< boost::shared_ptr<StreamingView> > Searches;

 private:
    boost::weak_ptr<MergeView> m_self;
    boost::shared_ptr<IndividualView> m_view;
    Searches m_searches;
    boost::shared_ptr<LocaleFactory> m_locale;
    boost::shared_ptr<IndividualCompare> m_compare;

    /**
     * As soon as this is true, m_entries becomes irrelevant and
     * MergeView becomes a simple proxy for m_view.
     */
    Bool m_viewReady;

    /**
     * Sorted entries from the simple views.
     */
    typedef boost::ptr_vector<IndividualData> Entries;
    Entries m_entries;

    MergeView(const boost::shared_ptr<IndividualView> &view,
              const Searches &searches,
              const boost::shared_ptr<LocaleFactory> &locale,
              const boost::shared_ptr<IndividualCompare> &compare);
    void init(const boost::shared_ptr<MergeView> &self);

    void addEDSIndividual(const FolksIndividualCXX &individual) throw ();
    void edsDone(const std::string &uuid) throw ();
    void viewReady() throw ();

 public:
    static boost::shared_ptr<MergeView> create(const boost::shared_ptr<IndividualView> &view,
                                               const Searches &searches,
                                               const boost::shared_ptr<LocaleFactory> &locale,
                                               const boost::shared_ptr<IndividualCompare> &compare);

    virtual bool isQuiescent() const { return m_view->isQuiescent(); }
    virtual int size() const { return m_viewReady ? m_view->size() : m_entries.size(); }
    virtual const IndividualData *getContact(int index) {
        return m_viewReady ? m_view->getContact(index) :
            (index >= 0 && (size_t)index < m_entries.size()) ? &m_entries[index] :
            NULL;
    }

 protected:
    virtual void doStart();
};

SE_END_CXX

#endif // INCL_SYNCEVO_DBUS_SERVER_PIM_MERGE_VIEW
