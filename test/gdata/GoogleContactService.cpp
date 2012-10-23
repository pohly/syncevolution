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

#include <cstdio>
#include <string>

#include <gdata/services/contacts/gdata-contacts-query.h>

#include "GoogleContactService.h"


static void _contact_progress (GDataEntry *entry,
                               guint entry_key,
                               guint entry_count,
                               gpointer data)
{
    GoogleContactService *service =
        reinterpret_cast<GoogleContactService *> (data);

    service->Progress();
}


GoogleContactService::GoogleContactService (GoogleAuthService &auth) :
    contacts(NULL)
{
    contacts = gdata_contacts_service_new(auth.authorizer());
    if (!contacts) {
        throw XGoogleContactService(
            "GoogleContactService::GoogleContactService(): gdata_contacts_service_new()");
    }
}



GoogleContactService::~GoogleContactService ()
{
    if (contacts) {
        g_object_unref(contacts);
        contacts = NULL;
    }
}


google_contact_vector_t GoogleContactService::QueryAllContacts ()
{
    GDataContactsQuery *query;
    GDataFeed *feed;
    GDataContactsContact *contact;
    GError *error = NULL;
    GList *list;
    google_contact_vector_t cv;

    query = gdata_contacts_query_new(NULL);
    feed = gdata_contacts_service_query_contacts(contacts,
                                                 GDATA_QUERY(query),
                                                 NULL,
                                                 _contact_progress,
                                                 this,
                                                 &error);
    g_object_unref(query);
    if (error) {
        throw XGoogleContactService(
            (std::string("GoogleContactService::QueryAllContacts(): ") +
            std::string(error->message)).c_str());
    }
    for (list = gdata_feed_get_entries(feed);
         list != NULL;
         list = g_list_next(list)) {
        contact = GDATA_CONTACTS_CONTACT(list->data);

        cv.push_back(google_contact_ptr_t(new GoogleContact(contact)));
    }
    g_object_unref(feed);

    return cv;
}

