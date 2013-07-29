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

#include <syncevo/IdentityProvider.h>
#include <syncevo/SyncConfig.h>

SE_BEGIN_CXX

const char USER_IDENTITY_PLAIN_TEXT[] = "user";
const char USER_IDENTITY_SYNC_CONFIG[] = "id";

Credentials IdentityProviderCredentials(const UserIdentity &identity,
                                        const InitStateString &password)
{
    Credentials cred;

    if (identity.m_provider == USER_IDENTITY_PLAIN_TEXT) {
        cred.m_username = identity.m_identity;
        cred.m_password = password;
    } else {
        // We could use the gSSO password plugin to request
        // username/password. But it is uncertain whether that is useful,
        // therefore that is not implemented at the moment.
        SE_THROW(StringPrintf("%s: need username+password as credentials", identity.toString().c_str()));
    }

    return cred;
}

SE_END_CXX
