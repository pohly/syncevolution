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

#ifndef INCL_SYNCEVO_DBUS_SERVER_FULL_VIEW
#define INCL_SYNCEVO_DBUS_SERVER_FULL_VIEW

#include "view.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * The view which takes input directly from IndividualAggregator
 * and maintains a sorted set of contacts as result.
 */
class FullView : public IndividualView
{
    FolksIndividualAggregatorCXX m_folks;
    boost::shared_ptr<LocaleFactory> m_locale;
    bool m_isQuiescent;
    boost::weak_ptr<FullView> m_self;
    Timeout m_waitForIdle;
    std::set<FolksIndividualCXX> m_pendingModifications;
    Timeout m_quiescenceDelay;

    /**
     * Sorted vector. Sort order is maintained by this class.
     */
    typedef boost::ptr_vector<IndividualData> Entries_t;
    Entries_t m_entries;

    /**
     * The sort object to be used.
     */
    boost::shared_ptr<IndividualCompare> m_compare;

    FullView(const FolksIndividualAggregatorCXX &folks,
             const boost::shared_ptr<LocaleFactory> &locale);
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

    /**
     * Adds the new individual to m_entries, transfers ownership
     * (data == NULL afterwards).
     */
    void doAddIndividual(Entries_t::auto_type &data);

 public:
    /**
     * @param folks     the aggregator to use
     */
    static boost::shared_ptr<FullView> create(const FolksIndividualAggregatorCXX &folks,
                                              const boost::shared_ptr<LocaleFactory> &locale);

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
     * FolksIndividualAggregator "is-quiescent" property change slot.
     *
     * It turned out that "quiescence" is only set to true once in
     * FolksIndividualAggregator. The code which watches that signal
     * is still in place, but it will only get invoked once.
     *
     * Therefore the main mechanism for emitting m_quiescenceSignal in
     * FullView is an idle callback which gets invoked each time the
     * daemon has nothing to do, which implies that (at least for now)
     * libfolks has no pending work to do.
     */
    void quiescenceChanged();

    /**
     * Mirrors the FolksIndividualAggregator "is-quiesent" state:
     * false initially, then true for the rest of the run.
     */
    virtual bool isQuiescent() const { return m_isQuiescent; }

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
    virtual const IndividualData *getContact(int index) { return (index >= 0 && (unsigned)index < m_entries.size()) ? &m_entries[index] : NULL; }
};

SE_END_CXX

#endif // INCL_SYNCEVO_DBUS_SERVER_PIM_FULL_VIEW
