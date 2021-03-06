/*
 * Copyright (C) 2008-2009 Patrick Ohly <patrick.ohly@gmx.de>
 * Copyright (C) 2009 Intel Corporation
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

#include "ActiveSyncSource.h"
#include "ActiveSyncCalendarSource.h"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef ENABLE_UNIT_TESTS
# include <cppunit/extensions/TestFactoryRegistry.h>
# include <cppunit/extensions/HelperMacros.h>
#endif

#include <fstream>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

static std::unique_ptr<SyncSource> createSource(const SyncSourceParams &params)
{
    SourceType sourceType = SyncSource::getSourceType(params.m_nodes);
    bool isMe;

    isMe = sourceType.m_backend == "ActiveSync Address Book";
    if (isMe) {
        return
#ifdef ENABLE_ACTIVESYNC
            std::make_unique<ActiveSyncContactSource>(params)
#else
            RegisterSyncSource::InactiveSource(params)
#endif
            ;
    }

    isMe = sourceType.m_backend == "ActiveSync Events";
    if (isMe) {
        return
#ifdef ENABLE_ACTIVESYNC
            std::make_unique<ActiveSyncCalendarSource>(params, EAS_ITEM_CALENDAR)
#else
            RegisterSyncSource::InactiveSource(params)
#endif
            ;
    }

    isMe = sourceType.m_backend == "ActiveSync Todos";
    if (isMe) {
        return
#ifdef ENABLE_ACTIVESYNC
            std::make_unique<ActiveSyncCalFormatSource>(params, EAS_ITEM_TODO)
#else
            RegisterSyncSource::InactiveSource(params)
#endif
            ;
    }

    isMe = sourceType.m_backend == "ActiveSync Memos";
    if (isMe) {
        return
#ifdef ENABLE_ACTIVESYNC
            std::make_unique<ActiveSyncCalFormatSource>(params, EAS_ITEM_JOURNAL)
#else
            RegisterSyncSource::InactiveSource(params)
#endif
            ;
    }

    return nullptr;
}

static RegisterSyncSource registerMe("ActiveSync",
#ifdef ENABLE_ACTIVESYNC
                                     true,
#else
                                     false,
#endif
                                     createSource,
                                     "ActiveSync Address Book = eas-contacts\n"
                                     "ActiveSync Events = eas-events\n"
                                     "ActiveSync Todos = eas-todos\n"
                                     "ActiveSync Memos = eas-memos",
                                     Values() +
                                     (Aliases("ActiveSync Address Book") + "eas-contacts") +
                                     (Aliases("ActiveSync Events") + "eas-events") +
                                     (Aliases("ActiveSync Todos") + "eas-todos") +
                                     (Aliases("ActiveSync Memos") + "eas-memos"));

#ifdef ENABLE_ACTIVESYNC
#ifdef ENABLE_UNIT_TESTS

class ActiveSyncsTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(ActiveSyncsTest);
    CPPUNIT_TEST(testInstantiate);
    CPPUNIT_TEST_SUITE_END();

protected:
    void testInstantiate() {
        std::unique_ptr<SyncSource> source;
        source = SyncSource::createTestingSource("contacts", "ActiveSync Address Book", true);
        source = SyncSource::createTestingSource("events", "ActiveSync Events", true);
        source = SyncSource::createTestingSource("todos", "ActiveSync Todos", true);
        source = SyncSource::createTestingSource("memos", "ActiveSync Memos", true);
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(ActiveSyncsTest);

#endif // ENABLE_UNIT_TESTS

namespace {
#if 0
}
#endif

/**
 * Takes all existing items in the source and writes them into the file,
 * separated by a blank line. beginSync() with the previous sync key was
 * already called.
 *
 * Used for testing and thus should better not rely on cached information,
 * but ActiveSync doesn't offer an independent "list and/or retrieve all items"
 * operation. Using the cached information implies that we won't find bugs in
 * the handling of that information.
 */
static int DumpItems(ClientTest &client, TestingSyncSource &source, const std::string &file,
                     bool forceBaseReadItem)
{
    ActiveSyncSource &eassource = static_cast<ActiveSyncSource &>(source);
    ofstream out(file.c_str());

    // find all ActiveSync server IDs: in ActiveSyncCalendarSource,
    // each server ID might appear multiple times, once for each
    // recurrence associated with it
    std::set<std::string> easids;
    for (const std::string &luid: eassource.getAllItems()) {
        // slight hack: we know that luids in ActiveSyncSource base
        // class pass through this method unmodified, so no need to
        // avoid it
        StringPair ids = ActiveSyncCalendarSource::splitLUID(luid);
        easids.insert(ids.first);
    }

    for (const std::string &easid: easids) {
        std::string item;
        if (forceBaseReadItem) {
            // This bypasses the more specialized
            // ActiveSyncCalendarSource::readItem(), which helps
            // reveal potential bugs in it. However, it depends on a
            // working Fetch operation in the ActiveSync server, which
            // Google doesn't seem to provide (404 error).
            eassource.ActiveSyncSource::readItem(easid, item);
        } else {
            // Normal readItem() works with Google by using the cached
            // item. However, the source must have done a beginSync()
            // with empty sync key, because otherwise the cache is
            // not guaranteed to be complete.
            eassource.readItem(easid, item);
        }
        out << item << '\n';
        if (!boost::ends_with(item, "\n")) {
            out << '\n';
        }
    }
    return 0;
}

