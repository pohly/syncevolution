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

#ifndef GOOGLE_AUTH_SERVICE_H
#define GOOGLE_AUTH_SERVICE_H

#include <gdata/gdata-client-login-authorizer.h>
#include <gdata/gdata-oauth1-authorizer.h>

#include "GoogleException.h"


class XGoogleAuthService : public XGoogle
{
    public:
        XGoogleAuthService (const char *message) :
            XGoogle(message)
            { }
};


class GoogleAuthService
{
        SoupURI *proxyUri;
        gchar *token;
        gchar *token_secret;
        GDataClientLoginAuthorizer *gcla;
        GDataOAuth1Authorizer *goaa;

    public:
        GoogleAuthService (const char *, GType);
        virtual ~GoogleAuthService ();
        void Authenticate (const char *, const char *);
        GDataAuthorizer * authorizer ()
            { return GDATA_AUTHORIZER(gcla); }
};

#endif  // GOOGLE_AUTH_SERVICE_H

