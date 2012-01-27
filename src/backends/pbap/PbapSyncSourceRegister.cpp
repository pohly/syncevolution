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

static SyncSource *createSource(const SyncSourceParams &params)
{
    SE_LOG_DEBUG(NULL, NULL, "---> PBAP::createSource()");

    SourceType sourceType = SyncSource::getSourceType(params.m_nodes);
    // The string returned by getSourceType() is always the one
    // registered as main Aliases() below.
    bool isMe = sourceType.m_backend == "PBAP Address Book";

#ifndef ENABLE_PBAP
    // tell SyncEvolution if the user wanted to use a disabled sync source,
    // otherwise let it continue searching
    return isMe ? RegisterSyncSource::InactiveSource : NULL;
#else
    // Also recognize one of the standard types?
    // Not in the PbapSyncSource!
    bool maybeMe = false /* sourceType.m_backend == "addressbook" */;

    if (isMe || maybeMe) {
        return new PbapSyncSource(params);
    }
    return NULL;
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

#ifdef ENABLE_PBAP

// The anonymous namespace ensures that we don't get
// name clashes: although the classes and objects are
// only defined in this file, the methods generated
// for local classes will clash with other methods
// of other classes with the same name if no namespace
// is used.
//
// The code above is not yet inside the anonymous
// name space because it would show up inside
// the CPPUnit unit test names. Use a unique class
// name there.
namespace {
#if 0
}
#endif

static class VCard30Test : public RegisterSyncSourceTest {
public:
    VCard30Test() : RegisterSyncSourceTest("file_contact", "eds_contact") {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.m_type = "file:text/vcard:3.0";
    }
} VCard30Test;

static class ICal20Test : public RegisterSyncSourceTest {
public:
    ICal20Test() : RegisterSyncSourceTest("file_event", "eds_event") {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.m_type = "file:text/calendar:2.0";

        // A sync source which supports linked items (= recurring
        // event with detached exception) is expected to handle
        // inserting the parent or child twice by turning the
        // second operation into an update. The file backend is
        // to dumb for that and therefore fails these tests:
        //
        // Client::Source::file_event::testLinkedItemsInsertParentTwice
        // Client::Source::file_event::testLinkedItemsInsertChildTwice
        //
        // Disable linked item testing to avoid this.
        config.m_sourceKnowsItemSemantic = false;
    }
} ICal20Test;

static class ITodo20Test : public RegisterSyncSourceTest {
public:
    ITodo20Test() : RegisterSyncSourceTest("file_task", "eds_task") {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.m_type = "file:text/calendar:2.0";
    }
} ITodo20Test;

static class SuperTest : public RegisterSyncSourceTest {
public:
    SuperTest() : RegisterSyncSourceTest("file_calendar+todo", "calendar+todo") {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.m_type = "virtual:text/x-vcalendar";
        config.m_subConfigs = "file_event,file_task";
    }

} superTest;

}

#endif // ENABLE_PBAP

SE_END_CXX
