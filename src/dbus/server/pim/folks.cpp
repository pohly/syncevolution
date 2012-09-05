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
#include "folks.h"
#include <boost/bind.hpp>
#include "test.h"


#include <syncevo/declarations.h>
SE_BEGIN_CXX

void IndividualView::readContacts(int start, int count, std::vector<FolksIndividualCXX> &contacts)
{
    contacts.clear();
    if (start < size()) {
        int actualCount = size() - start;
        if (actualCount > count) {
            actualCount = count;
        }
        contacts.reserve(actualCount);
        for (int i = start; i < start + actualCount; i++) {
            contacts.push_back(getContact(i));
        }
    }
}

bool IndividualCompare::compare(const Criteria_t &a, const Criteria_t &b) const
{
    Criteria_t::const_iterator ita = a.begin(),
        itb = b.begin();

    while (itb != b.end()) {
        if (ita == a.end()) {
            // a is shorter
            return true;
        }
        int cmp = ita->compare(*itb);
        if (cmp < 0) {
            // String comparison shows that a is less than b.
            return true;
        } else if (cmp > 0) {
            // Is greater, so definitely not less => don't compare
            // rest of the criteria.
            return false;
        } else {
            // Equal, continue comparing.
            ++ita;
            ++itb;
        }
    }

    // a is not less b
    return false;
}

boost::shared_ptr<FullView> FullView::create()
{
    boost::shared_ptr<FullView> view(new FullView);
    view->m_self = view;
    return view;
}

void FullView::addIndividual(FolksIndividual *individual)
{
    // TODO
}
void FullView::removeIndividual(FolksIndividual *individual)
{
    // TODO
}
void FullView::setCompare(const boost::shared_ptr<IndividualCompare> &compare)
{
    // TODO
}

boost::shared_ptr<FilteredView> FilteredView::create(const boost::shared_ptr<IndividualView> &parent,
                                                     const boost::shared_ptr<IndividualFilter> &filter)
{
    boost::shared_ptr<FilteredView> view(new FilteredView);
    view->m_self = view;
    view->m_parent = parent;
    // TODO
    return view;
}
void FilteredView::start()
{
    // TODO
}
void FilteredView::addIndividual(int parentIndex, FolksIndividual *individual)
{
    // TODO
}
void FilteredView::removeIndividual(int parentIndex, FolksIndividual *individual)
{
    // TODO
}
void FilteredView::changeIndividual(int parentIndex, FolksIndividual *individual)
{
    // TODO
}

boost::shared_ptr<IndividualAggregator> IndividualAggregator::create()
{
    boost::shared_ptr<IndividualAggregator> aggregator(new IndividualAggregator);
    aggregator->m_self = aggregator;
    aggregator->m_folks =
        FolksIndividualAggregatorCXX::steal(folks_individual_aggregator_new());
    boost::shared_ptr<FullView> view(FullView::create());
    aggregator->m_view = view;
    aggregator->m_folks.connectSignal<void (FolksIndividualAggregator *,
                                            GeeSet *added,
                                            GeeSet *removed,
                                            gchar *message,
                                            FolksPersona *actor,
                                            FolksGroupDetailsChangeReason)>("individuals-changed",
                                                                            boost::bind(&IndividualAggregator::individualsChanged,
                                                                                        aggregator.get(),
                                                                                        _2, _3, _4));
    return aggregator;
}

void IndividualAggregator::individualsChanged(GeeSet *added,
                                              GeeSet *removed,
                                              gchar *message) throw ()
{
    // TODO
}

/**
 * Generic error callback. There really isn't much that can be done if
 * libfolks fails, except for logging the problem.
 */
static void logResult(const GError *gerror, const char *operation)
{
    if (gerror) {
        SE_LOG_ERROR(NULL, NULL, "%s: %s", operation, gerror->message);
    } else {
        SE_LOG_DEBUG(NULL, NULL, "%s: done", operation);
    }
}

void IndividualAggregator::start()
{
    if (!m_started) {
        m_started = true;
        SYNCEVO_GLIB_CALL_ASYNC(folks_individual_aggregator_prepare,
                                boost::bind(logResult, _1,
                                            "folks_individual_aggregator_prepare"),
                                getFolks());
    }
}

#ifdef ENABLE_UNIT_TESTS

class CompareFormattedName : public IndividualCompare {
    bool m_reversed;
    bool m_firstLast;

public:
    CompareFormattedName(bool reversed = false, bool firstLast = false) :
        m_reversed(reversed),
        m_firstLast(firstLast)
    {
    }

