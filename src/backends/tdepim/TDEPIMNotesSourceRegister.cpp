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
 *
 * $Id: TDEPIMNotesSourceRegister.cpp,v 1.6 2016/09/20 12:56:49 emanoil Exp $
 *
 */


#include "TDEPIMNotesSource.h"
// #include "test.h"

#include "TDEPIMSyncSource.h"

#include <syncevo/util.h>
#include <syncevo/SyncSource.h>

SE_BEGIN_CXX

static std::unique_ptr<SyncSource> createSource ( const SyncSourceParams &params )
{

	SourceType sourceType = SyncSource::getSourceType(params.m_nodes);

// 	SE_LOG_DEBUG("createSource() c1", "Requested Source format %s", sourceType.m_format.c_str());
// 	SE_LOG_DEBUG("createSource() c2", "Requested backend type  %s", sourceType.m_backend.c_str() );

	bool isMe = sourceType.m_backend == "TDE PIM Notes";
#ifndef ENABLE_TDEPIMNOTES
	if (isMe) return RegisterSyncSource::InactiveSource(params);
#else
	if (isMe) {
// 		SE_LOG_DEBUG("createSource() c3", "Calendar Source format %s", sourceType.m_format.c_str());
		if ( sourceType.m_format == "" || sourceType.m_format == "text/plain" ) 
			return std::make_unique<TDEPIMNotesSource>( params );
		else  return NULL;
	}
#endif

// 	SE_LOG_DEBUG("createSource() c6", "Calendar Source matching the format %s not found", sourceType.m_format.c_str() );
	return NULL;
}

static class RegisterTDEPIMNotesSyncSource : public RegisterSyncSource
{
public:
    RegisterTDEPIMNotesSyncSource() :
        RegisterSyncSource("TDE PIM Notes",
#ifdef ENABLE_TDEPIMNOTES
                                       true,
#else
                                       false,
#endif
                                       createSource,
                                       "TDE PIM Notes = note = notes = tdepim-notes\n"
                                       "   plain text in UTF-8 (default) = text/plain\n",
                                       Values() +
                                       ( Aliases ( "TDE PIM Notes" ) + "note" + "notes" + "tdepim-notes" ) )
    {
        // configure and register our own property;
        // do this regardless whether the backend is enabled,
        // so that config migration always includes this property
/*        WebDAVCredentialsOkay().setHidden(true);
        SyncConfig::getRegistry().push_back(&WebDAVCredentialsOkay());
*/
    }
} registerMe;

// TODO finish unit tests
#ifdef ENABLE_TDEPIMNOTES
#ifdef ENABLE_UNIT_TESTS

class TDENotesTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(TDENotesTest);
    CPPUNIT_TEST(testInstantiate);

    // There is no default database in Akonadi:
    // CPPUNIT_TEST(testOpenDefaultCalendar);
    // CPPUNIT_TEST(testOpenDefaultTodo);
    // CPPUNIT_TEST(testOpenDefaultMemo);

    // Besides, don't enable tests which depend on running Akonadi,
    // because that would cause "client-test SyncEvolution" unless
    // Akonadi was started first:
    // CPPUNIT_TEST(testTimezones);

    CPPUNIT_TEST_SUITE_END();

