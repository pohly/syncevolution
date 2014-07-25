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
#include "full-view.h"
#include "filtered-view.h"
#include "individual-traits.h"
#include "persona-details.h"
#include <boost/bind.hpp>
#include "test.h"
#include <syncevo/BoostHelper.h>
#include <syncevo/LogRedirect.h>

#include <boost/ptr_container/ptr_vector.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * Generic error callback. There really isn't much that can be done if
 * libfolks fails, except for logging the problem.
 */
static void logResult(const GError *gerror, const char *operation)
{
    if (gerror) {
        SE_LOG_ERROR(NULL, "%s: %s", operation, gerror->message);
    } else {
        SE_LOG_DEBUG(NULL, "%s: done", operation);
    }
}

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
            SE_LOG_DEBUG(NULL, "criteria: formatted name: %s, %s",
                         family, given);
            if (m_firstLast) {
                criteria.push_back(given ? given : "");
                criteria.push_back(family ? family : "");
            } else {
                criteria.push_back(family ? family : "");
                criteria.push_back(given ? given : "");
            }
        } else {
            SE_LOG_DEBUG(NULL, "criteria: no formatted");
        }
    }

    virtual bool compare(const Criteria_t &a, const Criteria_t &b) const {
        return m_reversed ?
            IndividualCompare::compare(b, a) :
            IndividualCompare::compare(a, b);
    }
};

boost::shared_ptr<IndividualCompare> IndividualCompare::defaultCompare()
{
    boost::shared_ptr<IndividualCompare> compare(new CompareFormattedName);
    return compare;
}