    virtual void createCriteria(FolksIndividual *individual, Criteria_t &criteria) const {
        FolksStructuredName *fn =
            folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(individual));
        if (fn) {
            const char *family = folks_structured_name_get_family_name(fn);
            const char *given = folks_structured_name_get_given_name(fn);
            if (m_firstLast) {
                criteria.push_back(given ? given : "");
                criteria.push_back(family ? family : "");
            } else {
                criteria.push_back(family ? family : "");
                criteria.push_back(given ? given : "");
            }
        }
    }

    virtual bool compare(const Criteria_t &a, const Criteria_t &b) const {
        return m_reversed ?
            IndividualCompare::compare(b, a) :
            IndividualCompare::compare(a, b);
    }
};

class FolksTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(FolksTest);
    CPPUNIT_TEST(open);
    CPPUNIT_TEST(asyncError);
    CPPUNIT_TEST(gvalue);
    CPPUNIT_TEST(fullview);
    CPPUNIT_TEST(filter);
    CPPUNIT_TEST(aggregate);
    CPPUNIT_TEST_SUITE_END();

    FolksIndividualCXX m_contactA,
        m_contactB,
        m_contactC;

public:
    void setUp()
    {
        FolksIndividualCXX contact;
        FolksStructuredName *fn;

        contact = FolksIndividualCXX::steal(folks_individual_new(NULL));
        folks_name_details_set_full_name(FOLKS_NAME_DETAILS(contact.get()), "Abraham Zzz");
        fn = folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(contact.get()));
        folks_structured_name_set_family_name(fn, "Zzz");
        folks_structured_name_set_given_name(fn, "Abraham");
        m_contactA = contact;

        contact = FolksIndividualCXX::steal(folks_individual_new(NULL));
        folks_name_details_set_full_name(FOLKS_NAME_DETAILS(contact.get()), "Benjamin Yyy");
        fn = folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(contact.get()));
        folks_structured_name_set_family_name(fn, "Yyy");
        folks_structured_name_set_given_name(fn, "Benjamin");
        m_contactB = contact;

        contact = FolksIndividualCXX::steal(folks_individual_new(NULL));
        folks_name_details_set_full_name(FOLKS_NAME_DETAILS(contact.get()), "Charly Xxx");
        fn = folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(contact.get()));
        folks_structured_name_set_family_name(fn, "Xxx");
        folks_structured_name_set_given_name(fn, "Charly");
        m_contactC = contact;
    }

