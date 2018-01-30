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

#include "goa.h"

#include <syncevo/IdentityProvider.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

static class GOAProvider : public IdentityProvider
{
public:
    GOAProvider() :
        IdentityProvider("goa",
                         "goa:<GOA account presentation ID = email address>\n"
                         "   Authentication using GNOME Online Accounts,\n"
                         "   using an account created and managed with GNOME Control Center.")
    {}

    virtual std::shared_ptr<AuthProvider> create(const InitStateString &username,
                                                   const InitStateString &password)
    {
        // Returning nullptr if not enabled...
        std::shared_ptr<AuthProvider> provider;
#ifdef USE_GOA
        provider = createGOAAuthProvider(username, password);
#endif
        return provider;
    }
} gsso;

SE_END_CXX
