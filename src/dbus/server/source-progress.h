/*
 * Copyright (C) 2011 Intel Corporation
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

#ifndef SOURCE_PROGRESS_H
#define SOURCE_PROGRESS_H

#include "gdbus-cxx-bridge.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/** Part of the D-Bus API, see GetProgress() and ProgressChanged. */
struct APISourceProgress
{
    APISourceProgress() :
        m_phase(""),
        m_prepareCount(-1), m_prepareTotal(-1),
        m_sendCount(-1), m_sendTotal(-1),
        m_receiveCount(-1), m_receiveTotal(-1)
    {}

    std::string m_phase;
    int32_t m_prepareCount, m_prepareTotal;
    int32_t m_sendCount, m_sendTotal;
    int32_t m_receiveCount, m_receiveTotal;
};

/** Used internally. */
struct SourceProgress : public APISourceProgress
{
    SourceProgress() :
        m_added(0), m_updated(0), m_deleted(0)
    {}

    // Statistics from actual SyncSource operations. May differ
    // from item operations processed and counted by the sync engine.
    int32_t m_added, m_updated, m_deleted;
};

SE_END_CXX
namespace GDBusCXX {
using namespace SyncEvo;
template<> struct dbus_traits<APISourceProgress> :
    public dbus_struct_traits<APISourceProgress,
                              dbus_member<APISourceProgress, std::string, &APISourceProgress::m_phase,
                              dbus_member<APISourceProgress, int32_t, &APISourceProgress::m_prepareCount,
                              dbus_member<APISourceProgress, int32_t, &APISourceProgress::m_prepareTotal,
                              dbus_member<APISourceProgress, int32_t, &APISourceProgress::m_sendCount,
                              dbus_member<APISourceProgress, int32_t, &APISourceProgress::m_sendTotal,
                              dbus_member<APISourceProgress, int32_t, &APISourceProgress::m_receiveCount,
                              dbus_member_single<APISourceProgress, int32_t, &APISourceProgress::m_receiveTotal> > > > > > > >
{};

template<> struct dbus_traits<SourceProgress> :
    public dbus_struct_traits<SourceProgress,
                              dbus_base<SourceProgress, APISourceProgress,
                              dbus_member<SourceProgress, int32_t, &SourceProgress::m_added,
                              dbus_member<SourceProgress, int32_t, &SourceProgress::m_updated,
                              dbus_member_single<SourceProgress, int32_t, &SourceProgress::m_deleted> > > > >
{};
}

#endif // SOURCE_PROGRESS_H
