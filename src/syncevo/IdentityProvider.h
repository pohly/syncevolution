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
#ifndef INCL_SYNC_EVOLUTION_IDENTITY_PROVIDER
# define INCL_SYNC_EVOLUTION_IDENTITY_PROVIDER

#include <syncevo/util.h>

#include <string>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

extern const char USER_IDENTITY_PLAIN_TEXT[];
extern const char USER_IDENTITY_SYNC_CONFIG[];

struct UserIdentity; // from SyncConfig.h

struct Credentials
{
    std::string m_username;
    std::string m_password;
};

/**
 * Returns username/password for an identity. The password is the
 * string configured for it inside SyncEvolution. It may be empty and/or unset if
 * the plain text password comes from the identity provider.
 *
 * If the credentials cannot be retrieved, an error is thrown, so don't use this
 * in cases where a different authentication method might also work.
 */
Credentials IdentityProviderCredentials(const UserIdentity &identity,
                                        const InitStateString &password);

/**
 * Supports multiple different ways of authorizing the user.
 * Actual implementations are IdentityProvider specific.
 */
class AuthProvider
{
 public:
    /**
     * Creates an AuthProvider matching the identity.m_provider value
     * or throws an exception if that fails. Never returns NULL.
     */
    static boost::shared_ptr<AuthProvider> create(const UserIdentity &identity,
                                                  const InitStateString &password);

    enum AuthMethod {
        AUTH_METHOD_NONE,
        AUTH_METHOD_CREDENTIALS,
        AUTH_METHOD_OAUTH2,
        AUTH_METHOD_MAX
    };

    /**
     * Returns true if the given method is supported and currently possible.
     */
    virtual bool methodIsSupported(AuthMethod method) const = 0;

    /**
     * Returns username/password credentials. Throws an error if not supported.
     */
    virtual Credentials getCredentials() const = 0;

    /**
     * Returns the 'Bearer b64token' string required for logging into
     * services supporting OAuth2 or throws an exception when we don't
     * have a valid token. Internally this will refresh tokens
     * automatically.
     *
     * See http://tools.ietf.org/html/draft-ietf-oauth-v2-bearer-20#section-2.1
     */
    virtual std::string getOAuth2Bearer() const = 0;

    /**
     * Returns username at the remote service. Works for both
     * username/password credentials and OAuth2.
     */
    virtual std::string getUsername() const = 0;
};


SE_END_CXX
#endif
