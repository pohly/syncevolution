/*
 * Copyright (C) 2013 Intel Corporation
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

#include <config.h>

#include "signon.h"

#include <syncevo/IdentityProvider.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

#if defined(USE_GSSO) || defined(USE_UOA) || defined(USE_SIGNON) || defined(STATIC_GSSO) || defined(STATIC_UOA) || defined(STATIC_SIGNON)
static class SignonProvider : public IdentityProvider
{
public:
    SignonProvider() :
        // This uses "gsso" at the moment. The advantage of that is that if
        // gSSO and UOA were installed in parallel, the user could choose which
        // one to use. If it turns out that the two will never be installed at the
        // same time, then this perhaps should be "signon" instead, which then would
        // pick either a gSSO or UAO backend depending on which is available.
#if defined(USE_ACCOUNTS) && defined(USE_GSSO) || defined(STATIC_GSSO)
        IdentityProvider(SE_SIGNON_PROVIDER_ID,
                         SE_SIGNON_PROVIDER_ID ":<numeric account ID>[,<service name>]\n"
                         "   Authentication using libgsignond + libaccounts,\n"
                         "   using an account created and managed with libaccounts.\n"
                         "   The service name is optional. If not given, the\n"
                         "   settings from the account will be used.")
#elif defined(USE_ACCOUNTS) && defined(USE_UOA) || defined(STATIC_UOA)
        IdentityProvider(SE_SIGNON_PROVIDER_ID,
                         SE_SIGNON_PROVIDER_ID ":<numeric account ID>[,<service name>]\n"
                         "   Authentication using libsignon + libaccounts,\n"
                         "   using an account created and managed with libaccounts.\n"
                         "   The service name is optional. If not given, the\n"
                         "   settings from the account will be used.")
#elif defined(USE_SIGNON) || defined(STATIC_SIGNON)
        IdentityProvider(SE_SIGNON_PROVIDER_ID,
                         SE_SIGNON_PROVIDER_ID ":<parameters>]\n"
                         "   Authentication using libgsignond with an identity created\n"
                         "   before calling SyncEvolution. The <parameters> string is a\n"
                         "   GVariant text dump suitable for g_variant_parse() (see\n"
                         "   https://developer.gnome.org/glib/stable/gvariant-text.html).\n"
                         "   It must contain a hash with keys 'identity', 'method', \n"
                         "   'session' and 'mechanism'. The first two values are used for\n"
                         "   signon_identity_create_session(), the last one for\n"
                         "   signon_auth_session_process_async().\n")
#endif
    {}

    virtual std::shared_ptr<AuthProvider> create(const InitStateString &username,
                                                   const InitStateString &password)
    {
        std::shared_ptr<AuthProvider> provider;
        provider = createSignonAuthProvider(username, password);
        return provider;
    }
} gsso;

#endif // one signon-based provider enabled

SE_END_CXX
