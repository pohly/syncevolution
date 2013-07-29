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

#include <memory>
#include <map>
#include <sstream>
#include <list>
using namespace std;

#include "config.h"
#include "EvolutionSyncSource.h"
#include <syncevo/IdentityProvider.h>

#ifdef ENABLE_EBOOK

#include <syncevo/SyncContext.h>
#include "EvolutionContactSource.h"
#include <syncevo/util.h>

#include <syncevo/Logging.h>

#ifdef USE_EDS_CLIENT
#include <boost/range/algorithm/find.hpp>
#endif
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/foreach.hpp>
#include <boost/lambda/lambda.hpp>

#include <syncevo/declarations.h>

SE_GLIB_TYPE(EBookQuery, e_book_query)

SE_BEGIN_CXX

inline bool IsContactNotFound(const GError *gerror) {
    return gerror &&
#ifdef USE_EDS_CLIENT
        gerror->domain == E_BOOK_CLIENT_ERROR &&
        gerror->code == E_BOOK_CLIENT_ERROR_CONTACT_NOT_FOUND
#else
        gerror->domain == E_BOOK_ERROR &&
        gerror->code == E_BOOK_ERROR_CONTACT_NOT_FOUND
#endif
        ;
}


const EvolutionContactSource::extensions EvolutionContactSource::m_vcardExtensions;
const EvolutionContactSource::unique EvolutionContactSource::m_uniqueProperties;

EvolutionContactSource::EvolutionContactSource(const SyncSourceParams &params,
                                               EVCardFormat vcardFormat) :
    EvolutionSyncSource(params),
    m_vcardFormat(vcardFormat)
{
#ifdef USE_EDS_CLIENT
    m_cacheMisses =
        m_cacheStalls =
        m_contactReads =
        m_contactsFromDB =
        m_contactQueries = 0;
    m_readAheadOrder = READ_NONE;
#endif
    SyncSourceLogging::init(InitList<std::string>("N_FIRST") + "N_MIDDLE" + "N_LAST",
                            " ",
                            m_operations);
}

EvolutionContactSource::~EvolutionContactSource()
{
    // Don't close while we have pending operations.  They might
    // complete after we got destroyed, causing them to use an invalid
    // "this" pointer. We also don't know how well EDS copes with
    // closing the address book while it has pending operations - EDS
    // maintainer mcrha wasn't sure.
    //
    // TODO: cancel the operations().
    finishItemChanges();
    close();

#ifdef USE_EDS_CLIENT
    // logCacheStats(Logger::DEBUG);
#endif
}

#ifdef USE_EDS_CLIENT
static EClient *newEBookClient(ESource *source,
                               GError **gerror)
{
    return E_CLIENT(e_book_client_new(source, gerror));
}
#endif

EvolutionSyncSource::Databases EvolutionContactSource::getDatabases()
{
    Databases result;
#ifdef USE_EDS_CLIENT
    getDatabasesFromRegistry(result,
                             E_SOURCE_EXTENSION_ADDRESS_BOOK,
                             e_source_registry_ref_default_address_book);
#else
    ESourceList *sources = NULL;
    if (!e_book_get_addressbooks(&sources, NULL)) {
        SyncContext::throwError("unable to access address books");
    }

    Databases secondary;
    for (GSList *g = e_source_list_peek_groups (sources); g; g = g->next) {
        ESourceGroup *group = E_SOURCE_GROUP (g->data);
        for (GSList *s = e_source_group_peek_sources (group); s; s = s->next) {
            ESource *source = E_SOURCE (s->data);
            eptr<char> uri(e_source_get_uri(source));
            string uristr;
            if (uri) {
                uristr = uri.get();
            }
            Database entry(e_source_peek_name(source),
                           uristr,
                           false);
            if (boost::starts_with(uristr, "couchdb://")) {
                // Append CouchDB address books at the end of the list,
                // otherwise preserving the order of address books.
                //
                // The reason is Moblin Bugzilla #7877 (aka CouchDB
                // feature request #479110): the initial release of
                // evolution-couchdb in Ubuntu 9.10 is unusable because
                // it does not support the REV property.
                //
                // Reordering the entries ensures that the CouchDB
                // address book is not used as the default database by
                // SyncEvolution, as it happened in Ubuntu 9.10.
                // Users can still pick it intentionally via
                // "evolutionsource".
                secondary.push_back(entry);
            } else {
                result.push_back(entry);
            }
        }
    }
    result.insert(result.end(), secondary.begin(), secondary.end());

    // No results? Try system address book (workaround for embedded Evolution Dataserver).
    if (!result.size()) {
        eptr<EBook, GObject> book;
        GErrorCXX gerror;
        const char *name;

        name = "<<system>>";
        book = e_book_new_system_addressbook (gerror);
        gerror.clear();
        if (!book) {
            name = "<<default>>";
            book = e_book_new_default_addressbook (gerror);
        }

        if (book) {
            const char *uri = e_book_get_uri (book);
            result.push_back(Database(name, uri, true));
        }
    } else {
        //  the first DB found is the default
        result[0].m_isDefault = true;
    }
#endif

    return result;
}