protected:
    static string addItem(std::shared_ptr<TestingSyncSource> source,
                          string &data) {
        SyncSourceRaw::InsertItemResult res = source->insertItemRaw("", data);
        return res.m_luid;
    }

    void testInstantiate() {
        std::unique_ptr<SyncSource> source;

        // source = SyncSource::createTestingSource("memos", "memos", true);
        source = SyncSource::createTestingSource("memos", "tdepim-notes", true);
        source = SyncSource::createTestingSource("memos", "TDE PIM Notes:text/plain", true);
    }

    // TODO: support default databases

    // void testOpenDefaultMemo() {
    //     std::shared_ptr<TestingSyncSource> source;
    //     source = (TestingSyncSource *)SyncSource::createTestingSource("memos", "tdepim-memos", true, NULL);
    //     CPPUNIT_ASSERT_NO_THROW(source->open());
    // }

    void testTimezones() {
        const char *prefix = getenv("CLIENT_TEST_EVOLUTION_PREFIX");
        if (!prefix) {
            prefix = "SyncEvolution_Test_";
        }

        std::shared_ptr<TestingSyncSource> source;
        source = (TestingSyncSource *)SyncSource::createTestingSource("eds_event", "tdepim-notes", true, prefix);
        CPPUNIT_ASSERT_NO_THROW(source->open());

        string newyork = 
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTIMEZONE\n"
            "TZID:America/New_York\n"
            "BEGIN:STANDARD\n"
            "TZOFFSETFROM:-0400\n"
            "TZOFFSETTO:-0500\n"
            "TZNAME:EST\n"
            "DTSTART:19701025T020000\n"
            "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=10\n"
            "END:STANDARD\n"
            "BEGIN:DAYLIGHT\n"
            "TZOFFSETFROM:-0500\n"
            "TZOFFSETTO:-0400\n"
            "TZNAME:EDT\n"
            "DTSTART:19700405T020000\n"
            "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=1SU;BYMONTH=4\n"
            "END:DAYLIGHT\n"
            "END:VTIMEZONE\n"
            "BEGIN:VEVENT\n"
            "UID:artificial\n"
            "DTSTAMP:20060416T205224Z\n"
            "DTSTART;TZID=America/New_York:20060406T140000\n"
            "DTEND;TZID=America/New_York:20060406T143000\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:2\n"
            "SUMMARY:timezone New York with custom definition\n"
            "DESCRIPTION:timezone New York with custom definition\n"
            "CLASS:PUBLIC\n"
            "CREATED:20060416T205301Z\n"
            "LAST-MODIFIED:20060416T205301Z\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";

        string luid;
        CPPUNIT_ASSERT_NO_THROW(luid = addItem(source, newyork));

        string newyork_suffix = newyork;
        boost::replace_first(newyork_suffix,
                             "UID:artificial",
                             "UID:artificial-2");
        boost::replace_all(newyork_suffix,
                           "TZID:America/New_York",
                           "TZID://FOOBAR/America/New_York-SUFFIX");
        CPPUNIT_ASSERT_NO_THROW(luid = addItem(source, newyork_suffix));

        
        string notimezone = 
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VEVENT\n"
            "UID:artificial-3\n"
            "DTSTAMP:20060416T205224Z\n"
            "DTSTART;TZID=America/New_York:20060406T140000\n"
            "DTEND;TZID=America/New_York:20060406T143000\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:2\n"
            "SUMMARY:timezone New York without custom definition\n"
            "DESCRIPTION:timezone New York without custom definition\n"
            "CLASS:PUBLIC\n"
            "CREATED:20060416T205301Z\n"
            "LAST-MODIFIED:20060416T205301Z\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        CPPUNIT_ASSERT_NO_THROW(luid = addItem(source, notimezone));

        // fake VTIMEZONE where daylight saving starts on first Sunday in March
        string fake_march = 
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTIMEZONE\n"
            "TZID:FAKE\n"
            "BEGIN:STANDARD\n"
            "TZOFFSETFROM:-0400\n"
            "TZOFFSETTO:-0500\n"
            "TZNAME:EST MARCH\n"
            "DTSTART:19701025T020000\n"
            "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=10\n"
            "END:STANDARD\n"
            "BEGIN:DAYLIGHT\n"
            "TZOFFSETFROM:-0500\n"
            "TZOFFSETTO:-0400\n"
            "TZNAME:EDT\n"
            "DTSTART:19700405T020000\n"
            "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=1SU;BYMONTH=3\n"
            "END:DAYLIGHT\n"
            "END:VTIMEZONE\n"
            "BEGIN:VEVENT\n"
            "UID:artificial-4\n"
            "DTSTAMP:20060416T205224Z\n"
            "DTSTART;TZID=FAKE:20060406T140000\n"
            "DTEND;TZID=FAKE:20060406T143000\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:2\n"
            "SUMMARY:fake timezone with daylight starting in March\n"
            "CLASS:PUBLIC\n"
            "CREATED:20060416T205301Z\n"
            "LAST-MODIFIED:20060416T205301Z\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        CPPUNIT_ASSERT_NO_THROW(luid = addItem(source, fake_march));

        string fake_may = fake_march;
        boost::replace_first(fake_may,
                             "UID:artificial-4",
                             "UID:artificial-5");
        boost::replace_first(fake_may,
                             "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=1SU;BYMONTH=3",
                             "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=1SU;BYMONTH=5");
        boost::replace_first(fake_may,
                             "starting in March",
                             "starting in May");
        boost::replace_first(fake_may,
                             "TZNAME:EST MARCH",
                             "TZNAME:EST MAY");
        CPPUNIT_ASSERT_NO_THROW(luid = addItem(source, fake_may));

        // insert again, shouldn't re-add timezone
        CPPUNIT_ASSERT_NO_THROW(luid = addItem(source, fake_may));
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(TDENotesTest);

#endif // ENABLE_UNIT_TESTS

namespace {
#if 0
}
#endif



static class MemoTest : public RegisterSyncSourceTest {
public:
    MemoTest() : RegisterSyncSourceTest("tdepim_notes", "eds_memo") {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.m_type = "TDE PIM Notes"; // use an alias here to test that
    }
} memoTest;

}
#endif
SE_END_CXX
