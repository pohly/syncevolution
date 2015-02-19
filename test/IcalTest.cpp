/*
 * Copyright (C) 2005-2009 Patrick Ohly <patrick.ohly@gmx.de>
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

#include "config.h"

#ifdef ENABLE_ICAL
#include "test.h"
#include <syncevo/eds_abi_wrapper.h>
#include <syncevo/icalstrdup.h>
#include <syncevo/SmartPtr.h>
#include <pcrecpp.h>

SE_BEGIN_CXX

class IcalTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(IcalTest);
    CPPUNIT_TEST(testTimezone);
    CPPUNIT_TEST_SUITE_END();

protected:
    /**
     * Ignore exact day in DTSTART because icaltz-util.c uses the
     * transition day of the *current* year, instead of the one from
     * the (arbitrary) 1970 year. The value is wrong either way,
     * so fixing that bug is not important.
     */
    void patchDTSTART(std::string &vtimezone)
    {
        static const pcrecpp::RE re("(DTSTART:1970..)..");
        re.GlobalReplace("\\1XX", &vtimezone);
    }

    /**
     * Ensures that we get VTIMEZONE with RRULE from libical.
     *
     * This only works with libical 1.0 if we successfully
     * pick up our icaltimezone_get_component() or
     * libical uses our icaltzutil_fetch_timezone().
     *
     * This test uses the function lookup via eds_abi_wrapper.h if that
     * was enabled, otherwise goes via the static or dynamic linker.
     *
     * It only passes if the given timezone has not been loaded by
     * libical internally yet, because we cannot influence that. Only
     * direct calls to icaltimezone_get_component() as done by
     * libsynthesis are caught. This means that "Europe/Paris" must
     * not be used by, for example, test data used in
     * Client::Source::eds_event.
     */
    void testTimezone()
    {
        icaltimezone *zone = icaltimezone_get_builtin_timezone("Europe/Paris");
        CPPUNIT_ASSERT(zone);
        icalcomponent *comp = icaltimezone_get_component(zone);
        CPPUNIT_ASSERT(comp);
        SyncEvo::eptr<char> str(ical_strdup(icalcomponent_as_ical_string(comp)));
        CPPUNIT_ASSERT(str);
        // 2014 version of the VTIMEZONE
        std::string expected =
            "BEGIN:VTIMEZONE\r\n"
            "TZID:/freeassociation.sourceforge.net/Tzfile/Europe/Paris\r\n"
            "X-LIC-LOCATION:Europe/Paris\r\n"
            "BEGIN:STANDARD\r\n"
            "TZNAME:CET\r\n"
            "DTSTART:19701026T030000\r\n"
            "RRULE:FREQ=YEARLY;BYDAY=-1SU;BYMONTH=10\r\n"
            "TZOFFSETFROM:+0200\r\n"
            "TZOFFSETTO:+0100\r\n"
            "END:STANDARD\r\n"
            "BEGIN:DAYLIGHT\r\n"
            "TZNAME:CEST\r\n"
            "DTSTART:19700330T020000\r\n"
            "RRULE:FREQ=YEARLY;BYDAY=-1SU;BYMONTH=3\r\n"
            "TZOFFSETFROM:+0100\r\n"
            "TZOFFSETTO:+0200\r\n"
            "END:DAYLIGHT\r\n"
            "END:VTIMEZONE\r\n"
        ;
        patchDTSTART(expected);
        std::string actual(str);
        patchDTSTART(actual);
        // We are very specific here. This'll work until we change our
        // code or the zone data from Europe/Paris changes (not likely).
        CPPUNIT_ASSERT_EQUAL(expected, actual);
    }
};

CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(IcalTest, "SyncEvolution" );

SE_END_CXX

#endif // ENABLE_ICAL