void EvolutionContactSource::open()
{
#ifdef USE_EDS_CLIENT
    m_addressbook.reset(E_BOOK_CLIENT(openESource(E_SOURCE_EXTENSION_ADDRESS_BOOK,
                                                  e_source_registry_ref_builtin_address_book,
                                                  newEBookClient).get()));
    const char *mode = getEnv("SYNCEVOLUTION_EDS_ACCESS_MODE", "");
    m_accessMode = boost::iequals(mode, "synchronous") ? SYNCHRONOUS :
        boost::iequals(mode, "batched") ? BATCHED :
        DEFAULT;
#else
    GErrorCXX gerror;
    bool created = false;
    bool onlyIfExists = false; // always try to create address book, because even if there is
                               // a source there's no guarantee that the actual database was
                               // created already; the original logic below for only setting
                               // this when explicitly requesting a new address book
                               // therefore failed in some cases
    ESourceList *tmp;
    if (!e_book_get_addressbooks(&tmp, gerror)) {
        throwError("unable to access address books", gerror);
    }
    ESourceListCXX sources(tmp, TRANSFER_REF);

    string id = getDatabaseID();
    ESource *source = findSource(sources, id);
    if (!source) {
        // might have been special "<<system>>" or "<<default>>", try that and
        // creating address book from file:// URI before giving up
        if (id.empty() || id == "<<system>>") {
            m_addressbook.set( e_book_new_system_addressbook (gerror), "system address book" );
        } else if (id.empty() || id == "<<default>>") {
            m_addressbook.set( e_book_new_default_addressbook (gerror), "default address book" );
        } else if (boost::starts_with(id, "file://")) {
            m_addressbook.set(e_book_new_from_uri(id.c_str(), gerror), "creating address book");
        } else {
            throwError(string(getName()) + ": no such address book: '" + id + "'");
        }
        created = true;
    } else {
        m_addressbook.set( e_book_new( source, gerror ), "address book" );
    }
 
    if (!e_book_open( m_addressbook, onlyIfExists, gerror) ) {
        if (created) {
            // opening newly created address books often fails, try again once more
            sleep(5);
            if (!e_book_open(m_addressbook, onlyIfExists, gerror)) {
                throwError("opening address book", gerror);
            }
        } else {
            throwError("opening address book", gerror);
        }
    }

    // users are not expected to configure an authentication method,
    // so pick one automatically if the user indicated that he wants authentication
    // by setting user or password
    UserIdentity identity = getUser();
    InitStateString passwd = getPassword();
    if (identity.wasSet() || passwd.wasSet()) {
        GList *authmethod;
        if (!e_book_get_supported_auth_methods(m_addressbook, &authmethod, gerror)) {
            throwError("getting authentication methods", gerror);
        }
        while (authmethod) {
            // map identity + password to plain username/password credentials
            Credentials cred = IdentityProviderCredentials(identity, passwd);
            const char *method = (const char *)authmethod->data;
            SE_LOG_DEBUG(getDisplayName(), "trying authentication method \"%s\", user %s, password %s",
                         method,
                         identity.wasSet() ? "configured" : "not configured",
                         passwd.wasSet() ? "configured" : "not configured");
            if (e_book_authenticate_user(m_addressbook,
                                         cred.m_username.c_str(),
                                         cred.m_password.c_str(),
                                         method,
                                         gerror)) {
                SE_LOG_DEBUG(getDisplayName(), "authentication succeeded");
                break;
            } else {
                SE_LOG_ERROR(getDisplayName(), "authentication failed: %s", gerror->message);
            }
            authmethod = authmethod->next;
        }
    }

    g_signal_connect_after(m_addressbook,
                           "backend-died",
                           G_CALLBACK(SyncContext::fatalError),
                           (void *)"Evolution Data Server has died unexpectedly, contacts no longer available.");
#endif
}

bool EvolutionContactSource::isEmpty()
{
    // TODO: add more efficient implementation which does not
    // depend on actually pulling all items from EDS
    RevisionMap_t revisions;
    listAllItems(revisions);
    return revisions.empty();
}

#ifdef USE_EDS_CLIENT
class EBookClientViewSyncHandler {
    public:
        typedef boost::function<void (const GSList *list)> Process_t;

        EBookClientViewSyncHandler(const EBookClientViewCXX &view,
                                   const Process_t &process) :
            m_process(process),
            m_view(view)
        {}

        bool process(GErrorCXX &gerror) {
            // Listen for view signals
            m_view.connectSignal<void (EBookClientView *ebookview,
                                       const GSList *contacts)>("objects-added",
                                                                boost::bind(m_process, _2));
            m_view.connectSignal<void (EBookClientView *ebookview,
                                       const GError *error)>("complete",
                                                             boost::bind(&EBookClientViewSyncHandler::completed, this, _2));

            // Start the view
            e_book_client_view_start (m_view, m_error);
            if (m_error) {
                std::swap(gerror, m_error);
                return false;
            }

            // Async -> Sync
            m_loop.run();
            e_book_client_view_stop (m_view, NULL); 

            if (m_error) {
                std::swap(gerror, m_error);
                return false;
            } else {
                return true;
            }
        }
 
        void completed(const GError *error) {
            m_error = error;
            m_loop.quit();
        }