private:
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

        // using simpler macro
        GErrorCXX gerror;
        SYNCEVO_GLIB_CALL_SYNC(NULL,
                               gerror,
                               folks_individual_aggregator_remove_individual,
                               NULL, NULL);
        // Invalid parameters are not reported!
        CPPUNIT_ASSERT(!gerror);
    }

    static void individualSignal(std::ostringstream &out,
                                 const char *action,
                                 int index,
                                 FolksIndividual *individual) {
        out << action << ": " << index << " " <<
            folks_name_details_get_full_name(FOLKS_NAME_DETAILS(individual)) <<
            std::endl;
    }

    static void monitorView(IndividualView &view, std::ostringstream &out) {
        view.m_addedSignal.connect(boost::bind(individualSignal, boost::ref(out), "added", _1, _2));
        view.m_removedSignal.connect(boost::bind(individualSignal, boost::ref(out), "removed", _1, _2));
        view.m_modifiedSignal.connect(boost::bind(individualSignal, boost::ref(out), "modified", _1, _2));
    }

    void fullview() {
        boost::shared_ptr<FullView> view(FullView::create());
        std::ostringstream out;
        monitorView(*view, out);

        // add and remove
        view->addIndividual(m_contactA.get());
        CPPUNIT_ASSERT_EQUAL(std::string("added: 0 Abraham Zzz\n"), out.str());
        CPPUNIT_ASSERT_EQUAL(1, view->size());
        out.str("");
        view->removeIndividual(m_contactA);
        CPPUNIT_ASSERT_EQUAL(std::string("removed: 0 Abraham Zzz\n"), out.str());
        CPPUNIT_ASSERT_EQUAL(0, view->size());
        out.str("");

        // add three, in inverse order
        view->addIndividual(m_contactC.get());
        CPPUNIT_ASSERT_EQUAL(std::string("added: 0 Charly Xxx\n"), out.str());
        CPPUNIT_ASSERT_EQUAL(1, view->size());
        out.str("");
        view->addIndividual(m_contactB.get());
        CPPUNIT_ASSERT_EQUAL(std::string("added: 0 Benjamin Yyy\n"), out.str());
        CPPUNIT_ASSERT_EQUAL(2, view->size());
        out.str("");
        view->addIndividual(m_contactA.get());
        CPPUNIT_ASSERT_EQUAL(std::string("added: 0 Abraham Zzz\n"), out.str());
        CPPUNIT_ASSERT_EQUAL(3, view->size());
        out.str("");

        // change sorting: last,first inverse
        boost::shared_ptr<IndividualCompare> compare(new CompareFormattedName(true, false));
        view->setCompare(compare);
        // Exact sequence of events is not specified. The current
        // implementation prefers to announce changes at the end of
        // the array first.
        CPPUNIT_ASSERT_EQUAL(std::string("removed: 2 Charly Xxx\n"
                                         "removed: 1 Benjamin Yyy\n"
                                         "removed: 0 Abraham Zzz\n"
                                         "added: 0 Abraham Zzz\n"
                                         "added: 0 Benjamin Yyy\n"
                                         "added: 0 Charly Xxx\n"),
                             out.str());
        CPPUNIT_ASSERT_EQUAL(3, view->size());

        // TODO: insert sorted

    }

    void filter() {
        class FilterFullName : public IndividualFilter {
            std::string m_name;
            bool m_negated;
        public:
            FilterFullName(const std::string &name,
                           bool negated = false) :
                m_name(name),
                m_negated(negated)
            {
            }

            virtual bool matches(FolksIndividual *individual) const {
                return (m_name == folks_name_details_get_full_name(FOLKS_NAME_DETAILS(individual)))
                    ^ m_negated;
            }
        };

        // start with full view
        boost::shared_ptr<FullView> view(FullView::create());
        view->addIndividual(m_contactA.get());
        view->addIndividual(m_contactB.get());
        view->addIndividual(m_contactC.get());
        CPPUNIT_ASSERT_EQUAL(3, view->size());
        boost::shared_ptr<IndividualFilter> filterFullName(static_cast<IndividualFilter *>(new FilterFullName("Benjamin Yyy")));
        boost::shared_ptr<FilteredView> filter(FilteredView::create(view,
                                                                    filterFullName));
        std::ostringstream out;
        monitorView(*filter, out);
        filter->start();
        CPPUNIT_ASSERT_EQUAL(std::string("added: 0 Benjamin Yyy"), out.str());
        CPPUNIT_ASSERT_EQUAL(1, filter->size());

        // remove all individuals
        out.str("");
        view->removeIndividual(m_contactA.get());
        CPPUNIT_ASSERT_EQUAL(std::string(""), out.str());
        CPPUNIT_ASSERT_EQUAL(2, view->size());
        CPPUNIT_ASSERT_EQUAL(1, filter->size());
        view->removeIndividual(m_contactB.get());
        CPPUNIT_ASSERT_EQUAL(std::string("removed: 0 Benjamin Yyy"), out.str());
        CPPUNIT_ASSERT_EQUAL(1, view->size());
        CPPUNIT_ASSERT_EQUAL(0, filter->size());
        out.str("");
        view->removeIndividual(m_contactC.get());
        CPPUNIT_ASSERT_EQUAL(std::string(""), out.str());
        CPPUNIT_ASSERT_EQUAL(0, view->size());
        CPPUNIT_ASSERT_EQUAL(0, filter->size());

        // add again
        view->addIndividual(m_contactA.get());
        CPPUNIT_ASSERT_EQUAL(std::string(""), out.str());
        CPPUNIT_ASSERT_EQUAL(1, view->size());
        CPPUNIT_ASSERT_EQUAL(0, filter->size());
        view->addIndividual(m_contactB.get());
        CPPUNIT_ASSERT_EQUAL(std::string("added: 0 Benjamin Yyy"), out.str());
        CPPUNIT_ASSERT_EQUAL(2, view->size());
        CPPUNIT_ASSERT_EQUAL(1, filter->size());
        out.str("");
        view->addIndividual(m_contactC.get());
        CPPUNIT_ASSERT_EQUAL(std::string(""), out.str());
        CPPUNIT_ASSERT_EQUAL(3, view->size());
        CPPUNIT_ASSERT_EQUAL(1, filter->size());

        // TODO: same as above, with inverted filter

        // TODO: same as above, with inverted filter and reversed order

        // TODO: change sorting while we have a filter view open
    }

    void aggregate() {
        // TODO: run with well-defined set of databases and database content
        boost::shared_ptr<IndividualAggregator> aggregator(IndividualAggregator::create());
        boost::shared_ptr<IndividualView> view(aggregator->getMainView());
        std::ostringstream out;
        monitorView(*view, out);
        aggregator->start();
        while (!folks_individual_aggregator_get_is_quiescent(aggregator->getFolks())) {
            g_main_context_iteration(NULL, true);
        }
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(FolksTest);

#endif

SE_END_CXX

