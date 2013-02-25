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
 * Base classes for reading data, in particular individuals.
 */

#ifndef INCL_SYNCEVO_DBUS_SERVER_PIM_VIEW
#define INCL_SYNCEVO_DBUS_SERVER_PIM_VIEW

#include "folks.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * Accesses data once started. Derived classes report that data
 * differently.
 */
class View
{
    Bool m_started;
    std::string m_name;

 public:
    typedef boost::signals2::signal<void (void)> QuiescenceSignal_t;

    /**
     * Triggered each time the view reaches a quiescence state, meaning
     * that its current content is stable, at least for now.
     */
    QuiescenceSignal_t m_quiescenceSignal;

    /**
     * False when more changes are known to come.
     */
    virtual bool isQuiescent() const = 0;

    /**
     * A name for the view, for debugging.
     */
    const char *getName() const { return m_name.c_str(); }
    void setName(const std::string &name) { m_name = name; }

    /**
     * Start filling the view. Gives the user a chance to connect
     * to the signals first. May be called multiple times.
     */
    void start();

    /**
     * start() was called.
     */
    bool isRunning() const;

 protected:
    /**
     * Start filling the view. Will only be called once by start().
     */
    virtual void doStart() = 0;
};

/**
 * Reports individuals once as they come in, unsorted.
 */
class StreamingView : public View
{
 public:
    typedef boost::signals2::signal<void (const FolksIndividualCXX &)> AddedSignal_t;

    /**
     * A new FolksIndividual was added.
     */
    AddedSignal_t m_addedSignal;
};

/**
 * A view on a sorted list of individuals. Entries are numbered from
 * #0 to #n - 1, where n is the number of entries. Change
 * notifications are based upon those numbers and will be triggered
 * immediately.
 */
class IndividualView : public View
{
 public:
    typedef boost::signals2::signal<void (int, const IndividualData &)> ChangeSignal_t;

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

    /**
     * Replace filter with more specific one (refine = true) or redo
     * search without limitations.
     */
    virtual void replaceFilter(const boost::shared_ptr<IndividualFilter> &individualFilter,
                               bool refine) {
        SE_THROW("adding a search not supported by this view");
    }

    /** current number of entries */
    virtual int size() const = 0;

    typedef std::vector< std::pair<int, FolksIndividualCXX> > Contacts;

    /** read a set of contacts - see org.01.pim.contacts.ViewControl.ReadContacts() */
    virtual void readContacts(const std::vector<std::string> &ids, Contacts &contacts);

    /** returns access to one individual or an empty pointer if outside of the current range */
    virtual const IndividualData *getContact(int index) = 0;

 protected:
    void findContact(const std::string &id, int hint, int &index, FolksIndividualCXX &individual);
};

SE_END_CXX

#endif // INCL_SYNCEVO_DBUS_SERVER_PIM_VIEW