    public:
        // Event loop for Async -> Sync
        EvolutionAsync m_loop;

    private:
        // Process list callback
        boost::function<void (const GSList *list)> m_process;
        // View watched
        EBookClientViewCXX m_view;
        // Possible error while watching the view
        GErrorCXX m_error;
};

static void list_revisions(const GSList *contacts, EvolutionContactSource::RevisionMap_t *revisions)
{
    const GSList *l;

    for (l = contacts; l; l = l->next) {
        EContact *contact = E_CONTACT(l->data);
        if (!contact) {
            SE_THROW("contact entry without data");
        }
        pair<string, string> revmapping;
        const char *uid = (const char *)e_contact_get_const(contact,
                                                            E_CONTACT_UID);
        if (!uid || !uid[0]) {
            SE_THROW("contact entry without UID");
        }
        revmapping.first = uid;
        const char *rev = (const char *)e_contact_get_const(contact,
                                                            E_CONTACT_REV);
        if (!rev || !rev[0]) {
            SE_THROW(string("contact entry without REV: ") + revmapping.first);
        }
        revmapping.second = rev;
        revisions->insert(revmapping);
    }
}

#endif

void EvolutionContactSource::listAllItems(RevisionMap_t &revisions)
{
#ifdef USE_EDS_CLIENT
    GErrorCXX gerror;
    EBookClientView *view;

    EBookQueryCXX allItemsQuery(e_book_query_any_field_contains(""), TRANSFER_REF);
    PlainGStr sexp(e_book_query_to_string (allItemsQuery.get()));

    if (!e_book_client_get_view_sync(m_addressbook, sexp, &view, NULL, gerror)) {
        throwError( "getting the view" , gerror);
    }
    EBookClientViewCXX viewPtr = EBookClientViewCXX::steal(view);

    // Optimization: set fields_of_interest (UID / REV)
    GListCXX<const char, GSList> interesting_field_list;
    interesting_field_list.push_back(e_contact_field_name (E_CONTACT_UID));
    interesting_field_list.push_back(e_contact_field_name (E_CONTACT_REV));
    e_book_client_view_set_fields_of_interest (viewPtr, interesting_field_list, gerror);
    if (gerror) {
        SE_LOG_ERROR(getDisplayName(), "e_book_client_view_set_fields_of_interest: %s", (const char*)gerror);
        gerror.clear();
    }

    EBookClientViewSyncHandler handler(viewPtr, boost::bind(list_revisions, _1, &revisions));
    if (!handler.process(gerror)) {
        throwError("watching view", gerror);
    }
#else
    GErrorCXX gerror;
    eptr<EBookQuery> allItemsQuery(e_book_query_any_field_contains(""), "query");
    GList *nextItem;
    if (!e_book_get_contacts(m_addressbook, allItemsQuery, &nextItem, gerror)) {
        throwError( "reading all items", gerror );
    }
    eptr<GList> listptr(nextItem);
    while (nextItem) {
        EContact *contact = E_CONTACT(nextItem->data);
        if (!contact) {
            throwError("contact entry without data");
        }
        pair<string, string> revmapping;
        const char *uid = (const char *)e_contact_get_const(contact,
                                                            E_CONTACT_UID);
        if (!uid || !uid[0]) {
            throwError("contact entry without UID");
        }
        revmapping.first = uid;
        const char *rev = (const char *)e_contact_get_const(contact,
                                                            E_CONTACT_REV);
        if (!rev || !rev[0]) {
            throwError(string("contact entry without REV: ") + revmapping.first);
        }
        revmapping.second = rev;
        revisions.insert(revmapping);
        nextItem = nextItem->next;
    }
#endif
}

void EvolutionContactSource::close()
{
    m_addressbook.reset();
}

string EvolutionContactSource::getRevision(const string &luid)
{
    if (!needChanges()) {
        return "";
    }

    EContact *contact;
    GErrorCXX gerror;
    if (
#ifdef USE_EDS_CLIENT
        !e_book_client_get_contact_sync(m_addressbook,
                                        luid.c_str(),
                                        &contact,
                                        NULL,
                                        gerror)
#else
        !e_book_get_contact(m_addressbook,
                            luid.c_str(),
                            &contact,
                            gerror)
#endif
        ) {
        if (IsContactNotFound(gerror)) {
            throwError(STATUS_NOT_FOUND, string("retrieving item: ") + luid);
        } else {
            throwError(string("reading contact ") + luid,
                       gerror);
        }
    }
    eptr<EContact, GObject> contactptr(contact, "contact");
    const char *rev = (const char *)e_contact_get_const(contact,
                                                        E_CONTACT_REV);
    if (!rev || !rev[0]) {
        throwError(string("contact entry without REV: ") + luid);
    }
    return rev;
}

#ifdef USE_EDS_CLIENT
class ContactCache : public std::map<std::string, EContactCXX>
{
public:
    /** Asynchronous method call still pending. */
    bool m_running;
    /** The last luid requested in this query. Needed to start with the next contact after it. */
    std::string m_lastLUID;
    /** Result of batch read. Any error here means that the call failed completely. */
    GErrorCXX m_gerror;
    /** A debug logging name for this query. */
    std::string m_name;
};

