/*
 * Copyright (C) 2016 Emanoil Kotsev emanoil.kotsev@fincom.at
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
 *
 * $Id: TDEPIMAddressBookSourceRegister.cpp,v 1.5 2016/09/20 12:56:49 emanoil Exp $
 *
 */

#include "TDEPIMAddressBookSource.h"

#include <syncevo/util.h>
#include <syncevo/SyncSource.h>

SE_BEGIN_CXX

static std::unique_ptr<SyncSource> createSource(const SyncSourceParams &params)
{
	SourceType sourceType = SyncSource::getSourceType(params.m_nodes);
	
	bool isMe = sourceType.m_backend == "TDE PIM Address Book";

#ifndef ENABLE_TDEPIMABC
	if (isMe) return RegisterSyncSource::InactiveSource(params);
#else
	if (isMe || sourceType.m_backend == "addressbook" ) {
		if (sourceType.m_format == "" || sourceType.m_format == "text/vcard") {
			return std::make_unique<TDEPIMAddressBookSource>(TDEPIM_CONTACT_V30, params);
			}
		else if (sourceType.m_format == "text/x-vcard") {
			return std::make_unique<TDEPIMAddressBookSource>(TDEPIM_CONTACT_V21, params);
			}
		else return nullptr;
	}
#endif
	return nullptr;
}

static class RegisterTDEPIMAddressBokSyncSource : public RegisterSyncSource
{
public:
    RegisterTDEPIMAddressBokSyncSource() :
	RegisterSyncSource ("TDE PIM Address Book/Contacts",
#ifdef ENABLE_TDEPIMABC
                                     true,
#else
                                     false,
#endif
                                     createSource,
                                     "TDE PIM Address Book = TDE PIM Contacts = tdepim-contacts\n"
                                     "   vCard 2.1 = text/x-vcard\n"
                                     "   vCard 3.0 (default) = text/vcard\n"
                                     "   The later is the internal format of TDE PIM and preferred with\n"
                                     "   servers that support it.",
                                     Values() +
                                     (Aliases("TDE PIM Address Book") + "TDE PIM Contacts" + "tdepim-contacts")
			)
    {
        // configure and register our own property;
        // do this regardless whether the backend is enabled,
        // so that config migration always includes this property
/*        WebDAVCredentialsOkay().setHidden(true);
        SyncConfig::getRegistry().push_back(&WebDAVCredentialsOkay());
*/
    }
} registerMe;

#ifdef ENABLE_TDEPIMABC
#ifdef ENABLE_UNIT_TESTS

class TDEAddressBookTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(TDEAddressBookTest);
    CPPUNIT_TEST(testInstantiate);
    CPPUNIT_TEST_SUITE_END();

protected:
    static string addItem(std::shared_ptr<TestingSyncSource> source,
                          string &data) {
        SyncSourceRaw::InsertItemResult res = source->insertItemRaw("", data);
        return res.m_luid;
    }

    void testInstantiate() {
        std::unique_ptr<SyncSource> source;
        // source = SyncSource::createTestingSource("addressbook", "addressbook", true);
        // source = SyncSource::createTestingSource("addressbook", "contacts", true);
        source = SyncSource::createTestingSource("addressbook", "tdepim-contacts", true);
        source = SyncSource::createTestingSource("addressbook", "TDE Contacts", true);
 //       source = SyncSource::createTestingSource("addressbook", "TDE Address Book:text/x-vcard", true);
        source = SyncSource::createTestingSource("addressbook", "TDE Address Book:text/vcard", true);
    }

    // TODO: support default databases

    // void testOpenDefaultAddressBook() {
    //     std::shared_ptr<TestingSyncSource> source;
    //     source = (TestingSyncSource *)SyncSource::createTestingSource("contacts", "kde-contacts", true, nullptr);
    //     CPPUNIT_ASSERT_NO_THROW(source->open());
    // }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(TDEAddressBookTest);

#endif // ENABLE_UNIT_TESTS

namespace {
#if 0
}
#endif

static class vCard30Test : public RegisterSyncSourceTest {
public:
    vCard30Test() : RegisterSyncSourceTest("tdepim_contact", "eds_contact") {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.m_type = "tdepim-contacts";
    }
} vCard30Test;

}

#endif //ENABLE_TDEPIMABC

SE_END_CXX
