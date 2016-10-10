
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
 * $Id: TDEPIMCalendarSourceRegister.cpp,v 1.6 2016/09/20 12:56:49 emanoil Exp $
 *
 */


// #include "TDEPIMNotesSource.h"
// #include "test.h"

#include "TDEPIMCalendarSource.h"

#include <syncevo/util.h>
#include <syncevo/SyncSource.h>

SE_BEGIN_CXX

static SyncSource *createSource ( const SyncSourceParams &params )
{
/*
* NOTE: The libkcal vCal (v.1.0) does not work pretty well I had to leave
*       support only for iCal
*/

	SourceType sourceType = SyncSource::getSourceType(params.m_nodes);

// 	SE_LOG_DEBUG("createSource() c1", "Requested Source format %s", sourceType.m_format.c_str());
// 	SE_LOG_DEBUG("createSource() c2", "Requested backend type  %s", sourceType.m_backend.c_str() );

	bool isMe = sourceType.m_backend == "TDE PIM Calendar";
#ifndef ENABLE_TDEPIMCAL
	if (isMe) return RegisterSyncSource::InactiveSource(params);
#else
	if (isMe || sourceType.m_backend == "calendar" ) {
		if ( sourceType.m_format == "" || 
			sourceType.m_format == "text/calendar" /*||
			sourceType.m_format == "text/x-calendar" || 
			sourceType.m_format == "text/x-vcalendar"*/ )
				return new TDEPIMCalendarSource ( TDEPIM_TASKS, params );
		else  return NULL;
	}
#endif
	
	isMe = sourceType.m_backend == "TDE PIM Task List";
#ifndef ENABLE_TDEPIMCAL
	if (isMe) return RegisterSyncSource::InactiveSource(params);
#else
	if (isMe || sourceType.m_backend == "todo") {
		if ( sourceType.m_format == "" || 
			sourceType.m_format == "text/calendar" /*|| 
			sourceType.m_format == "text/x-calendar" ||
			sourceType.m_format == "text/x-vcalendar"*/)
				return new TDEPIMCalendarSource ( TDEPIM_TODO, params );
		else  return NULL;
	}
#endif
	
	isMe = sourceType.m_backend == "TDE PIM Memos";
#ifndef ENABLE_TDEPIMCAL
	if (isMe) return RegisterSyncSource::InactiveSource(params);
#else
	if (isMe || sourceType.m_backend == "memo") {
		if ( sourceType.m_format == "" || 
			sourceType.m_format == "text/calendar" /*||
			sourceType.m_format == "text/x-calendar" || 
			sourceType.m_format == "text/x-vcalendar"*/)
				return new TDEPIMCalendarSource ( TDEPIM_JOURNAL, params );
		else  return NULL;
	}
#endif
// 	SE_LOG_DEBUG("createSource() c6", "Calendar Source matching the format %s not found", sourceType.m_format.c_str() );
	return NULL;
}

static class RegisterTDEPIMCalSyncSource : public RegisterSyncSource
{
public:
    RegisterTDEPIMCalSyncSource() :
        RegisterSyncSource("TDE PIM Calendar/Tasks/Memos",
#ifdef ENABLE_TDEPIMCAL
                                       true,
#else
                                       false,
#endif
                                       createSource,
                                       "TDE PIM Calendar = calendar = events = tdepim-events\n"
                                       "   iCalendar 2.0 (default) = text/calendar\n"
//                                        "   vCalendar 1.0 = text/x-calendar\n"
//                                        "   vCalendar 1.0 = text/x-vcalendar\n"
                                       "TDE PIM Task List = TDE Tasks = todo = tasks = tdepim-tasks\n"
                                       "   iCalendar 2.0 (default) = text/calendar\n"
//                                        "   vCalendar 1.0 = text/x-calendar\n"
//                                        "   vCalendar 1.0 = text/x-vcalendar\n"
                                       "TDE PIM Memos = memo = memos = tdepim-memos\n"
                                       "   iCalendar 2.0 (default) = text/calendar\n"
                                       /*"   vCalendar 1.0 = text/x-vcalendar\n"
                                       "   vCalendar 1.0 = text/x-vcalendar\n"*/,
                                       Values() +
                                       ( Aliases ( "TDE PIM Calendar" ) + "TDE PIM Events" + "calendar" + "events" + "tdepim-calendar" ) +
                                       ( Aliases ( "TDE PIM Task List" ) + "TDE PIM Tasks" + "todo" + "todos" + "tasks" + "tdepim-tasks" ) +
                                       ( Aliases ( "TDE PIM Memos" ) + "TDE PIM Journal" + "memo" + "memos" + "tdepim-memos" ) )
    {
        // configure and register our own property;
        // do this regardless whether the backend is enabled,
        // so that config migration always includes this property
/*        WebDAVCredentialsOkay().setHidden(true);
        SyncConfig::getRegistry().push_back(&WebDAVCredentialsOkay());
*/
    }
} registerMe;