void EvolutionContactSource::setReadAheadOrder(ReadAheadOrder order,
                                               const ReadAheadItems &luids)
{
    SE_LOG_DEBUG(getDisplayName(), "reading: set order '%s', %ld luids",
                 order == READ_NONE ? "none" :
                 order == READ_ALL_ITEMS ? "all" :
                 order == READ_CHANGED_ITEMS ? "changed" :
                 order == READ_SELECTED_ITEMS ? "selected" :
                 "???",
                 (long)luids.size());
    m_readAheadOrder = order;
    m_nextLUIDs = luids;

    // Be conservative and throw away all cached data. Not doing so
    // can confuse our "cache miss" counting, for example when it uses
    // a cache where some entries have been removed in
    // invalidateCachedContact() and then mistakes the gaps for cache
    // misses.
    //
    // Another reason is that we want to use fairly recent data (in
    // case of concurrent changes in the DB, which currently is not
    // detected by the cache).
    m_contactCache.reset();
    m_contactCacheNext.reset();
}

void EvolutionContactSource::getReadAheadOrder(ReadAheadOrder &order,
                                               ReadAheadItems &luids)
{
    order = m_readAheadOrder;
    luids = m_nextLUIDs;
}

void EvolutionContactSource::checkCacheForError(boost::shared_ptr<ContactCache> &cache)
{
    if (cache->m_gerror) {
        GErrorCXX gerror;
        std::swap(gerror, cache->m_gerror);
        cache.reset();
        throwError(StringPrintf("reading contacts %s", cache->m_name.c_str()), gerror);
    }
}

void EvolutionContactSource::invalidateCachedContact(const std::string &luid)
{
    invalidateCachedContact(m_contactCache, luid);
    invalidateCachedContact(m_contactCacheNext, luid);
}

void EvolutionContactSource::invalidateCachedContact(boost::shared_ptr<ContactCache> &cache, const std::string &luid)
{
    if (cache) {
        ContactCache::iterator it = cache->find(luid);
        if (it != cache->end()) {
            SE_LOG_DEBUG(getDisplayName(), "reading: remove contact %s from cache because of remove or update", luid.c_str());
            // If we happen to read that contact (unlikely), it'll be
            // considered a cache miss. That's okay. Together with
            // counting cache misses it'll help us avoid using
            // read-ahead when the Synthesis engine is randomly
            // accessing contacts.
            cache->erase(it);
        }
    }
}

bool EvolutionContactSource::getContact(const string &luid, EContact **contact, GErrorCXX &gerror)
{
    SE_LOG_DEBUG(getDisplayName(), "reading: getting contact %s", luid.c_str());
    ReadAheadOrder order = m_readAheadOrder;

    // Use switch and let compiler tell us when we don't cover a case.
    switch (m_accessMode) {
    case SYNCHRONOUS:
        order = READ_NONE;
        break;
    case BATCHED:
    case DEFAULT:
        order = m_readAheadOrder;
        break;
    };

    m_contactReads++;
    if (order == READ_NONE) {
        m_contactsFromDB++;
        m_contactQueries++;
        return e_book_client_get_contact_sync(m_addressbook,
                                              luid.c_str(),
                                              contact,
                                              NULL,
                                              gerror);
    } else {
        return getContactFromCache(luid, contact, gerror);
    }
}

bool EvolutionContactSource::getContactFromCache(const string &luid, EContact **contact, GErrorCXX &gerror)
{
    *contact = NULL;

    // Use ContactCache.
    if (m_contactCache) {
        SE_LOG_DEBUG(getDisplayName(), "reading: active cache %s", m_contactCache->m_name.c_str());
        // Ran into a problem?
        checkCacheForError(m_contactCache);

        // Does the cache cover our item?
        ContactCache::const_iterator it = m_contactCache->find(luid);
        if (it == m_contactCache->end()) {
            if (m_contactCacheNext) {
                SE_LOG_DEBUG(getDisplayName(), "reading: not in cache, try cache %s",
                             m_contactCacheNext->m_name.c_str());
                // Throw away old cache, try with next one. This is not
                // a cache miss (yet).
                m_contactCache = m_contactCacheNext;
                m_contactCacheNext.reset();
                return getContactFromCache(luid, contact, gerror);
            } else {
                SE_LOG_DEBUG(getDisplayName(), "reading: not in cache, nothing pending -> start reading");
                // Throw away cache, start new read below.
                m_contactCache.reset();
            }
        } else {
            SE_LOG_DEBUG(getDisplayName(), "reading: in %s cache", m_contactCache->m_running ? "running" : "loaded");
            if (m_contactCache->m_running) {
                m_cacheStalls++;
                GRunWhile(boost::lambda::var(m_contactCache->m_running));
            }
            // Problem?
            checkCacheForError(m_contactCache);

            SE_LOG_DEBUG(getDisplayName(), "reading: in cache, %s", it->second ? "available" : "not found");
            if (it->second) {
                // Got it.
                *contact = it->second.ref();
            } else {
                // Delay throwing error. We need to go through the read-ahead code below.
                gerror.take(g_error_new(E_BOOK_CLIENT_ERROR, E_BOOK_CLIENT_ERROR_CONTACT_NOT_FOUND,
                                        "uid %s not found in batch read", luid.c_str()));
            }
        }
    }

    // No current cache? In that case we must read and block.
    if (!m_contactCache) {
        m_contactCache = startReading(luid, START);
        // Call code above recursively, which will block.
        return getContactFromCache(luid, contact, gerror);
    }

    // Can we read ahead?
    if (!m_contactCacheNext && !m_contactCache->m_running) {
        m_contactCacheNext = startReading(m_contactCache->m_lastLUID, CONTINUE);
    }

    // Everything is okay when we get here. Either we have the contact or
    // it wasn't in the database.
    SE_LOG_DEBUG(getDisplayName(), "reading: read %s: %s", luid.c_str(), gerror ? gerror->message : "<<okay>>");
    logCacheStats(Logger::DEBUG);
    return !gerror;
}

