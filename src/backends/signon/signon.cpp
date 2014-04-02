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

#define USE_SIGNON (defined USE_GSSO || defined USE_UOA)

#if USE_SIGNON
#ifdef USE_GSSO
#include "libgsignon-glib/signon-auth-service.h"
#include "libgsignon-glib/signon-identity.h"
#elif defined USE_UOA
#include "libsignon-glib/signon-auth-service.h"
#include "libsignon-glib/signon-identity.h"
#endif // USE_GSSO
#include "libaccounts-glib/ag-account.h"
#include "libaccounts-glib/ag-account-service.h"
#include <libaccounts-glib/ag-auth-data.h>
#include <libaccounts-glib/ag-service.h>
#include <libaccounts-glib/ag-manager.h>

#include <syncevo/GLibSupport.h>
#include <pcrecpp.h>

#include <boost/lambda/core.hpp>

SE_GOBJECT_TYPE(SignonAuthService)
SE_GOBJECT_TYPE(SignonAuthSession)
SE_GOBJECT_TYPE(SignonIdentity)
SE_GOBJECT_TYPE(AgAccount)
SE_GOBJECT_TYPE(AgAccountService)
SE_GOBJECT_TYPE(AgManager)
SE_GLIB_TYPE(AgService, ag_service)
SE_GLIB_TYPE(AgAuthData, ag_auth_data)
SE_GLIB_TYPE(GHashTable, g_hash_table)

#endif // USE_SIGNON

#include <syncevo/declarations.h>
SE_BEGIN_CXX

#ifdef USE_SIGNON

typedef GListCXX<AgService, GList, ag_service_unref> ServiceListCXX;

/**
 * Simple auto_ptr for GVariant.
 */
class GVariantCXX : boost::noncopyable
{
    GVariant *m_var;
 public:
    /** takes over ownership */
    GVariantCXX(GVariant *var = NULL) : m_var(var) {}
    ~GVariantCXX() { if (m_var) { g_variant_unref(m_var); } }

    operator GVariant * () { return m_var; }
    GVariantCXX &operator = (GVariant *var) {
        if (m_var != var) {
            if (m_var) {
                g_variant_unref(m_var);
            }
            m_var = var;
        }
        return *this;
    }
};


// Originally from google-oauth2-example.c, which is also under LGPL
// 2.1, or any later version.
static GVariant *HashTable2Variant(const GHashTable *hashTable) throw ()
{
    GVariantBuilder builder;
    GHashTableIter iter;
    const gchar *key;
    GVariant *value;

    if (!hashTable) {
        return NULL;
    }

    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);

    g_hash_table_iter_init(&iter, (GHashTable *)hashTable);
    while (g_hash_table_iter_next(&iter, (gpointer *)&key, (gpointer *)&value)) {
        g_variant_builder_add(&builder, "{sv}", key, g_variant_ref(value));
    }
    return g_variant_builder_end(&builder);
}

/**
 * The created GHashTable maps strings to GVariants which are
 * reference counted, so when adding or setting values, use g_variant_ref_sink(g_variant_new_...()).
 */
static GHashTable *Variant2HashTable(GVariant *variant) throw ()
{
    if (!variant) {
        return NULL;
    }

    GHashTable *hashTable;
    GVariantIter iter;
    GVariant *value;
    gchar *key;

    hashTable = g_hash_table_new_full(g_str_hash,
                                      g_str_equal,
                                      g_free,
                                      (GDestroyNotify)g_variant_unref);
    g_variant_iter_init(&iter, variant);
    while (g_variant_iter_next(&iter, "{sv}", &key, &value)) {
        g_hash_table_insert(hashTable, g_strdup(key), g_variant_ref(value));
    }
    return hashTable;
}

class SignonAuthProvider : public AuthProvider
{
    SignonAuthSessionCXX m_authSession;
    GHashTableCXX m_sessionData;
    std::string m_mechanism;

public:
    SignonAuthProvider(const SignonAuthSessionCXX &authSession,
                       const GHashTableCXX &sessionData,
                       const std::string &mechanism) :
        m_authSession(authSession),
        m_sessionData(sessionData),
        m_mechanism(mechanism)
    {}

