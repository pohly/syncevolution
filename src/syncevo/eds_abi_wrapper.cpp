/*
 * Copyright (C) 2008-2009 Patrick Ohly <patrick.ohly@gmx.de>
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

#define EDS_ABI_WRAPPER_NO_REDEFINE 1
#include <syncevo/eds_abi_wrapper.h>
#include <syncevo/util.h>

#include <string>
#include <sstream>
#include <dlfcn.h>
#include <stdarg.h>

#include <syncevo/declarations.h>
namespace {
static std::string &getLookupDebug() { static std::string lookupDebug; return lookupDebug; }
static std::string &getLookupInfo() { static std::string lookupInfo; return lookupInfo; }
}

extern "C" void EDSAbiWrapperInit()
{
    static bool initialized;

    if (initialized) {
        return;
    } else {
        initialized = true;
    }

    // TODO: remove?
#if 0 and defined(ENABLE_ICALTZ_UTIL)
    static void *icaltzutil = dlopen("libsyncevo-icaltz-util.so.0", RTLD_LAZY|RTLD_GLOBAL);
    if (icaltzutil) {
        // Bind icaltimezone_get_component() and
        // icaltzutil_fetch_timezone() to the version found (or not found,
        // if not enabled) in our own libsyncevo-icaltz-util.so.1. Without this, the
        // dynamic linker on Ubuntu Saucy and Trusty prefers the versions
        // from libical once it is loaded.
        void *fetch = dlsym(RTLD_DEFAULT, "icaltzutil_fetch_timezone"),
            *fetch_lib = dlsym(icaltzutil, "icaltzutil_fetch_timezone"),
            *get = dlsym(RTLD_DEFAULT, "icaltimezone_get_component"),
            *get_lib = dlsym(icaltzutil, "icaltimezone_get_component");
        getLookupInfo() += "libsyncevo-icaltz-util.so.0 + libical.so.1\n";
        getLookupDebug() += SyncEvo::StringPrintf("icaltzutil_fetch_timezone = %p (default), %p (SyncEvolution)\n", fetch, fetch_lib);
        getLookupDebug() += SyncEvo::StringPrintf("icaltimezone_get_component = %p (default), %p (SyncEvolution)\n", get, get_lib);
    }
#endif // ENABLE_ICALTZ_UTIL
}

extern "C" const char *EDSAbiWrapperInfo() { EDSAbiWrapperInit(); return getLookupInfo().c_str(); }
extern "C" const char *EDSAbiWrapperDebug() { EDSAbiWrapperInit(); return getLookupDebug().c_str(); }

#ifdef ENABLE_DBUS_TIMEOUT_HACK
/**
 * There are valid use cases where the (previously hard-coded) default
 * timeout was too short. For example, libecal and libebook >= 2.30 
 * implement their synchronous API with synchronous D-Bus method calls,
 * which inevitably suffers from timeouts on slow hardware with large
 * amount of data (MBC #4026).
 *
 * This function replaces _DBUS_DEFAULT_TIMEOUT_VALUE and - if set -
 * interprets the content of SYNCEVOLUTION_DBUS_TIMEOUT as number of
 * milliseconds. 0 disables timeouts, which is also the default if the
 * env variable is not set.
 */
static int _dbus_connection_default_timeout(void)
{
    const char *def = getenv("SYNCEVOLUTION_DBUS_TIMEOUT");
    int timeout = 0;

    if (def) {
        timeout = atoi(def);
    }
    if (timeout == 0) {
        timeout = INT_MAX - 1; // not infinite, but very long;
                               // INT_MAX led to a valgrind report in poll()/libdbus,
                               // avoid it
    }
    return timeout;
}

extern "C" int
dbus_connection_send_with_reply (void *connection,
                                 void *message,
                                 void **pending_return,
                                 int timeout_milliseconds)
{
    static decltype(dbus_connection_send_with_reply) *real_func;

    if (!real_func) {
        real_func = (decltype(dbus_connection_send_with_reply) *)dlsym(RTLD_NEXT, "dbus_connection_send_with_reply");
    }
    return real_func ?
        real_func(connection, message, pending_return,
                  timeout_milliseconds == -1 ? _dbus_connection_default_timeout() : timeout_milliseconds) :
        0;
}
#endif // ENABLE_DBUS_TIMEOUT_HACK