static int MaxBatchSize()
{
    int maxBatchSize = atoi(getEnv("SYNCEVOLUTION_EDS_BATCH_SIZE", "50"));
    if (maxBatchSize < 1) {
        maxBatchSize = 1;
    }
    return maxBatchSize;
}

boost::shared_ptr<ContactCache> EvolutionContactSource::startReading(const std::string &luid, ReadingMode mode)
{
    SE_LOG_DEBUG(getDisplayName(), "reading: %s contact %s",
                 mode == START ? "must read" :
                 mode == CONTINUE ? "continue after" :
                 "???",
                 luid.c_str());

    static int maxBatchSize = MaxBatchSize();
    std::vector<EBookQueryCXX> uidQueries;
    uidQueries.resize(maxBatchSize);
    std::vector<const std::string *> uids;
    uids.resize(maxBatchSize);
    int size = 0;
    bool found = false;

    switch (m_readAheadOrder) {
    case READ_ALL_ITEMS:
    case READ_CHANGED_ITEMS: {
        const Items_t &items = getAllItems();
        const Items_t &newItems = getNewItems();
        const Items_t &updatedItems = getUpdatedItems();
        Items_t::const_iterator it = items.find(luid);

        // Always read the requested item, even if not found in item list?
        if (mode == START) {
            uids[0] = &luid;
            size++;
        }
        // luid is dealt with, either way.
        if (it != items.end()) {
            // Check that it is a valid candidate for caching, else
            // we have a cache miss prediction.
            if (m_readAheadOrder == READ_ALL_ITEMS ||
                newItems.find(luid) != newItems.end() ||
                updatedItems.find(luid) != updatedItems.end()) {
                found = true;
            }
            ++it;
        }
        while (size < maxBatchSize &&
               it != items.end()) {
            const std::string &luid = *it;
            if (m_readAheadOrder == READ_ALL_ITEMS ||
                newItems.find(luid) != newItems.end() ||
                updatedItems.find(luid) != updatedItems.end()) {
                uids[size] = &luid;
                ++size;
            }
            ++it;
        }
        break;
    }
    case READ_SELECTED_ITEMS: {
        ReadAheadItems::const_iterator it = boost::find(std::make_pair(m_nextLUIDs.begin(), m_nextLUIDs.end()), luid);
        // Always read the requested item, even if not found in item list?
        if (mode == START) {
            uids[0] = &luid;
            size++;
        }
        // luid is dealt with, either way.
        if (it != m_nextLUIDs.end()) {
            found = true;
            ++it;
        }
        while (size < maxBatchSize &&
               it != m_nextLUIDs.end()) {
            uids[size] = &*it;
            ++size;
            ++it;
        }
        break;
    }
    case READ_NONE:
        // May be reached when read-ahead was turned off while
        // preparing for it.
        if (mode == START) {
            uids[0] = &luid;
            size++;
        }
        break;
    }

    if (m_readAheadOrder != READ_NONE &&
        mode == START &&
        !found) {
        // The requested contact was not on our list. Consider this
        // a cache miss (or rather, cache prediction failure) and turn
        // of the read-ahead.
        m_cacheMisses++;
        SE_LOG_DEBUG(getDisplayName(), "reading: disable read-ahead due to cache miss");
        m_readAheadOrder = READ_NONE;
    }

    boost::shared_ptr<ContactCache> cache;
    if (size) {
        // Prepare parameter for EDS C call. Ownership of query instances is in uidQueries array.
        boost::scoped_array<EBookQuery *> queries(new EBookQuery *[size]);
        for (int i = 0; i < size; i++) {
            // This shouldn't compile because we don't specify how ownership is handled.
            // The reset() method always bumps the ref count, which is not what we want here!
            // uidQueries[i].reset(e_book_query_field_test(E_CONTACT_UID, E_BOOK_QUERY_IS, it->c_str()));
            //
            // Take over ownership.
            uidQueries[i] = EBookQueryCXX::steal(e_book_query_field_test(E_CONTACT_UID, E_BOOK_QUERY_IS, uids[i]->c_str()));
            queries[i] = uidQueries[i].get();
        }
        EBookQueryCXX query(e_book_query_or(size, queries.get(), false), TRANSFER_REF);
        PlainGStr sexp(e_book_query_to_string(query.get()));

        cache.reset(new ContactCache);
        cache->m_running = true;
        cache->m_name = StringPrintf("%s-%s (%d)", uids[0]->c_str(), uids[size - 1]->c_str(), size);
        cache->m_lastLUID = *uids[size - 1];
        BOOST_FOREACH (const std::string *uid, std::make_pair(uids.begin(), uids.begin() + size)) {
            (*cache)[*uid] = EContactCXX();
        }
        m_contactsFromDB += size;
        m_contactQueries++;
        SYNCEVO_GLIB_CALL_ASYNC(e_book_client_get_contacts,
                                boost::bind(&EvolutionContactSource::completedRead,
                                            this,
                                            boost::weak_ptr<ContactCache>(cache),
                                            _1, _2, _3),
                                m_addressbook, sexp, NULL);
        SE_LOG_DEBUG(getDisplayName(), "reading: started contact read %s", cache->m_name.c_str());
    }
    return cache;
}