    virtual bool methodIsSupported(AuthMethod method) const { return method == AUTH_METHOD_OAUTH2; }

    virtual Credentials getCredentials() const { SE_THROW("only OAuth2 is supported"); }

    virtual std::string getOAuth2Bearer(int failedTokens) const
    {
        SE_LOG_DEBUG(NULL, "retrieving OAuth2 token, attempt %d", failedTokens);

        // Retry login if even the refreshed token failed.
        g_hash_table_insert(m_sessionData, g_strdup("UiPolicy"),
                            g_variant_ref_sink(g_variant_new_uint32(failedTokens >= 2 ? SIGNON_POLICY_REQUEST_PASSWORD : 0)));
        GVariantCXX resultDataVar;
        GErrorCXX gerror;
        // Enforce normal reference counting via _ref_sink.
        GVariantCXX sessionDataVar(g_variant_ref_sink(HashTable2Variant(m_sessionData)));
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
        GHashTableCXX resultData(Variant2HashTable(resultDataVar), TRANSFER_REF);
        GVariant *tokenVar = static_cast<GVariant *>(g_hash_table_lookup(resultData, (gpointer)"AccessToken"));
        if (!tokenVar) {
            SE_THROW("no AccessToken in OAuth2 response");
        }
        const char *token = g_variant_get_string(tokenVar, NULL);
        if (!token) {
            SE_THROW("AccessToken did not contain a string value");
        }
        return token;
    }

    virtual std::string getUsername() const { return ""; }
};

class StoreIdentityData
{
public:
    StoreIdentityData() : m_running(true) {}

    bool m_running;
    guint32 m_id;
    GErrorCXX m_gerror;
};

static void StoreIdentityCB(SignonIdentity *self,
                            guint32 id,
                            const GError *error,
                            gpointer userData)
{
    StoreIdentityData *data = reinterpret_cast<StoreIdentityData *>(userData);
    data->m_running = false;
    data->m_id = id;
    data->m_gerror = error;
}

