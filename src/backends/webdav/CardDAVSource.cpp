/*
 * Copyright (C) 2010 Intel Corporation
 */

#include "CardDAVSource.h"

#ifdef ENABLE_DAV

#include <syncevo/declarations.h>
SE_BEGIN_CXX

// TODO: use EDS backend icalstrdup.c
#define ical_strdup(_x) (_x)

typedef boost::shared_ptr<TransportStatusException> BatchReadFailure;

class CardDAVCache : public std::map< std::string, boost::variant<std::string, BatchReadFailure> >
{
};


CardDAVSource::CardDAVSource(const SyncSourceParams &params,
                             const boost::shared_ptr<Neon::Settings> &settings) :
    WebDAVSource(params, settings),
    m_readAheadOrder(READ_NONE),
    m_cacheMisses(0),
    m_contactReads(0),
    m_contactsFromDB(0),
    m_contactQueries(0)
{
    SyncSourceLogging::init(InitList<std::string>("N_FIRST") + "N_MIDDLE" + "N_LAST",
                            " ",
                            m_operations);
}

void CardDAVSource::logCacheStats(Logger::Level level)
{
    SE_LOG(getDisplayName(), level,
           "requested %d, retrieved %d from server in %d queries, misses %d/%d (%d%%)",
           m_contactReads,
           m_contactsFromDB,
           m_contactQueries,
           m_cacheMisses, m_contactReads,
           m_contactReads ? m_cacheMisses * 100 / m_contactReads : 0);
}

std::string CardDAVSource::getDescription(const string &luid)
{
    // TODO
    return "";
}

void CardDAVSource::readItemInternal(const std::string &luid, std::string &item, bool raw)
{
    if (m_cardDAVCache) {
        CardDAVCache::const_iterator it = m_cardDAVCache->find(luid);
        if (it != m_cardDAVCache->end()) {
            const std::string *data = boost::get<const std::string>(&it->second);
            if (data) {
                SE_LOG_DEBUG(getDisplayName(), "reading %s from cache", luid.c_str());
                item = *data;
                return;
            }
            const BatchReadFailure *failure = boost::get<BatchReadFailure>(&it->second);
            if (failure) {
                SE_LOG_DEBUG(getDisplayName(), "reading %s into cache had failed: %s",
                             luid.c_str(), (*failure)->what());
                throw **failure;
            }
            SE_THROW(StringPrintf("internal error, empty cache entry for %s", luid.c_str()));
        }
    }

    if (m_readAheadOrder != READ_NONE) {
        m_cardDAVCache = readBatch(luid);
        // Try again above.
        readItemInternal(luid, item, raw);
        return;
    }

    // Fallback: get inidividual item.
    m_contactsFromDB++;
    m_contactQueries++;
    WebDAVSource::readItem(luid, item, raw);
}

void CardDAVSource::readItem(const std::string &luid, std::string &item, bool raw)
{
    m_contactReads++;
    readItemInternal(luid, item, raw);
    logCacheStats(Logger::DEBUG);
}

static size_t MaxBatchSize()
{
    int maxBatchSize = atoi(getEnv("SYNCEVOLUTION_CARDDAV_BATCH_SIZE", "50"));
    if (maxBatchSize < 1) {
        maxBatchSize = 1;
    }
    return maxBatchSize;
}