typedef GListCXX< EContact, GSList, GObjectDestructor<EContact> > ContactListCXX;

void EvolutionContactSource::completedRead(const boost::weak_ptr<ContactCache> &cachePtr, gboolean success, GSList *contactsPtr, const GError *gerror) throw()
{
    try {
        ContactListCXX contacts(contactsPtr); // transfers ownership
        boost::shared_ptr<ContactCache> cache = cachePtr.lock();
        if (!cache) {
            SE_LOG_DEBUG(getDisplayName(), "reading: contact read finished, results no longer needed: %s", gerror ? gerror->message : "<<successful>>");
            return;
        }

        SE_LOG_DEBUG(getDisplayName(), "reading: contact read %s finished: %s",
                     cache->m_name.c_str(),
                     gerror ? gerror->message : "<<successful>>");
        if (success) {
            BOOST_FOREACH (EContact *contact, contacts) {
                const char *uid = (const char *)e_contact_get_const(contact, E_CONTACT_UID);
                SE_LOG_DEBUG(getDisplayName(), "reading: contact read %s got %s", cache->m_name.c_str(), uid);
                (*cache)[uid] = EContactCXX(contact, ADD_REF);
            }
        } else {
            cache->m_gerror = gerror;
        }
        cache->m_running = false;
    } catch (...) {
        Exception::handle(HANDLE_EXCEPTION_FATAL);
    }
}

void EvolutionContactSource::logCacheStats(Logger::Level level)
{
    SE_LOG(getDisplayName(), level,
           "requested %d, retrieved %d from DB in %d queries, misses %d/%d (%d%%), stalls %d",
           m_contactReads,
           m_contactsFromDB,
           m_contactQueries,
           m_cacheMisses, m_contactReads, m_contactReads ? m_cacheMisses * 100 / m_contactReads : 0,
           m_cacheStalls);
}

#endif

void EvolutionContactSource::readItem(const string &luid, std::string &item, bool raw)
{
    EContact *contact;
    GErrorCXX gerror;
    if (
#ifdef USE_EDS_CLIENT
        !getContact(luid, &contact, gerror)
#else
        !e_book_get_contact(m_addressbook,
                            luid.c_str(),
                            &contact,
                            gerror)
#endif
        ) {
        if (IsContactNotFound(gerror)) {
            throwError(STATUS_NOT_FOUND, string("reading contact: ") + luid);
        } else {
            throwError(string("reading contact ") + luid,
                       gerror);
        }
    }

    eptr<EContact, GObject> contactptr(contact, "contact");

    // Inline PHOTO data if exporting, leave VALUE=uri references unchanged
    // when processing inside engine (will be inlined by engine as needed).
    // The function for doing the inlining was added in EDS 3.4.
    // In compatibility mode, we must check the function pointer for non-NULL.
    // In direct call mode, the existence check is done by configure.
    if (raw
#ifdef EVOLUTION_COMPATIBILITY
        && e_contact_inline_local_photos
#endif
        ) {
#if defined(EVOLUTION_COMPATIBILITY) || defined(HAVE_E_CONTACT_INLINE_LOCAL_PHOTOS)
        if (!e_contact_inline_local_photos(contactptr, gerror)) {
            throwError(string("inlining PHOTO file data in ") + luid, gerror);
        }
#endif
    }

    eptr<char> vcardstr(e_vcard_to_string(&contactptr->parent,
                                          EVC_FORMAT_VCARD_30));
    if (!vcardstr) {
        throwError(string("failure extracting contact from Evolution " ) + luid);
    }

    item = vcardstr.get();
}

#ifdef USE_EDS_CLIENT
TrackingSyncSource::InsertItemResult EvolutionContactSource::checkBatchedInsert(const boost::shared_ptr<Pending> &pending)
{
    SE_LOG_DEBUG(pending->m_name, "checking operation: %s", pending->m_status == MODIFYING ? "waiting" : "inserted");
    if (pending->m_status == MODIFYING) {
        return TrackingSyncSource::InsertItemResult(boost::bind(&EvolutionContactSource::checkBatchedInsert, this, pending));
    }
    if (pending->m_gerror) {
        pending->m_gerror.throwError(pending->m_name);
    }
    string newrev = getRevision(pending->m_uid);
    return TrackingSyncSource::InsertItemResult(pending->m_uid, newrev, ITEM_OKAY);
}

