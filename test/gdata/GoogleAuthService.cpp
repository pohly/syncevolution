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
#include <cstdlib>
#include <string>

#include "GoogleAuthService.h"


GoogleAuthService::GoogleAuthService (const char *client_id,
                                      GType service_type) :
    proxyUri(NULL),
    token(NULL),
    token_secret(NULL),
    gcla(NULL),
    goaa(NULL)
{
    char *proxy;
    proxy = getenv("http_proxy");
    if (!proxy) {
        proxy = getenv("HTTP_PROXY");
    }
    if (proxy) {
        proxyUri = soup_uri_new(proxy);
    }

    gcla = gdata_client_login_authorizer_new(client_id, service_type);
    if (gcla == NULL) {
        throw XGoogleAuthService("gdata_client_login_authrizer_new()");
    }
    goaa = gdata_oauth1_authorizer_new("syncEvolution gdata",
                                       service_type);
    if (goaa == NULL) {
        throw XGoogleAuthService("gdata_oauth1_authorizer_new()");
    }

    if (proxyUri) {
        gdata_client_login_authorizer_set_proxy_uri(gcla, proxyUri);
        gdata_oauth1_authorizer_set_proxy_uri(goaa, proxyUri);
    }
}


GoogleAuthService::~GoogleAuthService ()
{
    if (token) {
        g_free(token);
        token = NULL;
    }
    if (token_secret) {
        g_free(token_secret);
        token_secret = NULL;
    }
    if (goaa) {
        g_object_unref(goaa);
        goaa = NULL;
    }
    if (gcla) {
        g_object_unref(gcla);
        gcla = NULL;
    }
    if (proxyUri) {
        soup_uri_free(proxyUri);
        proxyUri = NULL;
    }
}


void GoogleAuthService::Authenticate (const char *username,
                                      const char *password)
{
    //gchar *uri;
    GError *error = NULL;

    if (!gdata_client_login_authorizer_authenticate(gcla,
                                                    username,
                                                    password,
                                                    NULL,
                                                    &error)) {
        throw XGoogleAuthService(
            (std::string("GoogleAuthService::Authenticate(): ") +
             std::string(error->message)).c_str());
    }
    /*uri = gdata_oauth1_authorizer_request_authentication_uri(goaa,
                                                             &token,
                                                             &token_secret,
                                                             NULL,
                                                             &error);
    if (error) {
        throw XGoogleAuthService(
            (std::string("GoogleAuthService::Authenticate(): ") +
             std::string(error->message)).c_str());
    }
    printf("launch browser for uri: %s\n", uri);
    system((std::string("xdg-open ") + std::string(uri)).c_str());
    g_free(uri);
    if (!gdata_oauth1_authorizer_request_authorization(goaa,
                                                       token,
                                                       token_secret,
                                                       NULL, //verifier
                                                       NULL,
                                                       &error)) {
        if (error) {
            throw XGoogleAuthService(
                (std::string("GoogleAuthService::Authenticate(): ") +
                 std::string(error->message)).c_str());
        }
        else {
            throw XGoogleAuthService(
                "GoogleAuthService::Authenticate(): gdata_oauth1_authorizer_request_authorization()");
        }
    }*/
}

