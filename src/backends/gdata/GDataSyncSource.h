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


#ifndef INCL_GDATASYNCSOURCE
#define INCL_GDATASYNCSUORCE

#include <string>
#include <map>

#include <config.h>
#include <syncevo/TrackingSyncSource.h>

#include "GoogleAuthService.h"
#include "GoogleContactService.h"
#include "GoogleContact.h"
#include "GoogleVCard.h"


SE_BEGIN_CXX

class GDataSyncSource : public TrackingSyncSource
{
    protected:
        typedef std::map<std::string, google_contact_ptr_t> GContactCache_t;

        GoogleAuthService auth;
        GoogleContactService *service;
        GContactCache_t contacts;

    public:
        GDataSyncSource (const SyncSourceParams &params,
                         int granularitySeconds = 1);
        virtual ~GDataSyncSource ();

        virtual Databases getDatabases ();
        virtual void open ();
        virtual bool isEmpty ();
        virtual void listAllItems (
                                SyncSourceRevisions::RevisionMap_t &revisions);
        virtual InsertItemResult insertItem (const std::string &luid,
                                             const std::string &item,
                                             bool raw);
        virtual void readItem (const std::string &luid, std::string &item,
                               bool raw);
        virtual void removeItem (const std::string &luid);
        virtual void close ();
        virtual std::string getMimeType () const;
        virtual std::string getMimeVersion () const;

};

SE_END_CXX

#endif  // INCL_GDATASYNCSOURCE