void EvolutionContactSource::completedAdd(const boost::shared_ptr<PendingContainer_t> &batched, gboolean success, GSList *uids, const GError *gerror) throw()
{
    try {
        // The destructor ensures that the pending operations complete
        // before destructing the instance, so our "this" pointer is
        // always valid here.
        SE_LOG_DEBUG(getDisplayName(), "batch add of %d contacts completed", (int)batched->size());
        m_numRunningOperations--;
        PendingContainer_t::iterator it = (*batched).begin();
        GSList *uid = uids;
        while (it != (*batched).end() && uid) {
            SE_LOG_DEBUG((*it)->m_name, "completed: %s",
                         success ? "<<successfully>>" :
                         gerror ? gerror->message :
                         "<<unknown failure>>");
            if (success) {
                (*it)->m_uid = static_cast<gchar *>(uid->data);
                // Get revision when engine checks the item.
                (*it)->m_status = REVISION;
            } else {
                (*it)->m_status = DONE;
                (*it)->m_gerror = gerror;
            }
            ++it;
            uid = uid->next;
        }

        while (it != (*batched).end()) {
            // Should never happen.
            SE_LOG_DEBUG((*it)->m_name, "completed: missing uid?!");
            (*it)->m_status = DONE;
            ++it;
        }

        g_slist_free_full(uids, g_free);
    } catch (...) {
        Exception::handle(HANDLE_EXCEPTION_FATAL);
    }
}

void EvolutionContactSource::completedUpdate(const boost::shared_ptr<PendingContainer_t> &batched, gboolean success, const GError *gerror) throw()
{
    try {
        SE_LOG_DEBUG(getDisplayName(), "batch update of %d contacts completed", (int)batched->size());
        m_numRunningOperations--;
        PendingContainer_t::iterator it = (*batched).begin();
        while (it != (*batched).end()) {
            SE_LOG_DEBUG((*it)->m_name, "completed: %s",
                         success ? "<<successfully>>" :
                         gerror ? gerror->message :
                         "<<unknown failure>>");
            if (success) {
                (*it)->m_status = REVISION;
            } else {
                (*it)->m_status = DONE;
                (*it)->m_gerror = gerror;
            }
            ++it;
        }
    } catch (...) {
        Exception::handle(HANDLE_EXCEPTION_FATAL);
    }
}

void EvolutionContactSource::flushItemChanges()
{
    if (!m_batchedAdd.empty()) {
        SE_LOG_DEBUG(getDisplayName(), "batch add of %d contacts starting", (int)m_batchedAdd.size());
        m_numRunningOperations++;
        GListCXX<EContact, GSList> contacts;
        // Iterate backwards, push to front (cheaper for single-linked list) -> same order in the end.
        BOOST_REVERSE_FOREACH (const boost::shared_ptr<Pending> &pending, m_batchedAdd) {
            contacts.push_front(pending->m_contact.get());
        }
        // Transfer content without copying and then copy only the shared pointer.
        boost::shared_ptr<PendingContainer_t> batched(new PendingContainer_t);
        std::swap(*batched, m_batchedAdd);
        SYNCEVO_GLIB_CALL_ASYNC(e_book_client_add_contacts,
                                boost::bind(&EvolutionContactSource::completedAdd,
                                            this,
                                            batched,
                                            _1, _2, _3),
                                m_addressbook, contacts, NULL);
    }
    if (!m_batchedUpdate.empty()) {
        SE_LOG_DEBUG(getDisplayName(), "batch update of %d contacts starting", (int)m_batchedUpdate.size());
        m_numRunningOperations++;
        GListCXX<EContact, GSList> contacts;
        BOOST_REVERSE_FOREACH (const boost::shared_ptr<Pending> &pending, m_batchedUpdate) {
            contacts.push_front(pending->m_contact.get());
        }
        boost::shared_ptr<PendingContainer_t> batched(new PendingContainer_t);
        std::swap(*batched, m_batchedUpdate);
        SYNCEVO_GLIB_CALL_ASYNC(e_book_client_modify_contacts,
                                boost::bind(&EvolutionContactSource::completedUpdate,
                                            this,
                                            batched,
                                            _1, _2),
                                m_addressbook, contacts, NULL);
    }
}

void EvolutionContactSource::finishItemChanges()
{
    if (m_numRunningOperations) {
        SE_LOG_DEBUG(getDisplayName(), "waiting for %d pending operations to complete", m_numRunningOperations.get());
        while (m_numRunningOperations) {
            g_main_context_iteration(NULL, true);
        }
        SE_LOG_DEBUG(getDisplayName(), "pending operations completed");
    }
}

#endif

