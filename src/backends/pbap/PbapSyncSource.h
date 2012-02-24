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
SE_BEGIN_CXX

class PbapSession;

class PbapSyncSource : public TrackingSyncSource, private boost::noncopyable
{
  public:
    PbapSyncSource(const SyncSourceParams &params);


 protected:
    /* implementation of SyncSource interface */
    virtual void open();
    virtual bool isEmpty();
    virtual void close();
    virtual Databases getDatabases();
    virtual std::string getMimeType() const;
    virtual std::string getMimeVersion() const;

    /* implementation of TrackingSyncSource interface */
    virtual void listAllItems(RevisionMap_t &revisions);
    //virtual InsertItemResult insertItem(const string &luid, const std::string &item, bool raw);
    virtual InsertItemResult insertItem(const string &luid, const std::string &item, bool raw);
    void readItem(const std::string &luid, std::string &item, bool raw);
    virtual void removeItem(const string &uid);

 private:
    std::auto_ptr<PbapSession> m_session;

    typedef std::map<std::string, std::string> Content;
    Content m_content;
};

SE_END_CXX

#endif // ENABLE_PBAP
#endif // INCL_PBAPSYNCSOURCE