boost::shared_ptr<CardDAVCache> CardDAVSource::readBatch(const std::string &luid)
{
    static size_t maxBatchSize = MaxBatchSize();
    BatchLUIDs luids;
    luids.reserve(maxBatchSize);
    bool found = false;

    switch (m_readAheadOrder) {
    case READ_ALL_ITEMS:
    case READ_CHANGED_ITEMS: {
        const Items_t &items = getAllItems();
        const Items_t &newItems = getNewItems();
        const Items_t &updatedItems = getUpdatedItems();
        Items_t::const_iterator it = items.find(luid);

        // Always read the requested item, even if not found in item list.
        luids.push_back(&luid);

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
        while (luids.size() < maxBatchSize &&
               it != items.end()) {
            const std::string &luid = *it;
            if (m_readAheadOrder == READ_ALL_ITEMS ||
                newItems.find(luid) != newItems.end() ||
                updatedItems.find(luid) != updatedItems.end()) {
                luids.push_back(&luid);
            }
            ++it;
        }
        break;
    }
    case READ_SELECTED_ITEMS: {
        ReadAheadItems::const_iterator it; // boost::find(std::make_pair(, m_nextLUIDs.end()), luid) - not available on Ubuntu Lucid
        for (it = m_nextLUIDs.begin();
             it != m_nextLUIDs.end() && *it != luid;
             ++it)
            {}

        // Always read the requested item, even if not found in item list.
        luids.push_back(&luid);
        if (it != m_nextLUIDs.end()) {
            found = true;
            ++it;
        }
        while (luids.size() < maxBatchSize &&
               it != m_nextLUIDs.end()) {
            luids.push_back(&*it);
            ++it;
        }
        break;
    }
    case READ_NONE:
        // May be reached when read-ahead was turned off while
        // preparing for it.
        luids.push_back(&luid);
        break;
    }

    boost::shared_ptr<CardDAVCache> cache;
    if (m_readAheadOrder != READ_NONE &&
        !found) {
        // The requested contact was not on our list. Consider this
        // a cache miss (or rather, cache prediction failure) and turn
        // off the read-ahead.
        m_cacheMisses++;
        SE_LOG_DEBUG(getDisplayName(), "reading: disable read-ahead due to cache miss");
        m_readAheadOrder = READ_NONE;
        return cache;
    }

    cache.reset(new CardDAVCache);
    if (!luids.empty()) {
        Timespec deadline = createDeadline();
        m_contactQueries++;
        m_contactsFromDB += luids.size();
        getSession()->startOperation("MULTIGET", deadline);
        while (!luids.empty()) {
            std::stringstream query;

            query <<
                "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
                "<C:addressbook-multiget xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:carddav\">\n"
                "<D:prop>\n"
                "<D:getetag/>\n"
                "<C:address-data/>\n"
                "</D:prop>\n"
                ;
            for (const std::string *luid: luids) {
                query << "<D:href>" << luid2path(*luid) << "</D:href>\n";
            }
            query << "</C:addressbook-multiget>";

            string data;
            Neon::XMLParser parser;
            // This removes all items for which we get data from luids.
            // The purpose of that is two-fold: don't request data again that
            // we already got when resending, and detect missing 404 status errors
            // with Google.
            parser.initReportParser(boost::bind(&CardDAVSource::addItemToCache, this,
                                                cache, boost::ref(luids),
                                                _1, _2, boost::ref(data)));
            parser.pushHandler(boost::bind(Neon::XMLParser::accept, "urn:ietf:params:xml:ns:carddav", "address-data", _2, _3),
                               boost::bind(Neon::XMLParser::append, boost::ref(data), _2, _3));
            std::string request = query.str();
            Neon::Request req(*getSession(), "REPORT", getCalendar().m_path,
                              request, parser);
            req.addHeader("Depth", "0");
            req.addHeader("Content-Type", "application/xml; charset=\"utf-8\"");
            if (req.run()) {
                // CardDAV servers must include a response for each requested item.
                // Google CardDAV doesn't due that at the time of implementing the
                // batched read. As a workaround assume that any remaining item
                // isn't available.
                for (const std::string *luid: luids) {
                    boost::shared_ptr<TransportStatusException> failure(new TransportStatusException(__FILE__,
                                                                                                     __LINE__,
                                                                                                     StringPrintf("%s: not contained in multiget response", luid->c_str()),
                                                                                                     STATUS_NOT_FOUND));
                    (*cache)[*luid] = failure;
                }
                break;
            }
        }
    }
    return cache;
}

void CardDAVSource::addItemToCache(boost::shared_ptr<CardDAVCache> &cache,
                                   BatchLUIDs &luids,
                                   const std::string &href,
                                   const std::string &etag,
                                   std::string &data)
{
    std::string luid = path2luid(href);

    // TODO: error checking
    CardDAVCache::mapped_type result;
    if (!data.empty()) {
        result = data;
        SE_LOG_DEBUG(getDisplayName(), "batch response: got %ld bytes of data for %s",
                     (long)data.size(), luid.c_str());
    } else {
        SE_LOG_DEBUG(getDisplayName(), "batch response: unknown failure for %s",
                     luid.c_str());
    }

    (*cache)[luid] = result;
    bool found = false;
    for (BatchLUIDs::iterator it = luids.begin();
         it != luids.end();
         ++it) {
        if (**it == luid) {
            luids.erase(it);
            found = true;
            break;
        }
    }
    if (!found) {
        SE_LOG_DEBUG(getDisplayName(), "batch response: unexpected item: %s = %s",
                     href.c_str(), luid.c_str());
    }

    // reset data for next item
    data.clear();
}

CardDAVSource::InsertItemResult CardDAVSource::insertItem(const string &luid, const std::string &item, bool raw)
{
    invalidateCachedItem(luid);
    return WebDAVSource::insertItem(luid, item, raw);
}

void CardDAVSource::removeItem(const string &luid)
{
    invalidateCachedItem(luid);
    WebDAVSource::removeItem(luid);
}

void CardDAVSource::setReadAheadOrder(ReadAheadOrder order,
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
    m_cardDAVCache.reset();
}

void CardDAVSource::getReadAheadOrder(ReadAheadOrder &order,
                                      ReadAheadItems &luids)
{
    order = m_readAheadOrder;
    luids = m_nextLUIDs;
}

void CardDAVSource::invalidateCachedItem(const std::string &luid)
{
    if (m_cardDAVCache) {
        CardDAVCache::iterator it = m_cardDAVCache->find(luid);
        if (it != m_cardDAVCache->end()) {
            SE_LOG_DEBUG(getDisplayName(), "reading: remove contact %s from cache because of remove or update", luid.c_str());
            // If we happen to read that contact (unlikely), it'll be
            // considered a cache miss. That's okay. Together with
            // counting cache misses it'll help us avoid using
            // read-ahead when the Synthesis engine is randomly
            // accessing contacts.
            m_cardDAVCache->erase(it);
        }
    }
}

bool CardDAVSource::typeMatches(const StringMap &props) const
{
    StringMap::const_iterator it = props.find("DAV::resourcetype");
    if (it != props.end()) {
        const std::string &type = it->second;
        // allow parameters (no closing bracket)
        // and allow also "carddavaddressbook" (caused by invalid Neon 
        // string concatenation?!)
        if (type.find("<urn:ietf:params:xml:ns:carddav:addressbook") != type.npos ||
            type.find("<urn:ietf:params:xml:ns:carddavaddressbook") != type.npos) {
            return true;
        }
    }
    return false;
}

SE_END_CXX

#endif // ENABLE_DAV
