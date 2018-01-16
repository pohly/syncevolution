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

#include "localed-listener.h"
#include <syncevo/BoostHelper.h>
#include <syncevo/Logging.h>
#include <syncevo/util.h>

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <algorithm>

SE_BEGIN_CXX

static const char LOCALED_PATH[] = "/org/freedesktop/locale1";
static const char LOCALED_INTERFACE[] = "org.freedesktop.locale1";
static const char LOCALED_DESTINATION[] = "org.freedesktop.locale1";
static const char LOCALED_LOCALE_PROPERTY[] = "Locale";

/**
 * Must be a complete list, because we need to know which variables
 * we have to unset if not set remotely.
 *
 * Localed intentionally does not support LC_ALL. As localed.c says:
 * "We don't list LC_ALL here on purpose. People should be using LANG instead."
 */
static const char * const LOCALED_ENV_VARS[] = {
    "LANG",
    "LC_CTYPE",
    "LC_NUMERIC",
    "LC_TIME",
    "LC_COLLATE",
    "LC_MONETARY",
    "LC_MESSAGES",
    "LC_PAPER",
    "LC_NAME",
    "LC_ADDRESS",
    "LC_TELEPHONE",
    "LC_MEASUREMENT",
    "LC_IDENTIFICATION"
};

static const char PROPERTIES_INTERFACE[] = "org.freedesktop.DBus.Properties";
static const char PROPERTIES_CHANGED_SIGNAL[] = "PropertiesChanged";
static const char PROPERTIES_GET[] = "Get";

LocaledListener::LocaledListener():
    GDBusCXX::DBusRemoteObject(!strcmp(getEnv("SYNCEVOLUTION_LOCALED", ""), "none") ?
                               NULL : /* simulate missing localed */
                               GDBusCXX::dbus_get_bus_connection(!strcmp(getEnv("SYNCEVOLUTION_LOCALED", ""), "session") ?
                                                                 "SESSION" : /* use our own localed stub */
                                                                 "SYSTEM" /* use real localed */,
                                                                 NULL, false, NULL),
                               LOCALED_PATH,
                               PROPERTIES_INTERFACE,
                               LOCALED_DESTINATION),
    m_propertiesChanged(*this, PROPERTIES_CHANGED_SIGNAL),
    m_propertiesGet(*this, PROPERTIES_GET)
{
    if (getConnection()) {
        auto change = [this] (const std::string &interface,
                              const Properties &properties,
                              const Invalidated &invalidated) {
            onPropertiesChange(interface, properties, invalidated);
        };
        m_propertiesChanged.activate(change);
    } else {
        SE_LOG_DEBUG(NULL, "localed: not activating, no connection");
    }
};


std::shared_ptr<LocaledListener> LocaledListener::create()
{
    static std::weak_ptr<LocaledListener> singleton;
    std::shared_ptr<LocaledListener> self = singleton.lock();
    if (!self) {
        self.reset(new LocaledListener());
        singleton = self;
    }
    return self;
};

void LocaledListener::onPropertiesChange(const std::string &interface,
                                         const Properties &properties,
                                         const Invalidated &invalidated)
{
    if (interface == LOCALED_INTERFACE) {
        auto result = [self=weak_from_this()] (const LocaleEnv &env) {
            auto lock = self.lock();
            if (lock) {
                lock->emitLocaleEnv(env);
            }
        };
        for (const auto &entry: properties) {
            if (entry.first == LOCALED_LOCALE_PROPERTY) {
                const LocaleEnv *locale = boost::get<LocaleEnv>(&entry.second);
                if (locale) {
                    SE_LOG_DEBUG(NULL, "localed: got new Locale");
                    processLocaleProperty(*locale, "", false, result);
                } else {
                    SE_LOG_DEBUG(NULL, "localed: got new Locale of invalid type?! Ignore.");
                }
                return;
            }
        }
        if (std::find(invalidated.begin(),
                      invalidated.end(),
                      LOCALED_LOCALE_PROPERTY) != invalidated.end()) {
            SE_LOG_DEBUG(NULL, "localed: Locale changed, need to get new value");
            m_propertiesGet.start([self=weak_from_this(), result] (const LocaleVariant &variant, const std::string &error) {
                    auto lock = self.lock();
                    if (lock) {
                        lock->processLocaleProperty(variant, error, false, result);
                    }
                },
                std::string(LOCALED_INTERFACE),
                std::string(LOCALED_LOCALE_PROPERTY));
        }
        SE_LOG_DEBUG(NULL, "localed: ignoring irrelevant property change");
    }
}

void LocaledListener::processLocaleProperty(const LocaleVariant &variant,
                                            const std::string &error,
                                            bool mustCall,
                                            const ProcessLocalePropCB_t &result)
{
    SE_LOG_DEBUG(NULL, "localed: got Locale property: %s", error.empty() ? "<<successfully>>" : error.c_str());
    const LocaleEnv *locale =
        error.empty() ?
        boost::get<LocaleEnv>(&variant) :
        NULL;
    LocaleEnv current;
    if (!locale && mustCall) {
        SE_LOG_DEBUG(NULL, "localed: using current environment as fallback");
        for (const char *name: LOCALED_ENV_VARS) {
            const char *value = getenv(name);
            if (value) {
                current.push_back(StringPrintf("%s=%s", name, value));
            }
        }
        locale = &current;
    }
    if (locale) {
        result(*locale);
    }
}

void LocaledListener::emitLocaleEnv(const LocaleEnv &env)
{
    SE_LOG_DEBUG(NULL, "localed: got environment: %s",
                 boost::join(env, " ").c_str());
    m_localeValues(env);
}

void LocaledListener::check(const std::function<void (const LocaleEnv &env)> &result)
{
    if (getConnection()) {
        SE_LOG_DEBUG(NULL, "localed: get current Locale property");
        m_propertiesGet.start([self=weak_from_this(), result] (const LocaleVariant &variant, const std::string &error) {
                    auto lock = self.lock();
                    if (lock) {
                        lock->processLocaleProperty(variant, error, true, result);
                    }
            },
            std::string(LOCALED_INTERFACE),
            std::string(LOCALED_LOCALE_PROPERTY));
    } else {
        processLocaleProperty(LocaleVariant(), "no D-Bus connection", true, result);
    }
}

void LocaledListener::setLocale(const LocaleEnv &locale)
{
    bool modified = false;
    for (const char *name: LOCALED_ENV_VARS) {
        const char *value = getenv(name);
        std::string assignment = StringPrintf("%s=", name);
        auto instance = std::find_if(locale.begin(), locale.end(),
                                     [&name] (const std::string &l) { return boost::starts_with(l, name); });
        const char *newvalue = instance != locale.end() ? instance->c_str() + assignment.size() : NULL;
        if ((value && newvalue && strcmp(value, newvalue)) ||
            (!value && newvalue)) {
            modified = true;
            setenv(name, newvalue, true);
            SE_LOG_DEBUG(NULL, "localed: %s = %s -> %s", name, value ? value : "<none>", newvalue);
        } else if (value && !newvalue) {
            modified = true;
            unsetenv(name);
            SE_LOG_DEBUG(NULL, "localed: %s = %s -> <none>", name, value);
        }
    }
    SE_LOG_DEBUG(NULL, "localed: environment %s", modified ? "changed" : "unchanged");
    if (modified) {
        m_localeChanged();
    }
}

SE_END_CXX
