/*
 * Copyright (C) 2008-2009 Patrick Ohly <patrick.ohly@gmx.de>
 * Copyright (C) 2009 Intel Corporation
 * Copyright (C) 2012 BMW Car IT GmbH. All rights reserved.
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

#include "PbapSyncSource.h"
#include "test.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

static std::unique_ptr<SyncSource> createSource(const SyncSourceParams &params)
{
    SourceType sourceType = SyncSource::getSourceType(params.m_nodes);
    // The string returned by getSourceType() is always the one
    // registered as main Aliases() below.
    bool isMe = sourceType.m_backend == "PBAP Address Book";

#ifndef ENABLE_PBAP
    // tell SyncEvolution if the user wanted to use a disabled sync source,
    // otherwise let it continue searching
    return isMe ? RegisterSyncSource::InactiveSource(params) : nullptr;
#else
    // Also recognize one of the standard types?
    // Not in the PbapSyncSource!
    bool maybeMe = false /* sourceType.m_backend == "addressbook" */;

    if (isMe || maybeMe) {
        return std::make_unique<PbapSyncSource>(params);
    }
    return nullptr;
#endif
}

static RegisterSyncSource registerMe("One-way sync using PBAP",
#ifdef ENABLE_PBAP
                                     true,
#else
                                     false,
#endif
                                     createSource,
                                     "One-way sync using PBAP = pbap\n"
                                     "   Requests phonebook entries using PBAP profile, and thus\n"
                                     "   supporting read-only operations.\n"
                                     "   The BT address is selected via database=obex-bt://<bt-addr>.\n",
                                     Values() +
                                     (Aliases("PBAP Address Book") + "pbap"));

SE_END_CXX
