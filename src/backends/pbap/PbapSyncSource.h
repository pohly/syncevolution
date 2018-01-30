/*
 * Copyright (C) 2007-2009 Patrick Ohly <patrick.ohly@gmx.de>
 * Copyright (C) 2012 BMW Car IT GmbH. All rights reserved.
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

#ifndef INCL_PBAPSYNCSOURCE
#define INCL_PBAPSYNCSOURCE

#include <syncevo/TrackingSyncSource.h>

#ifdef ENABLE_PBAP

#include <memory>
#include <boost/noncopyable.hpp>

#include <syncevo/declarations.h>
#include <syncevo/TmpFile.h>
SE_BEGIN_CXX

class PullAll;
class PbapSession;

class PbapSyncSource : virtual public SyncSource, virtual public SyncSourceSession, virtual public SyncSourceRaw, private boost::noncopyable
{
  public:
    PbapSyncSource(const SyncSourceParams &params);
    ~PbapSyncSource();

 protected:
    /* implementation of SyncSource interface */
    virtual void open();
    virtual bool isEmpty();
    virtual void close();
    virtual void setFreeze(bool freeze);
    virtual Databases getDatabases();
    virtual void enableServerMode();
    virtual bool serverModeEnabled() const;
    virtual std::string getPeerMimeType() const;
    virtual void getSynthesisInfo(SynthesisInfo &info,
                                  XMLConfigFragments &fragments);

    /* implementation of SyncSourceSession interface */
    virtual void beginSync(const std::string &lastToken, const std::string &resumeToken);
    virtual std::string endSync(bool success);

    /* implementation of SyncSourceRaw interface */
    virtual InsertItemResult insertItemRaw(const std::string &luid, const std::string &item);
    virtual void readItemRaw(const std::string &luid, std::string &item);

 private:
    std::shared_ptr<PbapSession> m_session;
    std::shared_ptr<PullAll> m_pullAll;
    enum PBAPSyncMode {
        PBAP_SYNC_NORMAL,      ///< Read contact data according to filter.
        PBAP_SYNC_TEXT,        ///< Sync without reading photo data from phone and keeping local photos instead.
        PBAP_SYNC_INCREMENTAL  ///< Sync first without photo data (as in PBAP_SYNC_TEXT),
                               ///  then add photo data in second cycle.
    } m_PBAPSyncMode;
    bool m_isFirstCycle;
    bool m_hadContacts;

    /**
     * List items as expected by Synthesis engine.
     */
    sysync::TSyError readNextItem(sysync::ItemID aID,
                                  sysync::sInt32 *aStatus,
                                  bool aFirst);

    /**
     * Copy item into Synthesis key.
     */
    sysync::TSyError readItemAsKey(sysync::cItemID aID, sysync::KeyH aItemKey);
};

SE_END_CXX

#endif // ENABLE_PBAP
#endif // INCL_PBAPSYNCSOURCE
