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

#include <syncevo/IdentityProvider.h>

#ifdef USE_SIGNON
#ifdef USE_GSSO
#include "libgsignon-glib/signon-auth-service.h"
#include "libgsignon-glib/signon-identity.h"
#elif defined USE_UOA
#include "libsignon-glib/signon-auth-service.h"
#include "libsignon-glib/signon-identity.h"
#endif // USE_GSSO

#include <syncevo/GLibSupport.h>
#include <syncevo/GVariantSupport.h>

SE_GOBJECT_TYPE(SignonAuthService)
SE_GOBJECT_TYPE(SignonAuthSession)
SE_GOBJECT_TYPE(SignonIdentity)

#endif // USE_SIGNON

#include <syncevo/declarations.h>
SE_BEGIN_CXX

#ifdef USE_SIGNON

class SignonAuthProvider : public AuthProvider
{
    SignonAuthSessionCXX m_authSession;
    GHashTableCXX m_sessionData;
    std::string m_mechanism;
    std::string m_accessToken;
    bool m_invalidateCache;

public:
    SignonAuthProvider(const SignonAuthSessionCXX &authSession,
                       const GHashTableCXX &sessionData,
                       const std::string &mechanism) :
        m_authSession(authSession),
        m_sessionData(sessionData),
        m_mechanism(mechanism),
        m_invalidateCache(false)
    {}

    virtual bool methodIsSupported(AuthMethod method) const { return method == AUTH_METHOD_OAUTH2; }

    virtual Credentials getCredentials() { SE_THROW("only OAuth2 is supported"); }

    virtual std::string getOAuth2Bearer(const PasswordUpdateCallback &passwordUpdateCallback)
    {
        SE_LOG_DEBUG(NULL, "retrieving OAuth2 token");

        if (!m_accessToken.empty() && !m_invalidateCache) {
            return m_accessToken;
        }

        // Retry login if even the refreshed token failed.
        g_hash_table_insert(m_sessionData, g_strdup("ForceTokenRefresh"),
                            g_variant_ref_sink(g_variant_new_boolean(m_invalidateCache)));

        // We get assigned a plain pointer to an instance that we'll own,
        // so we have to use the "steal" variant to enable that assignment.
        GVariantStealCXX resultDataVar;
        GErrorCXX gerror;
        GVariantCXX sessionDataVar(HashTable2Variant(m_sessionData));
        PlainGStr buffer(g_variant_print(sessionDataVar, true));
        SE_LOG_DEBUG(NULL, "asking for OAuth2 token with method %s, mechanism %s and parameters %s",
                     signon_auth_session_get_method(m_authSession),
                     m_mechanism.c_str(),
                     buffer.get());

#define signon_auth_session_process_async_finish signon_auth_session_process_finish
        SYNCEVO_GLIB_CALL_SYNC(resultDataVar, gerror, signon_auth_session_process_async,
                               m_authSession, sessionDataVar, m_mechanism.c_str(), NULL);
        buffer.reset(resultDataVar ? g_variant_print(resultDataVar, true) : NULL);
        SE_LOG_DEBUG(NULL, "OAuth2 token result: %s, %s",
                     buffer.get() ? buffer.get() : "<<null>>",
                     gerror ? gerror->message : "???");
        if (!resultDataVar || gerror) {
            SE_THROW_EXCEPTION_STATUS(StatusException,
                                      StringPrintf("could not obtain OAuth2 token: %s", gerror ? gerror->message : "???"),
                                      STATUS_FORBIDDEN);
        }
        GHashTableCXX resultData(Variant2HashTable(resultDataVar));
        GVariant *tokenVar = static_cast<GVariant *>(g_hash_table_lookup(resultData, (gpointer)"AccessToken"));
        if (!tokenVar) {
            SE_THROW("no AccessToken in OAuth2 response");
        }
        std::string newToken = g_variant_get_string(tokenVar, NULL);
        if (newToken.empty()) {
            SE_THROW("AccessToken did not contain a string value");
        } else if (m_invalidateCache && newToken == m_accessToken) {
            SE_THROW("Got the same invalid AccessToken");
        }
        m_accessToken = newToken;
        return m_accessToken;
    }

    virtual void invalidateCachedSecrets() { m_invalidateCache = true; }

    virtual std::string getUsername() const { return ""; }
};

std::shared_ptr<AuthProvider> createSignonAuthProvider(const InitStateString &username,
                                                         const InitStateString &password)
{
    // Expected content of parameter GVariant.
    std::shared_ptr<GVariantType> hashtype(g_variant_type_new("a{sv}"), g_variant_type_free);

    // 'username' is the part after signon: which we can parse directly.
    GErrorCXX gerror;
    GVariantCXX parametersVar(g_variant_parse(hashtype.get(), username.c_str(), NULL, NULL, gerror),
                              TRANSFER_REF);
    if (!parametersVar) {
        gerror.throwError(SE_HERE, "parsing 'signon:' username");
    }
    GHashTableCXX parameters(Variant2HashTable(parametersVar));

    // Extract the values that we expect in the parameters hash.
    guint32 signonID;
    const char *method;
    const char *mechanism;

    GVariant *value;
    value = (GVariant *)g_hash_table_lookup(parameters, "identity");
    if (!value ||
        !g_variant_type_equal(G_VARIANT_TYPE_UINT32, g_variant_get_type(value))) {
        SE_THROW("need 'identity: <numeric ID>' in 'signon:' parameters");
    }
    signonID = g_variant_get_uint32(value);

    value = (GVariant *)g_hash_table_lookup(parameters, "method");
    if (!value ||
        !g_variant_type_equal(G_VARIANT_TYPE_STRING, g_variant_get_type(value))) {
        SE_THROW("need 'method: <string>' in 'signon:' parameters");
    }
    method = g_variant_get_string(value, NULL);

    value = (GVariant *)g_hash_table_lookup(parameters, "mechanism");
    if (!value ||
        !g_variant_type_equal(G_VARIANT_TYPE_STRING, g_variant_get_type(value))) {
        SE_THROW("need 'mechanism: <string>' in 'signon:' parameters");
    }
    mechanism = g_variant_get_string(value, NULL);

    value = (GVariant *)g_hash_table_lookup(parameters, "session");
    if (!value ||
        !g_variant_type_equal(hashtype.get(), g_variant_get_type(value))) {
        SE_THROW("need 'session: <hash>' in 'signon:' parameters");
    }
    GHashTableCXX sessionData(Variant2HashTable(value));

    SE_LOG_DEBUG(NULL, "using identity %u, method %s, mechanism %s",
                 signonID, method, mechanism);
    SignonIdentityCXX identity(signon_identity_new_from_db(signonID), TRANSFER_REF);
    SE_LOG_DEBUG(NULL, "using signond identity %d", signonID);
    SignonAuthSessionCXX authSession(signon_identity_create_session(identity, method, gerror), TRANSFER_REF);

    auto provider = std::make_shared<SignonAuthProvider>(authSession, sessionData, mechanism);
    return provider;
}

#endif // USE_SIGNON

SE_END_CXX

