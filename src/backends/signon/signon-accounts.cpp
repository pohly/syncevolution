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
#include "libaccounts-glib/ag-account.h"
#include "libaccounts-glib/ag-account-service.h"
#include <libaccounts-glib/ag-auth-data.h>
#include <libaccounts-glib/ag-service.h>
#include <libaccounts-glib/ag-manager.h>

#include <syncevo/GLibSupport.h>
#include <syncevo/GVariantSupport.h>
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

#endif // USE_SIGNON

#include <syncevo/declarations.h>
SE_BEGIN_CXX

#ifdef USE_SIGNON

typedef GListCXX<AgService, GList, ag_service_unref> ServiceListCXX;

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

        if (m_invalidateCache) {
            // Retry login if even the refreshed token failed.
            g_hash_table_insert(m_sessionData, g_strdup("ForceTokenRefresh"),
                                g_variant_ref_sink(g_variant_new_boolean(true)));
        }

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

    GVariantCXX sessionDataVar(ag_auth_data_get_login_parameters(authData, NULL), TRANSFER_REF);
    GHashTableCXX sessionData(Variant2HashTable(sessionDataVar));

    SignonIdentityCXX identity(signon_identity_new_from_db(signonID), TRANSFER_REF);
    SE_LOG_DEBUG(NULL, "using signond identity %d", signonID);
    SignonAuthSessionCXX authSession(signon_identity_create_session(identity, method, gerror), TRANSFER_REF);

    // TODO (?): retrieve start URL from account system

    provider.reset(new SignonAuthProvider(authSession, sessionData, mechanism));

    return provider;
}

#endif // USE_SIGNON

SE_END_CXX