TrackingSyncSource::InsertItemResult
EvolutionContactSource::insertItem(const string &uid, const std::string &item, bool raw)
{
    EContactCXX contact(e_contact_new_from_vcard(item.c_str()), TRANSFER_REF);
    if (contact) {
        e_contact_set(contact, E_CONTACT_UID,
                      uid.empty() ?
                      NULL :
                      const_cast<char *>(uid.c_str()));
        GErrorCXX gerror;
#ifdef USE_EDS_CLIENT
        invalidateCachedContact(uid);
        switch (m_accessMode) {
        case SYNCHRONOUS:
            if (uid.empty()) {
                gchar* newuid;
                if (!e_book_client_add_contact_sync(m_addressbook, contact, &newuid, NULL, gerror)) {
                    throwError("add new contact", gerror);
                }
                PlainGStr newuidPtr(newuid);
                string newrev = getRevision(newuid);
                return InsertItemResult(newuid, newrev, ITEM_OKAY);
            } else {
                if (!e_book_client_modify_contact_sync(m_addressbook, contact, NULL, gerror)) {
                    throwError("updating contact "+ uid, gerror);
                }
                string newrev = getRevision(uid);
                return InsertItemResult(uid, newrev, ITEM_OKAY);
            }
            break;
        case BATCHED:
        case DEFAULT:
            std::string name = StringPrintf("%s: %s operation #%d",
                                            getDisplayName().c_str(),
                                            uid.empty() ? "add" : ("insert " + uid).c_str(),
                                            m_asyncOpCounter++);
            SE_LOG_DEBUG(name, "queueing for batched %s", uid.empty() ? "add" : "update");
            boost::shared_ptr<Pending> pending(new Pending);
            pending->m_name = name;
            pending->m_contact = contact;
            pending->m_uid = uid;
            if (uid.empty()) {
                m_batchedAdd.push_back(pending);
            } else {
                m_batchedUpdate.push_back(pending);
            }
            // SyncSource is going to live longer than Synthesis
            // engine, so using "this" is safe here.
            return InsertItemResult(boost::bind(&EvolutionContactSource::checkBatchedInsert, this, pending));
            break;
        }
#else
        if (uid.empty() ?
            e_book_add_contact(m_addressbook, contact, gerror) :
            e_book_commit_contact(m_addressbook, contact, gerror)) {
            const char *newuid = (const char *)e_contact_get_const(contact, E_CONTACT_UID);
            if (!newuid) {
                throwError("no UID for contact");
            }
            string newrev = getRevision(newuid);
            return InsertItemResult(newuid, newrev, ITEM_OKAY);
        } else {
            throwError(uid.empty() ?
                       "storing new contact" :
                       string("updating contact ") + uid,
                       gerror);
        }
#endif
    } else {
        throwError(string("failure parsing vcard " ) + item);
    }
    // not reached!
    return InsertItemResult("", "", ITEM_OKAY);
}

void EvolutionContactSource::removeItem(const string &uid)
{
    GErrorCXX gerror;
    if (
#ifdef USE_EDS_CLIENT
        (invalidateCachedContact(uid),
         !e_book_client_remove_contact_by_uid_sync(m_addressbook, uid.c_str(), NULL, gerror))
#else
        !e_book_remove_contact(m_addressbook, uid.c_str(), gerror)
#endif
        ) {
        if (IsContactNotFound(gerror)) {
            throwError(STATUS_NOT_FOUND, string("deleting contact: ") + uid);
        } else {
            throwError( string( "deleting contact " ) + uid,
                        gerror);
        }
    }
}

std::string EvolutionContactSource::getDescription(const string &luid)
{
    try {
        EContact *contact;
        GErrorCXX gerror;
        if (
#ifdef USE_EDS_CLIENT
            !getContact(luid,
                        &contact,
                        gerror)
#else
            !e_book_get_contact(m_addressbook,
                                luid.c_str(),
                                &contact,
                                gerror)
#endif
            ) {
            throwError(string("reading contact ") + luid,
                       gerror);
        }
        eptr<EContact, GObject> contactptr(contact, "contact");
        const char *name = (const char *)e_contact_get_const(contact, E_CONTACT_FULL_NAME);
        if (name) {
            return name;
        }
        const char *fileas = (const char *)e_contact_get_const(contact, E_CONTACT_FILE_AS);
        if (fileas) {
            return fileas;
        }
        EContactName *names =
            (EContactName *)e_contact_get(contact, E_CONTACT_NAME);
        std::list<std::string> buffer;
        if (names) {
            try {
                if (names->given && names->given[0]) {
                    buffer.push_back(names->given);
                }
                if (names->additional && names->additional[0]) {
                    buffer.push_back(names->additional);
                }
                if (names->family && names->family[0]) {
                    buffer.push_back(names->family);
                }
            } catch (...) {
            }
            e_contact_name_free(names);
        }
        return boost::join(buffer, " ");
    } catch (...) {
        // Instead of failing we log the error and ask
        // the caller to log the UID. That way transient
        // errors or errors in the logging code don't
        // prevent syncs.
        handleException();
        return "";
    }
}

std::string EvolutionContactSource::getMimeType() const
{
    switch( m_vcardFormat ) {
     case EVC_FORMAT_VCARD_21:
        return "text/x-vcard";
        break;
     case EVC_FORMAT_VCARD_30:
     default:
        return "text/vcard";
        break;
    }
}

std::string EvolutionContactSource::getMimeVersion() const
{
    switch( m_vcardFormat ) {
     case EVC_FORMAT_VCARD_21:
        return "2.1";
        break;
     case EVC_FORMAT_VCARD_30:
     default:
        return "3.0";
        break;
    }
}

SE_END_CXX

#endif /* ENABLE_EBOOK */

#ifdef ENABLE_MODULES
# include "EvolutionContactSourceRegister.cpp"
#endif