bool IndividualData::init(const IndividualCompare *compare,
                          const LocaleFactory *locale,
                          FolksIndividual *individual)
{
    bool precomputedModified = false;
    m_individual = FolksIndividualCXX(individual, ADD_REF);
    if (compare) {
        m_criteria.clear();
        compare->createCriteria(individual, m_criteria);
    }
    if (locale) {
        precomputedModified = locale->precompute(individual, m_precomputed);
    }
    return precomputedModified;
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

IndividualAggregator::IndividualAggregator(const boost::shared_ptr<LocaleFactory> &locale) :
    m_locale(locale),
    m_databases(gee_hash_set_new(G_TYPE_STRING, (GBoxedCopyFunc) g_strdup, g_free, NULL, NULL, NULL, NULL, NULL, NULL), TRANSFER_REF)
{
}

void IndividualAggregator::init(boost::shared_ptr<IndividualAggregator> &self)
{
    m_self = self;
    m_backendStore =
        FolksBackendStoreCXX::steal(folks_backend_store_dup());

    // Ignore some known harmless messages from folks.
    LogRedirect::addIgnoreError("Error preparing Backend 'ofono'");
    LogRedirect::addIgnoreError("Error preparing Backend 'telepathy'");

    SYNCEVO_GLIB_CALL_ASYNC(folks_backend_store_prepare,
                            boost::bind(&IndividualAggregator::storePrepared,
                                        m_self),
                            m_backendStore);

    m_folks =
        FolksIndividualAggregatorCXX::steal(folks_individual_aggregator_new_with_backend_store(m_backendStore));
}

boost::shared_ptr<IndividualAggregator> IndividualAggregator::create(const boost::shared_ptr<LocaleFactory> &locale)
{
    boost::shared_ptr<IndividualAggregator> aggregator(new IndividualAggregator(locale));
    aggregator->init(aggregator);
    return aggregator;
}

std::string IndividualAggregator::dumpDatabases()
{
    std::string res;

    BOOST_FOREACH (const gchar *tmp, GeeStringCollection(GEE_COLLECTION(m_databases.get()), ADD_REF)) {
        if (!res.empty()) {
            res += ", ";
        }
        res += tmp;
    }
    return res;
}

void IndividualAggregator::storePrepared()
{
    SE_LOG_DEBUG(NULL, "backend store is prepared");

    // Have to hard-code the list of known backends that we don't want.
    SYNCEVO_GLIB_CALL_ASYNC(folks_backend_store_disable_backend,
                            boost::bind(logResult, (const GError *)NULL,
                                        "folks_backend_store_disable_backend"),
                            m_backendStore, "telepathy");
    SYNCEVO_GLIB_CALL_ASYNC(folks_backend_store_disable_backend,
                            boost::bind(logResult, (const GError *)NULL,
                                        "folks_backend_store_disable_backend"),
                            m_backendStore, "tracker");
    SYNCEVO_GLIB_CALL_ASYNC(folks_backend_store_disable_backend,
                            boost::bind(logResult, (const GError *)NULL,
                                        "folks_backend_store_disable_backend"),
                            m_backendStore, "key-file");
    SYNCEVO_GLIB_CALL_ASYNC(folks_backend_store_disable_backend,
                            boost::bind(logResult, (const GError *)NULL,
                                        "folks_backend_store_disable_backend"),
                            m_backendStore, "libsocialweb");
    // Explicitly enable EDS, just to be sure.
    SYNCEVO_GLIB_CALL_ASYNC(folks_backend_store_enable_backend,
                            boost::bind(logResult, (const GError *)NULL,
                                        "folks_backend_store_enable_backend"),
                            m_backendStore, "eds");

    // Start loading backends right away. Assumes that the
    // asynchronous operations above will be done first.
    SYNCEVO_GLIB_CALL_ASYNC(folks_backend_store_load_backends,
                            boost::bind(&IndividualAggregator::backendsLoaded, m_self),
                            m_backendStore);
}

void IndividualAggregator::backendsLoaded()
{
    SE_LOG_DEBUG(NULL, "backend store has loaded backends");
    GeeCollectionCXX coll(folks_backend_store_list_backends(m_backendStore), TRANSFER_REF);
    BOOST_FOREACH (FolksBackend *backend, GeeCollCXX<FolksBackend *>(coll)) {
        SE_LOG_DEBUG(NULL, "folks backend: %s", folks_backend_get_name(backend));
    }
    m_eds =
        FolksBackendCXX::steal(folks_backend_store_dup_backend_by_name(m_backendStore, "eds"));
    if (m_eds) {
        // Remember system store, for writing contacts.
        GeeMap *stores = folks_backend_get_persona_stores(m_eds);
        FolksPersonaStore *systemStore = static_cast<FolksPersonaStore *>(gee_map_get(stores, "system-address-book"));
        m_systemStore = FolksPersonaStoreCXX(systemStore, TRANSFER_REF);

        // Tell the backend which databases we want.
        SE_LOG_DEBUG(NULL, "backends loaded: setting EDS persona stores: [%s]",
                     dumpDatabases().c_str());
        folks_backend_set_persona_stores(m_eds, GEE_SET(m_databases.get()));

        if (m_view) {
            // We were started, prepare aggregator.
            SYNCEVO_GLIB_CALL_ASYNC(folks_individual_aggregator_prepare,
                                    boost::bind(logResult, _1,
                                                "folks_individual_aggregator_prepare"),
                                    getFolks());
        }

        // Execute delayed work.
        m_backendsLoadedSignal();
    } else {
        SE_LOG_ERROR(NULL, "EDS backend not active?!");
    }
}

void IndividualAggregator::setDatabases(std::set<std::string> &databases)
{
    gee_collection_clear(GEE_COLLECTION(m_databases.get()));
    BOOST_FOREACH (const std::string &database, databases) {
        gee_collection_add(GEE_COLLECTION(m_databases.get()), database.c_str());
    }

    if (m_eds) {
        // Backend is loaded, tell it about the change.
        SE_LOG_DEBUG(NULL, "backends already loaded: setting EDS persona stores directly: [%s]",
                     dumpDatabases().c_str());
        folks_backend_set_persona_stores(m_eds, GEE_SET(m_databases.get()));
    } else {
        SE_LOG_DEBUG(NULL, "backends not loaded yet: setting EDS persona stores delayed: [%s]",
                     dumpDatabases().c_str());
    }
}

void IndividualAggregator::setCompare(const boost::shared_ptr<IndividualCompare> &compare)
{
    // Don't start main view. Instead rememeber the compare instance
    // for start().
    m_compare = compare;
    if (m_view) {
        m_view->setCompare(compare);
    }
}

void IndividualAggregator::setLocale(const boost::shared_ptr<LocaleFactory> &locale)
{
    m_locale = locale;

    if (m_view) {
        m_view->setLocale(m_locale);
    }
}


void IndividualAggregator::start()
{
    if (!m_view) {
        m_view = FullView::create(m_folks, m_locale);
        if (m_compare) {
            m_view->setCompare(m_compare);
        }
        if (m_eds) {
            // Backend was loaded and configured, we can prepare the aggregator.
            SYNCEVO_GLIB_CALL_ASYNC(folks_individual_aggregator_prepare,
                                    boost::bind(logResult, _1,
                                                "folks_individual_aggregator_prepare"),
                                    getFolks());
        }
    }
}

bool IndividualAggregator::isRunning() const
{
    return m_view;
}

boost::shared_ptr<FullView> IndividualAggregator::getMainView()
{
    if (!m_view) {
        start();
    }
    return m_view;
}

void IndividualAggregator::addContact(const Result<void (const std::string &)> &result,
                                      const PersonaDetails &details)
{
    // Called directly by D-Bus client. Need fully functional system address book.
    runWithAddressBook(boost::bind(&IndividualAggregator::doAddContact,
                                   this,
                                   result,
                                   details),
                       result.getOnError());
}

void IndividualAggregator::doAddContact(const Result<void (const std::string &)> &result,
                                        const PersonaDetails &details)
{
    SYNCEVO_GLIB_CALL_ASYNC(folks_persona_store_add_persona_from_details,
                            boost::bind(&IndividualAggregator::addContactDone,
                                        this,
                                        _2, _1,
                                        result),
                            m_systemStore,
                            details.get());
}

void IndividualAggregator::addContactDone(const GError *gerror,
                                          FolksPersona *persona,
                                          const Result<void (const std::string &)> &result) throw()
{
    try {
        // Handle result of folks_persona_store_add_persona_from_details().
        if (!persona || gerror) {
            GErrorCXX::throwError(SE_HERE, "add contact", gerror);
        }

        const gchar *uid = folks_persona_get_uid(persona);
        if (uid) {
            gchar *backend, *storeID, *personaID;
            folks_persona_split_uid(uid, &backend, &storeID, &personaID);
            PlainGStr tmp1(backend), tmp2(storeID), tmp3(personaID);
            result.done(personaID);
        } else {
            SE_THROW("new persona has empty UID");
        }
    } catch (...) {
        result.failed();
    }
}

void IndividualAggregator::modifyContact(const Result<void ()> &result,
                                         const std::string &localID,
                                         const PersonaDetails &details)
{
    runWithPersona(boost::bind(&IndividualAggregator::doModifyContact,
                               this,
                               result,
                               _1,
                               details),
                   localID,
                   result.getOnError());
}

void IndividualAggregator::doModifyContact(const Result<void ()> &result,
                                           FolksPersona *persona,
                                           const PersonaDetails &details) throw()
{
    try {
        // Asynchronously modify the persona. This will be turned into
        // EDS updates by folks.
        Details2Persona(result, details, persona);
    } catch (...) {
        result.failed();
    }
}

void IndividualAggregator::removeContact(const Result<void ()> &result,
                                         const std::string &localID)
{
    runWithPersona(boost::bind(&IndividualAggregator::doRemoveContact,
                               this,
                               result,
                               _1),
                   localID,
                   result.getOnError());
}

void IndividualAggregator::doRemoveContact(const Result<void ()> &result,
                                           FolksPersona *persona) throw()
{
    try {
        SYNCEVO_GLIB_CALL_ASYNC(folks_persona_store_remove_persona,
                                boost::bind(&IndividualAggregator::removeContactDone,
                                            this,
                                            _1,
                                            result),
                                m_systemStore,
                                persona);
    } catch (...) {
        result.failed();
    }
}

void IndividualAggregator::removeContactDone(const GError *gerror,
                                             const Result<void ()> &result) throw()
{
    try {
        if (gerror) {
            GErrorCXX::throwError(SE_HERE, "remove contact", gerror);
        }
        result.done();
    } catch (...) {
        result.failed();
    }
}


void IndividualAggregator::runWithAddressBook(const boost::function<void ()> &operation,
                                              const ErrorCb_t &onError) throw()
{
    try {
        if (m_eds) {
            runWithAddressBookHaveEDS(boost::signals2::connection(),
                                      operation,
                                      onError);
        } else {
            // Do it later.
            m_backendsLoadedSignal.connect_extended(boost::bind(&IndividualAggregator::runWithAddressBookHaveEDS,
                                                                this,
                                                                _1,
                                                                operation,
                                                                onError));
        }
    } catch (...) {
        onError();
    }
}

void IndividualAggregator::runWithAddressBookHaveEDS(const boost::signals2::connection &conn,
                                                     const boost::function<void ()> &operation,
                                                     const ErrorCb_t &onError) throw()
{
    try {
        // Called after we obtained EDS backend. Need system store
        // which is prepared.
        m_backendsLoadedSignal.disconnect(conn);
        if (!m_systemStore) {
            SE_THROW("system address book not found");
        }
        if (folks_persona_store_get_is_prepared(m_systemStore)) {
            runWithAddressBookPrepared(NULL,
                                       operation,
                                       onError);
        } else {
            SYNCEVO_GLIB_CALL_ASYNC(folks_persona_store_prepare,
                                    boost::bind(&IndividualAggregator::runWithAddressBookPrepared,
                                                this,
                                                _1,
                                                operation,
                                                onError),
                                    m_systemStore);
        }
    } catch (...) {
        onError();
    }
}

void IndividualAggregator::runWithAddressBookPrepared(const GError *gerror,
                                                      const boost::function<void ()> &operation,
                                                      const ErrorCb_t &onError) throw()
{
    try {
        // Called after EDS system store is prepared, successfully or unsuccessfully.
        if (gerror) {
            GErrorCXX::throwError(SE_HERE, "prepare EDS system address book", gerror);
        }
        operation();
    } catch (...) {
        onError();
    }
}

void IndividualAggregator::runWithPersona(const boost::function<void (FolksPersona *)> &operation,
                                          const std::string &localID,
                                          const ErrorCb_t &onError) throw()
{
    try {
        runWithAddressBook(boost::bind(&IndividualAggregator::doRunWithPersona,
                                       this,
                                       operation,
                                       localID,
                                       onError),
                           onError);
    } catch (...) {
        onError();
    }
}

void IndividualAggregator::doRunWithPersona(const boost::function<void (FolksPersona *)> &operation,
                                            const std::string &localID,
                                            const ErrorCb_t &onError) throw()
{
    try {
        typedef GeeCollCXX< GeeMapEntryWrapper<const gchar *, FolksPersona *> > Coll;
        Coll personas(folks_persona_store_get_personas(m_systemStore), ADD_REF);
        BOOST_FOREACH (const Coll::value_type &entry, personas) {
            // key seems to be <store id>:<persona ID>
            const gchar *key = entry.key();
            const gchar *colon = strchr(key, ':');
            if (colon && localID == colon + 1) {
                operation(entry.value());
                return;
            }
        }
        SE_THROW(StringPrintf("contact with local ID '%s' not found in system address book", localID.c_str()));
    } catch (...) {
        onError();
    }
}

#ifdef ENABLE_UNIT_TESTS

class FolksTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(FolksTest);
    CPPUNIT_TEST(open);
    CPPUNIT_TEST(gvalue);
    CPPUNIT_TEST_SUITE_END();

private:
    static void asyncCB(const GError *gerror, const char *func, bool &failed, bool &done) {
        done = true;
        if (gerror) {
            failed = true;
            SE_LOG_ERROR(NULL, "%s: %s", func, gerror->message);
        }
    }

    void open() {
        FolksIndividualAggregatorCXX aggregator(folks_individual_aggregator_new(), TRANSFER_REF);
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
        typedef GeeCollCXX< GeeMapEntryWrapper<const gchar *, FolksIndividual *> > Coll;
        Coll coll(individuals, ADD_REF);
        SE_LOG_DEBUG(NULL, "%d individuals", gee_map_get_size(individuals));

        GeeMapIteratorCXX it(gee_map_map_iterator(individuals), TRANSFER_REF);
        while (gee_map_iterator_next(it)) {
            PlainGStr id(reinterpret_cast<gchar *>(gee_map_iterator_get_key(it)));
            FolksIndividualCXX individual(reinterpret_cast<FolksIndividual *>(gee_map_iterator_get_value(it)),
                                          TRANSFER_REF);
            GValueStringCXX fullname;
            g_object_get_property(G_OBJECT(individual.get()), "full-name", &fullname);
            SE_LOG_DEBUG(NULL, "map: id %s name %s = %s",
                         id.get(),
                         fullname.toString().c_str(),
                         fullname.get());
        }

        GeeIteratorCXX it2(gee_iterable_iterator(GEE_ITERABLE(individuals)), TRANSFER_REF);
        while (gee_iterator_next(it2)) {
            GeeMapEntryCXX entry(reinterpret_cast<GeeMapEntry *>(gee_iterator_get(it2)), TRANSFER_REF);
            gchar *id(reinterpret_cast<gchar *>(const_cast<gpointer>(gee_map_entry_get_key(entry))));
            FolksIndividual *individual(reinterpret_cast<FolksIndividual *>(const_cast<gpointer>(gee_map_entry_get_value(entry))));
            GValueStringCXX fullname;
            g_object_get_property(G_OBJECT(individual), "full-name", &fullname);
            SE_LOG_DEBUG(NULL, "iterable: id %s name %s = %s",
                         id,
                         fullname.toString().c_str(),
                         fullname.get());
        }

        Coll::const_iterator curr = coll.begin();
        Coll::const_iterator end = coll.end();
        if (curr != end) {
            const gchar *id = (*curr).key();
            FolksIndividual *individual((*curr).value());
            GValueStringCXX fullname;
            g_object_get_property(G_OBJECT(individual), "full-name", &fullname);

            SE_LOG_DEBUG(NULL, "first: id %s name %s = %s",
                         id,
                         fullname.toString().c_str(),
                         fullname.get());
            ++curr;
        }

        BOOST_FOREACH (Coll::value_type &entry, coll) {
            const gchar *id = entry.key();
            FolksIndividual *individual(entry.value());
            // GValueStringCXX fullname;
            // g_object_get_property(G_OBJECT(individual), "full-name", &fullname);
            const gchar *fullname = folks_name_details_get_full_name(FOLKS_NAME_DETAILS(individual));

            SE_LOG_DEBUG(NULL, "boost: id %s %s name %s",
                         id,
                         fullname ? "has" : "has no",
                         fullname);

            typedef GeeCollCXX<FolksEmailFieldDetails *> EmailColl;
            EmailColl emails(folks_email_details_get_email_addresses(FOLKS_EMAIL_DETAILS(individual)), ADD_REF);
            SE_LOG_DEBUG(NULL, "     %d emails", gee_collection_get_size(GEE_COLLECTION(emails.get())));
            BOOST_FOREACH (FolksEmailFieldDetails *email, emails) {
                SE_LOG_DEBUG(NULL, "     %s",
                             reinterpret_cast<const gchar *>(folks_abstract_field_details_get_value(FOLKS_ABSTRACT_FIELD_DETAILS(email))));
            }
        }

        aggregator.reset();
    }

    void gvalue() {
        GValueBooleanCXX b(true);
        SE_LOG_DEBUG(NULL, "GValueBooleanCXX(true) = %s", b.toString().c_str());
        GValueBooleanCXX b2(b);
        CPPUNIT_ASSERT_EQUAL(b.get(), b2.get());
        b2.set(false);
        CPPUNIT_ASSERT_EQUAL(b.get(), (gboolean)!b2.get());
        b2 = b;
        CPPUNIT_ASSERT_EQUAL(b.get(), b2.get());

        GValueStringCXX str("foo bar");
        SE_LOG_DEBUG(NULL, "GValueStringCXX(\"foo bar\") = %s", str.toString().c_str());
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

    static void individualSignal(std::ostringstream &out,
                                 const char *action,
                                 int index,
                                 const IndividualData &data) {
        out << action << ": " << index << " " <<
            folks_name_details_get_full_name(FOLKS_NAME_DETAILS(data.m_individual.get())) <<
            std::endl;
    }

    static void monitorView(IndividualView &view, std::ostringstream &out) {
        view.m_addedSignal.connect(boost::bind(individualSignal, boost::ref(out), "added", _1, _2));
        view.m_removedSignal.connect(boost::bind(individualSignal, boost::ref(out), "removed", _1, _2));
        view.m_modifiedSignal.connect(boost::bind(individualSignal, boost::ref(out), "modified", _1, _2));
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(FolksTest);

#endif

SE_END_CXX