#ifdef ENABLE_TDEPIMCAL
#ifdef ENABLE_UNIT_TESTS

class TDECalendarTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(TDECalendarTest);
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
    static string addItem(boost::shared_ptr<TestingSyncSource> source,
                          string &data) {
        SyncSourceRaw::InsertItemResult res = source->insertItemRaw("", data);
        return res.m_luid;
    }

    void testInstantiate() {
        boost::shared_ptr<SyncSource> source;
        // source.reset(SyncSource::createTestingSource("addressbook", "addressbook", true));
        // source.reset(SyncSource::createTestingSource("addressbook", "contacts", true));
        source.reset(SyncSource::createTestingSource("addressbook", "tdepim-contacts", true));
        source.reset(SyncSource::createTestingSource("addressbook", "TDE Contacts", true));
        source.reset(SyncSource::createTestingSource("addressbook", "TDE Address Book:text/x-vcard", true));
        source.reset(SyncSource::createTestingSource("addressbook", "TDE Address Book:text/vcard", true));


        // source.reset(SyncSource::createTestingSource("calendar", "calendar", true));
        source.reset(SyncSource::createTestingSource("calendar", "tdepim-calendar", true));
        source.reset(SyncSource::createTestingSource("calendar", "TDE Calendar:text/calendar", true));

        // source.reset(SyncSource::createTestingSource("tasks", "tasks", true));
        source.reset(SyncSource::createTestingSource("tasks", "tdepim-tasks", true));
        source.reset(SyncSource::createTestingSource("tasks", "TDE Tasks", true));
        source.reset(SyncSource::createTestingSource("tasks", "TDE Task List:text/calendar", true));

        // source.reset(SyncSource::createTestingSource("memos", "memos", true));
        source.reset(SyncSource::createTestingSource("memos", "tdepim-memos", true));
        source.reset(SyncSource::createTestingSource("memos", "TDE Memos:text/plain", true));
    }

    // TODO: support default databases

    // void testOpenDefaultCalendar() {
    //     boost::shared_ptr<TestingSyncSource> source;
    //     source.reset((TestingSyncSource *)SyncSource::createTestingSource("calendar", "tdepim-calendar", true, NULL));
    //     CPPUNIT_ASSERT_NO_THROW(source->open());
    // }

    // void testOpenDefaultTodo() {
    //     boost::shared_ptr<TestingSyncSource> source;
    //     source.reset((TestingSyncSource *)SyncSource::createTestingSource("tasks", "tdepim-tasks", true, NULL));
    //     CPPUNIT_ASSERT_NO_THROW(source->open());
    // }

    // void testOpenDefaultMemo() {
    //     boost::shared_ptr<TestingSyncSource> source;
    //     source.reset((TestingSyncSource *)SyncSource::createTestingSource("memos", "tdepim-memos", true, NULL));
    //     CPPUNIT_ASSERT_NO_THROW(source->open());
    // }

    void testTimezones() {
        const char *prefix = getenv("CLIENT_TEST_EVOLUTION_PREFIX");
        if (!prefix) {
            prefix = "SyncEvolution_Test_";
        }

        boost::shared_ptr<TestingSyncSource> source;
        source.reset((TestingSyncSource *)SyncSource::createTestingSource("eds_event", "tdepim-calendar", true, prefix));
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

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(TDECalendarTest);

#endif // ENABLE_UNIT_TESTS

namespace {
#if 0
}
#endif

static class iCal20Test : public RegisterSyncSourceTest {
public:
    iCal20Test() : RegisterSyncSourceTest("tdepim_event", "eds_event") {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.m_type = "tdepim-calendar";
        // Looks like iCalendar file resource in Akonadi 1.11.0 does
        // not actually enforce iCalendar 2.0 semantic. It allows
        // updating of events with no UID
        // (testLinkedItemsInsertBothUpdateChildNoIDs)
        // and fails to detect double-adds (testInsertTwice).
        // TODO: this should better be fixed.
        config.m_sourceKnowsItemSemantic = false;
    }
} iCal20Test;

static class iTodo20Test : public RegisterSyncSourceTest {
public:
    iTodo20Test() : RegisterSyncSourceTest("tdepim_task", "eds_task") {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.m_type = "tdepim-tasks";
        // See above.
        config.m_sourceKnowsItemSemantic = false;
    }
} iTodo20Test;

static class MemoTest : public RegisterSyncSourceTest {
public:
    MemoTest() : RegisterSyncSourceTest("tdepim_memo", "eds_memo") {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.m_type = "TDE Memos"; // use an alias here to test that
    }
} memoTest;

}
#endif

SE_END_CXX
