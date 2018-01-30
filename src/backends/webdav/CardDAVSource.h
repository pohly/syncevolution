/*
 * Copyright (C) 2010 Intel Corporation
 */

#ifndef INCL_CARDDAVSOURCE
#define INCL_CARDDAVSOURCE

#include <config.h>

#ifdef ENABLE_DAV

#include "WebDAVSource.h"
#include <syncevo/MapSyncSource.h>
#include <syncevo/eds_abi_wrapper.h>
#include <syncevo/SmartPtr.h>

#include <memory>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

class CardDAVCache;

class CardDAVSource : public WebDAVSource,
    public SyncSourceLogging
{
 public:
    CardDAVSource(const SyncSourceParams &params, const std::shared_ptr<SyncEvo::Neon::Settings> &settings);

    /* implementation of SyncSourceSerialize interface */
    virtual std::string getMimeType() const { return "text/vcard"; }
    virtual std::string getMimeVersion() const { return "3.0"; }

    // implementation of SyncSourceLogging callback
    virtual std::string getDescription(const string &luid);

    // implements read-ahead and vCard specific conversions on top of generic WebDAV readItem()
    virtual void readItem(const std::string &luid, std::string &item, bool raw);
    virtual InsertItemResult insertItem(const string &luid, const std::string &item, bool raw);
    virtual void removeItem(const string &luid);

    // Use the information provided to us to implement read-ahead efficiently.
    virtual void setReadAheadOrder(ReadAheadOrder order,
                                   const ReadAheadItems &luids);
    virtual void getReadAheadOrder(ReadAheadOrder &order,
                                   ReadAheadItems &luids);

 protected:
    // implementation of WebDAVSource callbacks
    virtual std::string serviceType() const { return "carddav"; }
    virtual bool typeMatches(const StringMap &props) const;
    virtual std::string homeSetProp() const { return "urn:ietf:params:xml:ns:carddav:addressbook-home-set"; }
    virtual std::string wellKnownURL() const { return "/.well-known/carddav"; }
    virtual std::string contentType() const { return "text/vcard; charset=utf-8"; }
    virtual std::string getContent() const { return "VCARD"; }
    virtual bool getContentMixed() const { return false; }

 private:
    ReadAheadOrder m_readAheadOrder;
    ReadAheadItems m_nextLUIDs;
    std::shared_ptr<CardDAVCache> m_cardDAVCache;
    int m_cacheMisses; /**< number of times that we had to get a contact without using the cache */
    int m_contactReads; /**< number of readItem() calls */
    int m_contactsFromDB; /**< number of contacts requested from DB (including ones not found) */
    int m_contactQueries; /**< total number of GET or multiget REPORT requests */

    typedef std::vector<const std::string *> BatchLUIDs;

    void logCacheStats(Logger::Level level);
    std::shared_ptr<CardDAVCache> readBatch(const std::string &luid);
    void invalidateCachedItem(const std::string &luid);
    void readItemInternal(const std::string &luid, std::string &item, bool raw);
};

SE_END_CXX

#endif // ENABLE_DAV
#endif // INCL_CARDDAVSOURCE
