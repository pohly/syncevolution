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
#include <gnome-keyring.h>
}

#include "GNOMEPlatform.h"

#include <syncevo/Exception.h>
#include <syncevo/UserInterface.h>
#include <syncevo/SyncConfig.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

// Occasionally, libgnome-keyring fails with the following error messages:
// Gkr: received an invalid, unencryptable, or non-utf8 secret
// Gkr: call to daemon returned an invalid response: (null).(null)
//
// We work around that by retrying the operation a few times, for at
// most this period of time. Didn't really help, so disable it for now
// by using a zero duration.
static const double GNOMEKeyringRetryDuration = 2; // seconds
static const double GNOMEKeyringRetryInterval = 0.1; // seconds

/**
 * libgnome-keyring has an internal gkr_reset_session()
 * method which gets called when the "org.freedesktop.secrets"
 * disconnects from the D-Bus session bus.
 *
 * We cannot call that method directly, but we can get it called by
 * faking the "disconnect" signal. That works because
 * on_connection_filter() in gkr-operation.c doesn't check who the
 * sender of the signal is.
 *
 * Once gkr_reset_session() got called, the next operation will
 * re-establish the connection. After the failure above, the second
 * attempt usually works.
 *
 * Any other client using libgnome-keyring will also be tricked into
 * disconnecting temporarily. That should be fine, any running
 * operation will continue to run and complete (?).
 */
static void FlushGNOMEKeyring()
{
    // Invoking dbus-send is easier than writing this in C++.
    // Besides, it ensures that the signal comes from some other
    // process. Not sure whether signals are sent back to the sender.
    system("dbus-send --session --type=signal /org/freedesktop/DBus org.freedesktop.DBus.NameOwnerChanged string:'org.freedesktop.secrets' string:':9.99' string:''");
}

/**
 * GNOME keyring distinguishes between empty and unset
 * password keys. This function returns NULL for an
 * empty std::string.
 */
inline const char *passwdStr(const std::string &str)
{
    return str.empty() ? NULL : str.c_str();
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

    GnomeKeyringResult result = GNOME_KEYRING_RESULT_OK;
    GList* list;
    Timespec start = Timespec::monotonic();
    double sleepSecs = 0;
    do {
        if (sleepSecs != 0) {
            SE_LOG_DEBUG(NULL, "%s: previous attempt to load password '%s' from GNOME keyring failed, will try again: %s",
                         key.description.c_str(),
                         key.toString().c_str(),
                         gnome_keyring_result_to_message(result));
            FlushGNOMEKeyring();
            Sleep(sleepSecs);
        }
        result = gnome_keyring_find_network_password_sync(passwdStr(key.user),
                                                          passwdStr(key.domain),
                                                          passwdStr(key.server),
                                                          passwdStr(key.object),
                                                          passwdStr(key.protocol),
                                                          passwdStr(key.authtype),
                                                          key.port,
                                                          &list);
        sleepSecs = GNOMEKeyringRetryInterval;
    } while (result != GNOME_KEYRING_RESULT_OK &&
             (Timespec::monotonic() - start).duration() < GNOMEKeyringRetryDuration);

    // if find password stored in gnome keyring
    if(result == GNOME_KEYRING_RESULT_OK && list && list->data ) {
        GnomeKeyringNetworkPasswordData *key_data;
        key_data = (GnomeKeyringNetworkPasswordData*)list->data;
        password = std::string(key_data->password);
        gnome_keyring_network_password_list_free(list);
        SE_LOG_DEBUG(NULL, "%s: loaded password from GNOME keyring using %s",
                     key.description.c_str(),
                     key.toString().c_str());
    } else {
        SE_LOG_DEBUG(NULL, "password not in GNOME keyring using %s: %s",
                     key.toString().c_str(),
                     result == GNOME_KEYRING_RESULT_NO_MATCH ? "no match" :
                     result != GNOME_KEYRING_RESULT_OK ? gnome_keyring_result_to_message(result) :
                     "empty result list");
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

    guint32 itemId;
    GnomeKeyringResult result = GNOME_KEYRING_RESULT_OK;
    // write password to keyring
    Timespec start = Timespec::monotonic();
    double sleepSecs = 0;
    do {
        if (sleepSecs != 0) {
            SE_LOG_DEBUG(NULL, "%s: previous attempt to save password '%s' in GNOME keyring failed, will try again: %s",
                         key.description.c_str(),
                         key.toString().c_str(),
                         gnome_keyring_result_to_message(result));
            FlushGNOMEKeyring();
            Sleep(sleepSecs);
        }
        result = gnome_keyring_set_network_password_sync(NULL,
                                                         passwdStr(key.user),
                                                         passwdStr(key.domain),
                                                         passwdStr(key.server),
                                                         passwdStr(key.object),
                                                         passwdStr(key.protocol),
                                                         passwdStr(key.authtype),
                                                         key.port,
                                                         password.c_str(),
                                                         &itemId);
        sleepSecs = GNOMEKeyringRetryInterval;
    } while (result != GNOME_KEYRING_RESULT_OK &&
             (Timespec::monotonic() - start).duration() < GNOMEKeyringRetryDuration);
    if (result != GNOME_KEYRING_RESULT_OK) {
        Exception::throwError(SE_HERE, StringPrintf("%s: saving password '%s' in GNOME keyring failed: %s",
                                             key.description.c_str(),
                                             key.toString().c_str(),
                                             gnome_keyring_result_to_message(result)));
    }
    SE_LOG_DEBUG(NULL, "saved password in GNOME keyring using %s", key.toString().c_str());

    // handled
    return true;
}

SE_END_CXX

#endif // USE_GNOME_KEYRING