boost::shared_ptr<AuthProvider> createSignonAuthProvider(const InitStateString &username,
                                                         const InitStateString &password)
{
    boost::shared_ptr<AuthProvider> provider;

    // Split username into <account ID> and <service name>.
    // Be flexible and allow leading/trailing white space.
    // Comma is optional.
    static const pcrecpp::RE re("^\\s*(\\d+)\\s*,?\\s*(.*)\\s*$");
    AgAccountId accountID;
    std::string serviceName;
    if (!re.FullMatch(username, &accountID, &serviceName)) {
        SE_THROW(StringPrintf("username must have the format gsso:<account ID>,<service name>: %s",
                              username.c_str()));
    }
    SE_LOG_DEBUG(NULL, "looking up account ID %d and service '%s'",
                 accountID,
                 serviceName.c_str());
    AgManagerCXX manager(ag_manager_new(), TRANSFER_REF);
    GErrorCXX gerror;
    AgAccountCXX account(ag_manager_load_account(manager, accountID, gerror), TRANSFER_REF);
    if (!account) {
        gerror.throwError(SE_HERE, StringPrintf("loading account with ID %d from %s failed",
                                       accountID,
                                       username.c_str()));
    }
    if (!ag_account_get_enabled(account)) {
        SE_THROW(StringPrintf("account with ID %d from %s is disabled, refusing to use it",
                              accountID,
                              username.c_str()));
    }
    AgAccountServiceCXX accountService;
    if (serviceName.empty()) {
        accountService = AgAccountServiceCXX::steal(ag_account_service_new(account, NULL));
    } else {
        ServiceListCXX services(ag_account_list_enabled_services(account));
        BOOST_FOREACH (AgService *service, services) {
            const char *name = ag_service_get_name(service);
            SE_LOG_DEBUG(NULL, "enabled service: %s", name);
            if (serviceName == name) {
                accountService = AgAccountServiceCXX::steal(ag_account_service_new(account, service));
                // Do *not* select the service for reading/writing properties.
                // AgAccountService does this internally, and when we create
                // a new identity below, we want it to be shared by all
                // services so that the user only needs to log in once.
                // ag_account_select_service(account, service);
                break;
            }
        }
    }
    if (!accountService) {
        SE_THROW(StringPrintf("service '%s' in account with ID %d not found or not enabled",
                              serviceName.c_str(),
                              accountID));
    }
    AgAuthDataCXX authData(ag_account_service_get_auth_data(accountService), TRANSFER_REF);

    // SignonAuthServiceCXX authService(signon_auth_service_new(), TRANSFER_REF);
    guint signonID = ag_auth_data_get_credentials_id(authData);
    const char *method = ag_auth_data_get_method(authData);
    const char *mechanism = ag_auth_data_get_mechanism(authData);

    // Check that the service has a credentials ID. If not, create it and
    // store its ID permanently.
    if (!signonID) {
        SE_LOG_DEBUG(NULL, "have to create signon identity");
        SignonIdentityCXX identity(signon_identity_new(
#ifdef USE_GSSO
                                                       NULL
#endif
                                                       ), TRANSFER_REF);
        boost::shared_ptr<SignonIdentityInfo> identityInfo(signon_identity_info_new(), signon_identity_info_free);
        signon_identity_info_set_caption(identityInfo.get(),
                                         StringPrintf("created by SyncEvolution for account #%d and service %s",
                                                      accountID,
                                                      serviceName.empty() ? "<<none>>" : serviceName.c_str()).c_str());
        const gchar *mechanisms[] = { mechanism ? mechanism : "*", NULL };
        signon_identity_info_set_method(identityInfo.get(), method, mechanisms);
        StoreIdentityData data;
        signon_identity_store_credentials_with_info(identity, identityInfo.get(),
                                                    StoreIdentityCB, &data);
        GRunWhile(boost::lambda::var(data.m_running));
        if (!data.m_id || data.m_gerror) {
            SE_THROW(StringPrintf("failed to create signon identity: %s",
                                  data.m_gerror ? data.m_gerror->message : "???"));
        }

        // Now store in account.
        static const char CREDENTIALS_ID[] = "CredentialsId";
        ag_account_set_variant(account, CREDENTIALS_ID, g_variant_new_uint32(data.m_id));
#define ag_account_store_async_finish ag_account_store_finish
        gboolean res;
        SYNCEVO_GLIB_CALL_SYNC(res, gerror, ag_account_store_async,
                               account, NULL);
        if (!res) {
            gerror.throwError(SE_HERE, "failed to store account");
        }

        authData = AgAuthDataCXX::steal(ag_account_service_get_auth_data(accountService));
        signonID = ag_auth_data_get_credentials_id(authData);
        if (!signonID) {
            SE_THROW("still no signonID?!");
        }
        method = ag_auth_data_get_method(authData);
        mechanism = ag_auth_data_get_mechanism(authData);
    }

    GVariantCXX sessionDataVar(ag_auth_data_get_login_parameters(authData, NULL));
    GHashTableCXX sessionData(Variant2HashTable(sessionDataVar), TRANSFER_REF);
    SignonIdentityCXX identity(signon_identity_new_from_db(signonID
#ifdef USE_GSSO
                                                           , NULL
#endif
                                                           ), TRANSFER_REF);
    SE_LOG_DEBUG(NULL, "using signond identity %d", signonID);
    SignonAuthSessionCXX authSession(signon_identity_create_session(identity, method, gerror), TRANSFER_REF);

    // TODO (?): retrieve start URL from account system

    provider.reset(new SignonAuthProvider(authSession, sessionData, mechanism));

    return provider;
}

#endif // USE_SIGNON

SE_END_CXX

