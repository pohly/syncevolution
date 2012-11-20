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


#include "GDataSyncSource.h"


#define GOOGLE_CLIENT_ID        "XXX.apps.googleusercontent.com"


SE_BEGIN_CXX

GDataSyncSource::GDataSyncSource (const SyncSourceParams &params,
                                  int granularitySeconds) :
    TrackingSyncSource(params, granularitySeconds),
    auth(GOOGLE_CLIENT_ID, GoogleContactService::ServiceType),
    service(0)
{
}


GDataSyncSource::~GDataSyncSource ()
{
    if (service) delete service;
}


GDataSyncSource::Databases GDataSyncSource::getDatabases ()
{
    Databases dbs;
    dbs.push_back(Database("", "", true));
    return dbs;
}


void GDataSyncSource::open ()
{
    close();

    auth.Authenticate("osso.rtcom@gmail.com", "ossochavo");
    service = new GoogleContactService(auth);
}


bool GDataSyncSource::isEmpty ()
{
    return contacts.empty();
}


void GDataSyncSource::listAllItems (
                                SyncSourceRevisions::RevisionMap_t &revisions)
{
    google_contact_vector_t list = service->QueryAllContacts();
    for (google_contact_vector_t::iterator iter = list.begin();
         iter != list.end();
         iter++) {
        contacts[iter->id] = *iter;
        revisions[iter->id] = iter->etag;
    }
}


GDataSyncSource::InsertItemResult GDataSyncSource::insertItem (
                                                    const std::string &luid,
                                                    const std::string &item,
                                                    bool raw)
{
    return InsertItemResult();
}


void GDataSyncSource::readItem (const std::string &luid, std::string &item,
                                bool raw)
{
    GContactCache_t::iterator iter = contacts.find(luid);
    if (iter == contacts.end()) return;
    GoogleVCard vcard(iter->second);
    item = vcard.card;
    raw = false;
}


void GDataSyncSource::removeItem (const std::string &luid)
{
    GContactCache_t::iterator iter = contacts.find(luid);
    if (iter == contacts.end()) return;
    contacts.erase(iter);
}


void GDataSyncSource::close ()
{
    if (service) {
        delete service;
        service = 0;
    }
    contacts.clear();
}


std::string GDataSyncSource::getMimeType ()
{
    return "text/vcard";
}


std::string GDataSynCsource::getMimeVersion ()
{
    return "4.0";
}

SE_END_CXX

