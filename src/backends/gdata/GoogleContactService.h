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

#ifndef GOOGLE_CONTACT_SERVICE_H
#define GOOGLE_CONTACT_SERVICE_H

#include <gdata/services/contacts/gdata-contacts-service.h>

#include "GoogleException.h"
#include "GoogleAuthService.h"
#include "GoogleContact.h"


class XGoogleContactService : public XGoogle
{
    public:
        XGoogleContactService (const char *message) :
            XGoogle(message)
            { }
};


class GoogleContactService
{
        GDataContactsService *contacts;

    public:
        GoogleContactService (GoogleAuthService &);
        virtual ~GoogleContactService ();

        google_contact_vector_t QueryAllContacts ();

        virtual void Progress ()
            { }

        static GType ServiceType ()
            { return GDATA_TYPE_CONTACTS_SERVICE; }
};

#endif  // GOOGLE_AUTH_SERVICE_H

