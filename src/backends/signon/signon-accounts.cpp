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
#include "signon.h"

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
    AgAuthDataCXX m_authData;
    std::string m_accessToken;
    Credentials m_credentials;
    bool m_invalidateCache;

public:
    SignonAuthProvider(const SignonAuthSessionCXX &authSession,
                       const AgAuthDataCXX &authData) :
        m_authSession(authSession),
        m_authData(authData),
        m_invalidateCache(false)
    {}

    virtual bool methodIsSupported(AuthMethod method) const {
        // Unless the method name is "password", let's assume it's OAuth;
        // note that we don't explicitly check if the method name is a
        // OAuth one, because gSSO and UOA use different method names for
        // their OAuth implementations
        AuthMethod ourMethod =
            (strcmp(ag_auth_data_get_method(m_authData), "password") == 0) ?
            AUTH_METHOD_CREDENTIALS : AUTH_METHOD_OAUTH2;
        return method == ourMethod;
    }

    virtual Credentials getCredentials() {
        SE_LOG_DEBUG(NULL, "retrieving password");

        if (!m_credentials.m_password.empty() && !m_invalidateCache) {
            return m_credentials;
        }

        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
        if (m_invalidateCache) {
            // Request the user's password
            g_variant_builder_add(&builder, "{sv}", "UiPolicy",
                                  g_variant_new_uint32(SIGNON_POLICY_REQUEST_PASSWORD));
        }
        GVariantCXX extraOptions(g_variant_take_ref(g_variant_builder_end(&builder)), TRANSFER_REF);

        GVariantCXX resultData = authenticate(extraOptions);
        GVariantCXX usernameVar(g_variant_lookup_value(resultData, "UserName", G_VARIANT_TYPE_STRING), TRANSFER_REF);
        GVariantCXX passwordVar(g_variant_lookup_value(resultData, "Secret", G_VARIANT_TYPE_STRING), TRANSFER_REF);
        if (!usernameVar || !passwordVar) {
            SE_THROW("Username or password missing");
        }
        Credentials credentials;
        credentials.m_username = g_variant_get_string(usernameVar, NULL);
        credentials.m_password = g_variant_get_string(passwordVar, NULL);
        if (credentials.m_password.empty()) {
            SE_THROW("Got an empty password");
        } else if (m_invalidateCache &&
                   credentials.m_password == m_credentials.m_password) {
            SE_THROW("Got the same invalid credentials");
        }
        m_credentials = credentials;
        return m_credentials;
    }

    virtual std::string getOAuth2Bearer(const PasswordUpdateCallback &passwordUpdateCallback)
    {
        SE_LOG_DEBUG(NULL, "retrieving OAuth2 token");

        if (!m_accessToken.empty() && !m_invalidateCache) {
            return m_accessToken;
        }

        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
        if (m_invalidateCache) {
            // Clear any tokens cached in Online Accounts
            g_variant_builder_add(&builder, "{sv}", "ForceTokenRefresh",
                                  g_variant_new_boolean(true));
        }
        GVariantCXX extraOptions(g_variant_take_ref(g_variant_builder_end(&builder)), TRANSFER_REF);

        GVariantCXX resultData = authenticate(extraOptions);
        GVariantCXX tokenVar(g_variant_lookup_value(resultData, "AccessToken", G_VARIANT_TYPE_STRING), TRANSFER_REF);
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

private:
    GVariantCXX authenticate(GVariant *extraOptions) {
        // We get assigned a plain pointer to an instance that we'll own,
        // so we have to use the "steal" variant to enable that assignment.
        GVariantStealCXX resultData;
        GErrorCXX gerror;
        GVariantCXX sessionData(ag_auth_data_get_login_parameters(m_authData, extraOptions), TRANSFER_REF);
        const char *mechanism = ag_auth_data_get_mechanism(m_authData);
        PlainGStr buffer(g_variant_print(sessionData, true));
        SE_LOG_DEBUG(NULL, "asking for authentication with method %s, mechanism %s and parameters %s",
                     signon_auth_session_get_method(m_authSession),
                     mechanism,
                     buffer.get());

#define signon_auth_session_process_async_finish signon_auth_session_process_finish
        SYNCEVO_GLIB_CALL_SYNC(resultData, gerror, signon_auth_session_process_async,
                               m_authSession, sessionData, mechanism, NULL);
        buffer.reset(resultData ? g_variant_print(resultData, true) : NULL);
        SE_LOG_DEBUG(NULL, "authentication result: %s, %s",
                     buffer.get() ? buffer.get() : "<<null>>",
                     gerror ? gerror->message : "???");
        if (!resultData || gerror) {
            SE_THROW_EXCEPTION_STATUS(StatusException,
                                      StringPrintf("could not authenticate: %s", gerror ? gerror->message : "???"),
                                      STATUS_FORBIDDEN);
        }
        return resultData;
    }
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
        SE_THROW(StringPrintf("username must have the format " SE_SIGNON_PROVIDER_ID ":<account ID>,<service name>: %s",
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

    SignonIdentityCXX identity(signon_identity_new_from_db(signonID), TRANSFER_REF);
    SE_LOG_DEBUG(NULL, "using signond identity %d", signonID);
    SignonAuthSessionCXX authSession(signon_identity_create_session(identity, method, gerror), TRANSFER_REF);

    // TODO (?): retrieve start URL from account system

    provider.reset(new SignonAuthProvider(authSession, authData));

    return provider;
}

#endif // USE_SIGNON

SE_END_CXX

