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

#include "EvolutionContactSource.h"
#include "test.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

static std::unique_ptr<SyncSource> createSource(const SyncSourceParams &params)
{
    SourceType sourceType = SyncSource::getSourceType(params.m_nodes);
    bool isMe = sourceType.m_backend == "Evolution Address Book";
    bool maybeMe = sourceType.m_backend == "addressbook";
#ifdef ENABLE_EBOOK
    const bool enabled = true;
#endif

    EDSAbiWrapperInit();

    if (isMe || maybeMe) {
        if (sourceType.m_format == "text/x-vcard") {
            return
#ifdef ENABLE_EBOOK
                enabled ? std::make_unique<EvolutionContactSource>(params, EVC_FORMAT_VCARD_21) :
#endif
                isMe ? RegisterSyncSource::InactiveSource(params) : nullptr;
        } else if (sourceType.m_format == "" || sourceType.m_format == "text/vcard") {
            return
#ifdef ENABLE_EBOOK
                enabled ? std::make_unique<EvolutionContactSource>(params, EVC_FORMAT_VCARD_30) :
#endif
                isMe ? RegisterSyncSource::InactiveSource(params) : nullptr;
        }
    }
    return nullptr;
}

static RegisterSyncSource registerMe("Evolution Address Book",
#ifdef ENABLE_EBOOK
                                     true,
#else
                                     false,
#endif
                                     createSource,
                                     "Evolution Address Book = Evolution Contacts = addressbook = contacts = evolution-contacts\n"
                                     "   vCard 2.1 = text/x-vcard\n"
                                     "   vCard 3.0 (default) = text/vcard\n"
                                     "   The later is the internal format of Evolution and preferred with\n"
                                     "   servers that support it.",
                                     Values() +
                                     (Aliases("Evolution Address Book") + "Evolution Contacts" + "evolution-contacts"));

#ifdef ENABLE_EBOOK
#ifdef ENABLE_UNIT_TESTS

class EvolutionContactTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(EvolutionContactTest);
    CPPUNIT_TEST(testInstantiate);
    CPPUNIT_TEST(testImport);
    CPPUNIT_TEST_SUITE_END();

protected:
    void testInstantiate() {
        std::unique_ptr<SyncSource> source;
        source = SyncSource::createTestingSource("addressbook", "addressbook", true);
        source = SyncSource::createTestingSource("addressbook", "contacts", true);
        source = SyncSource::createTestingSource("addressbook", "evolution-contacts", true);
        source = SyncSource::createTestingSource("addressbook", "Evolution Contacts", true);
        source = SyncSource::createTestingSource("addressbook", "Evolution Address Book:text/x-vcard", true);
        source = SyncSource::createTestingSource("addressbook", "Evolution Address Book:text/vcard", true);
    }

    /**
     * Tests parsing of contacts as they might be send by certain servers.
     * This complements the actual testing with real servers and might cover
     * cases not occurring with servers that are actively tested against.
     */
    void testImport() {
        // this only tests that we can instantiate something under the type "addressbook";
        auto source21 = SyncSource::createTestingSource("evolutioncontactsource21", "evolution-contacts:text/x-vcard", true);
        auto source30 = SyncSource::createTestingSource("evolutioncontactsource30", "Evolution Address Book:text/vcard", true);
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(EvolutionContactTest);
#endif // ENABLE_UNIT_TESTS

namespace {
#if 0
}
#endif

static class VCard30Test : public RegisterSyncSourceTest {
public:
    VCard30Test() : RegisterSyncSourceTest("eds_contact", "eds_contact") {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.m_type = "evolution-contacts:text/vcard";
        config.m_update = config.m_genericUpdate;
        // this property gets re-added by EDS and thus cannot be removed
	config.m_essentialProperties.insert("X-EVOLUTION-FILE-AS");
    }
} vCard30Test;

}

#endif // ENABLE_EBOOK


SE_END_CXX
