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

#include <config.h>

#include <folks/folks.h>

#include <boost/bind.hpp>

#include <syncevo/GLibSupport.h>
#include <syncevo/GeeSupport.h>
#include <syncevo/GValueSupport.h>
#include "test.h"

SE_GOBJECT_TYPE(FolksIndividualAggregator)
SE_GOBJECT_TYPE(FolksIndividual)
SE_GOBJECT_TYPE(FolksEmailFieldDetails)

#include <syncevo/declarations.h>
SE_BEGIN_CXX

#ifdef ENABLE_UNIT_TESTS

class FolksTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(FolksTest);
    CPPUNIT_TEST(open);
    CPPUNIT_TEST(asyncError);
    CPPUNIT_TEST(gvalue);
    CPPUNIT_TEST_SUITE_END();

    static void asyncCB(const GError *gerror, const char *func, bool &failed, bool &done) {
        done = true;
        if (gerror) {
            failed = true;
            SE_LOG_ERROR(NULL, NULL, "%s: %s", func, gerror->message);
        }
    }

    void open() {
        FolksIndividualAggregatorCXX aggregator(folks_individual_aggregator_new(), false);
        bool done = false, failed = false;
        SYNCEVO_GLIB_CALL_ASYNC(folks_individual_aggregator_prepare,
                                boost::bind(asyncCB, _1,
                                            "folks_individual_aggregator_prepare",
                                            boost::ref(failed), boost::ref(done)),
                                aggregator);

        while (!done) {
            g_main_context_iteration(NULL, true);
        }
        CPPUNIT_ASSERT(!failed);

        while (!folks_individual_aggregator_get_is_quiescent(aggregator)) {
            g_main_context_iteration(NULL, true);
        }

        GeeMap *individuals = folks_individual_aggregator_get_individuals(aggregator);
        SE_LOG_DEBUG(NULL, NULL, "%d individuals", gee_map_get_size(individuals));

        GeeMapIteratorCXX it(gee_map_map_iterator(individuals), false);
        while (gee_map_iterator_next(it)) {
            PlainGStr id(reinterpret_cast<gchar *>(gee_map_iterator_get_key(it)));
            FolksIndividualCXX individual(reinterpret_cast<FolksIndividual *>(gee_map_iterator_get_value(it)),
                                          false);
            GValueStringCXX fullname;
            g_object_get_property(G_OBJECT(individual.get()), "full-name", &fullname);
            SE_LOG_DEBUG(NULL, NULL, "map: id %s name %s = %s",
                         id.get(),
                         fullname.toString().c_str(),
                         fullname.get());
        }

        GeeIteratorCXX it2(gee_iterable_iterator(GEE_ITERABLE(individuals)), false);
        while (gee_iterator_next(it2)) {
            GeeMapEntryCXX entry(reinterpret_cast<GeeMapEntry *>(gee_iterator_get(it2)), false);
            gchar *id(reinterpret_cast<gchar *>(const_cast<gpointer>(gee_map_entry_get_key(entry))));
            FolksIndividual *individual(reinterpret_cast<FolksIndividual *>(const_cast<gpointer>(gee_map_entry_get_value(entry))));
            GValueStringCXX fullname;
            g_object_get_property(G_OBJECT(individual), "full-name", &fullname);
            SE_LOG_DEBUG(NULL, NULL, "iterable: id %s name %s = %s",
                         id,
                         fullname.toString().c_str(),
                         fullname.get());
        }

        typedef GeeCollCXX< GeeMapEntryWrapper<const gchar *, FolksIndividual *> > Coll;
        Coll coll(individuals);
        Coll::const_iterator curr = coll.begin();
        Coll::const_iterator end = coll.end();
        if (curr != end) {
            const gchar *id = (*curr).key();
            FolksIndividual *individual((*curr).value());
            GValueStringCXX fullname;
            g_object_get_property(G_OBJECT(individual), "full-name", &fullname);

            SE_LOG_DEBUG(NULL, NULL, "first: id %s name %s = %s",
                         id,
                         fullname.toString().c_str(),
                         fullname.get());
            ++curr;
        }

        BOOST_FOREACH (Coll::value_type &entry, Coll(individuals)) {
            const gchar *id = entry.key();
            FolksIndividual *individual(entry.value());
            // GValueStringCXX fullname;
            // g_object_get_property(G_OBJECT(individual), "full-name", &fullname);
            const gchar *fullname = folks_name_details_get_full_name(FOLKS_NAME_DETAILS(individual));

            SE_LOG_DEBUG(NULL, NULL, "boost: id %s %s name %s",
                         id,
                         fullname ? "has" : "has no",
                         fullname);

            GeeSet *emails = folks_email_details_get_email_addresses(FOLKS_EMAIL_DETAILS(individual));
            SE_LOG_DEBUG(NULL, NULL, "     %d emails", gee_collection_get_size(GEE_COLLECTION(emails)));
            typedef GeeCollCXX<FolksEmailFieldDetails *> EmailColl;
            BOOST_FOREACH (FolksEmailFieldDetails *email, EmailColl(emails)) {
                SE_LOG_DEBUG(NULL, NULL, "     %s",
                             reinterpret_cast<const gchar *>(folks_abstract_field_details_get_value(FOLKS_ABSTRACT_FIELD_DETAILS(email))));
            }
        }

        aggregator.reset();
    }

    void gvalue() {
        GValueBooleanCXX b(true);
        SE_LOG_DEBUG(NULL, NULL, "GValueBooleanCXX(true) = %s", b.toString().c_str());
        GValueBooleanCXX b2(b);
        CPPUNIT_ASSERT_EQUAL(b.get(), b2.get());
        b2.set(false);
        CPPUNIT_ASSERT_EQUAL(b.get(), (gboolean)!b2.get());
        b2 = b;
        CPPUNIT_ASSERT_EQUAL(b.get(), b2.get());

        GValueStringCXX str("foo bar");
        SE_LOG_DEBUG(NULL, NULL, "GValueStringCXX(\"foo bar\") = %s", str.toString().c_str());
        CPPUNIT_ASSERT(!strcmp(str.get(), "foo bar"));
        GValueStringCXX str2(str);
        CPPUNIT_ASSERT(!strcmp(str.get(), str2.get()));
        CPPUNIT_ASSERT(str.get() != str2.get());
        str2.set("foo");
        CPPUNIT_ASSERT(strcmp(str.get(), str2.get()));
        CPPUNIT_ASSERT(str.get() != str2.get());
        str2 = str;
        CPPUNIT_ASSERT(!strcmp(str.get(), str2.get()));
        CPPUNIT_ASSERT(str.get() != str2.get());
        str2.take(g_strdup("bar"));
        CPPUNIT_ASSERT(strcmp(str.get(), str2.get()));
        CPPUNIT_ASSERT(str.get() != str2.get());
        const char *fixed = "fixed";
        str2.setStatic(fixed);
        CPPUNIT_ASSERT(!strcmp(str2.get(), fixed));
        CPPUNIT_ASSERT(str2.get() == fixed);
    }

    void asyncError() {
        bool done = false, failed = false;
        SYNCEVO_GLIB_CALL_ASYNC(folks_individual_aggregator_remove_individual,
                                boost::bind(asyncCB, _1,
                                            "folks_individual_aggregator_remove_individual",
                                            boost::ref(failed), boost::ref(done)),
                                NULL, NULL);
        while (!done) {
            g_main_context_iteration(NULL, true);
        }
        // Invalid parameters are not reported!
        CPPUNIT_ASSERT(!failed);
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(FolksTest);

#endif

SE_END_CXX