static std::unique_ptr<TestingSyncSource> createEASSource(const ClientTestConfig::createsource_t &create,
                                                          ClientTest &client,
                                                          const std::string &clientID,
                                                          int source, bool isSourceA)
{
    std::unique_ptr<TestingSyncSource> res(create(client, clientID, source, isSourceA));

    // Mangle username: if the base username in the config is account
    // "foo", then source B uses "foo_B", because otherwise it'll end
    // up sharing change tracking with source A.
    if (!isSourceA) {
        ActiveSyncSource *eassource = static_cast<ActiveSyncSource *>(res.get());
        std::string account = eassource->getSyncConfig().getSyncUser().toString();
        account += "_B";
        eassource->getSyncConfig().setSyncUsername(account, true);
    }

    if (res->getDatabaseID().empty()) {
        return res;
    } else {
        // sorry, no database
        SE_LOG_ERROR(NULL, "cannot create EAS datastore for database %s, check config",
                     res->getDatabaseID().c_str());
        return nullptr;
    }
}

// common settings for all kinds of data
static void updateConfigEAS(const RegisterSyncSourceTest */* me */,
                            ClientTestConfig &config,
                            EasItemType type)
{
        // cannot run tests involving a second database:
        // wrap orginal source creation, set default database for
        // database #0 and refuse to return a source for database #1
        config.m_createSourceA = [create=config.m_createSourceA] (ClientTest &client, const std::string &clientID, int source, bool isSourceA) {
            return createEASSource(create, client, clientID, source, isSourceA);
        };
        config.m_createSourceB = [create=config.m_createSourceB] (ClientTest &client, const std::string &clientID, int source, bool isSourceA) {
            return createEASSource(create, client, clientID, source, isSourceA);
        };
        config.m_dump = [type] (ClientTest &client, TestingSyncSource &source, const std::string &file) {
            return DumpItems(client, source, file,
                             type == EAS_ITEM_CONTACT ||
                             // need to read from our cache for Google Calendar,
                             // because it does not support Fetch
                             strcmp(getEnv("CLIENT_TEST_SERVER", ""), "googleeas")
                             );
        };
        config.m_sourceLUIDsAreVolatile = true;
        // TODO: find out how ActiveSync/Exchange handle children without parent;
        // at the moment, the child is stored as if it was a stand-alone event
        // and the RECURRENCE-ID is lost (BMC #22831).
        config.m_linkedItemsRelaxedSemantic = false;
}

static class ActiveSyncContactTest : public RegisterSyncSourceTest {
public:
    ActiveSyncContactTest() :
        RegisterSyncSourceTest("eas_contact", // name of test => Client::Source::eas_contact"
                               "eds_contact"  // name of test cases: inherit from EDS, override below
                               ) {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        // override default eds_contact test config
        config.m_type = "eas-contacts";
        // TODO: provide comprehensive set of vCard 3.0 contacts as they are understood by the ActiveSync library
        // config.testcases = "testcases/eas_contact.vcf";

        updateConfigEAS(this, config, EAS_ITEM_CONTACT);
    }
} ActiveSyncContactTest;

static class ActiveSyncEventTest : public RegisterSyncSourceTest {
public:
    ActiveSyncEventTest() :
        RegisterSyncSourceTest("eas_event", "eds_event")
    {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.m_type = "eas-events";
        updateConfigEAS(this, config, EAS_ITEM_CALENDAR);
    }
} ActiveSyncEventTest;

static class ActiveSyncTodoTest : public RegisterSyncSourceTest {
public:
    ActiveSyncTodoTest() :
        RegisterSyncSourceTest("eas_task", "eds_task")
    {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.m_type = "eas-todos";
        updateConfigEAS(this, config, EAS_ITEM_TODO);
    }
} ActiveSyncTodoTest;

static class ActiveSyncMemoTest : public RegisterSyncSourceTest {
public:
    ActiveSyncMemoTest() :
        RegisterSyncSourceTest("eas_memo", "eds_memo")
    {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.m_type = "eas-memos";
        updateConfigEAS(this, config, EAS_ITEM_JOURNAL);
    }
} ActiveSyncMemoTest;

} // anonymous namespace

#endif // ENABLE_ACTIVESYNC

SE_END_CXX
