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

#include <config.h>

#ifdef USE_GNOME_KEYRING

extern "C" {
#include <libsecret/secret.h>
}

#include "GNOMEPlatform.h"

#include <syncevo/Exception.h>
#include <syncevo/UserInterface.h>
#include <syncevo/SyncConfig.h>
#include <syncevo/GLibSupport.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * GNOME keyring distinguishes between empty and unset
 * password keys. This function returns nullptr for an
 * empty std::string.
 */
inline const char *passwdStr(const std::string &str)
{
    return str.empty() ? nullptr : str.c_str();
}

static bool UseGNOMEKeyring(const InitStateTri &keyring)
{
    // Disabled by user?
    if (keyring.getValue() == InitStateTri::VALUE_FALSE) {
        return false;
    }

    // If explicitly selected, it must be us.
    if (keyring.getValue() == InitStateTri::VALUE_STRING &&
        !boost::iequals(keyring.get(), "GNOME")) {
        return false;
    }

    // Use GNOME Keyring.
    return true;
}

class LibSecretHash : public GHashTableCXX
{
    std::list<std::string> m_buffer;

public:
    LibSecretHash(const ConfigPasswordKey &key) :
        GHashTableCXX(g_hash_table_new(g_str_hash, g_str_equal), TRANSFER_REF)
    {
        // see https://developer.gnome.org/libsecret/0.16/libsecret-SecretSchema.html#SECRET-SCHEMA-COMPAT-NETWORK:CAPS
        insert("user", key.user);
        insert("domain", key.domain);
        insert("server", key.server);
        insert("object", key.object);
        insert("protocol", key.protocol);
        insert("authtype", key.authtype);
        if (key.port) {
            std::string value = StringPrintf("%d", key.port);
            insert("port", value);
        }
    }

    /** Keys are expected to be constants and not copied. Values are copied. */
    void insert(const char *key, const std::string &value)
    {
        if (!value.empty()) {
            m_buffer.push_back(value);
            g_hash_table_insert(get(),
                                const_cast<char *>(key),
                                const_cast<char *>(m_buffer.back().c_str()));
        }
    }
};

/**
 * Sometimes libsecret and GNOME Keyring fail to initialize
 * the encryption: https://bugzilla.gnome.org/show_bug.cgi?id=778357
 * If that happens, then trying with a new SecretService instance
 * may work.
 *
 * When libsecret detects that, we get a well-defined error.
 * When GNOME Keyring detects it, the error is less obvious.
 */
static bool IsSharedSecretError(const GErrorCXX &gerror)
{
    bool result = gerror.matches(SECRET_ERROR, SECRET_ERROR_PROTOCOL) /* = "received an invalid or unencryptable secret" */ ||
        strstr(gerror->message, "The secret was transferred or encrypted in an invalid way");
    SE_LOG_DEBUG(NULL, "IsSharedSecretError: %d/%d/%s: %s", (int)gerror->domain, (int)gerror->code, gerror->message, result ? "yes" : "no");
    return result;
}

bool GNOMELoadPasswordSlot(const InitStateTri &keyring,
                           const std::string &passwordName,
                           const std::string &descr,
                           const ConfigPasswordKey &key,
                           InitStateString &password)
{
    if (!UseGNOMEKeyring(keyring)) {
        SE_LOG_DEBUG(NULL, "not using GNOME keyring");
        return false;
    }

    LibSecretHash hash(key);
    for (int i = 0; ; i++ ) {
        GErrorCXX gerror;
        PlainGStr result(secret_password_lookupv_sync(SECRET_SCHEMA_COMPAT_NETWORK,
                                                      hash,
                                                      nullptr,
                                                      gerror));

        // if find password stored in gnome keyring
        if (gerror) {
            /* It is uncertain whether we end up here at all when such
               an error occurs. Check just in case. */
            if (IsSharedSecretError(gerror) &&
                i < 3) {
                SE_LOG_DEBUG(NULL, "disconnecting secret service: %u/%d = %s", gerror->domain, gerror->code, gerror->message);
                secret_service_disconnect();
            } else {
                gerror.throwError(SE_HERE, StringPrintf("looking up password '%s'", descr.c_str()));
            }
        } else if (result.get()) {
            SE_LOG_DEBUG(NULL, "%s: loaded password from GNOME keyring using %s",
                         key.description.c_str(),
                         key.toString().c_str());
            password = result;
            break;
        } else {
            /*
             * There have been cases where "received an invalid or
             * unencryptable secret" was printed to the console right
             * before we end up here. Apparently the error doesn't
             * get propagated properly to us.
             *
             * To cope with that, we try to disconnect and check again.
             */
            if (i < 3) {
                SE_LOG_DEBUG(NULL, "disconnecting secret service: password not found");
                secret_service_disconnect();
            } else {
                SE_LOG_DEBUG(NULL, "password not in GNOME keyring using %s",
                             key.toString().c_str());
                break;
            }
        }
    }

    return true;
}

bool GNOMESavePasswordSlot(const InitStateTri &keyring,
                           const std::string &passwordName,
                           const std::string &password,
                           const ConfigPasswordKey &key)
{
    if (!UseGNOMEKeyring(keyring)) {
        SE_LOG_DEBUG(NULL, "not using GNOME keyring");
        return false;
    }

    // Cannot store a password for just a user, that's ambiguous.
    // Also, a password without server ("user=foo") somehow removes
    // the password with server ("user=foo server=bar").
    if (key.user.empty() ||
        (key.domain.empty() && key.server.empty() && key.object.empty())) {
        SE_THROW(StringPrintf("%s: cannot store password in GNOME keyring, not enough attributes (%s). Try setting syncURL or remoteDeviceID if this is a sync password.",
                              key.description.c_str(),
                              key.toString().c_str()));
    }

    LibSecretHash hash(key);
    std::string label;
    if (!key.user.empty() && !key.server.empty()) {
        // This emulates the behavior of libgnomekeyring.
        label = key.user + "@" + key.server;
    } else {
        label = passwordName;
    }
    for (int i = 0; ; i++) {
        GErrorCXX gerror;
        gboolean result = secret_password_storev_sync(SECRET_SCHEMA_COMPAT_NETWORK,
                                                      hash,
                                                      nullptr,
                                                      label.c_str(),
                                                      password.c_str(),
                                                      nullptr,
                                                      gerror);
        if (result) {
            SE_LOG_DEBUG(NULL, "saved password in GNOME keyring using %s", key.toString().c_str());
            break;
        }

        if (IsSharedSecretError(gerror) &&
            i < 3) {
            SE_LOG_DEBUG(NULL, "disconnecting secret service: %u/%d = %s", gerror->domain, gerror->code, gerror->message);
            secret_service_disconnect();
        } else {
            gerror.throwError(SE_HERE, StringPrintf("%s: saving password '%s' in GNOME keyring",
                                                    key.description.c_str(),
                                                    key.toString().c_str()));
        }
    }

    // handled
    return true;
}

SE_END_CXX

#endif // USE_GNOME_KEYRING
