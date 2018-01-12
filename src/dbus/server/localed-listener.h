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

#ifndef INCL_LOCALED_LISTENER
#define INCL_LOCALED_LISTENER

#include <gdbus-cxx-bridge.h>
#include <boost/signals2.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * The D-Bus binding for http://www.freedesktop.org/wiki/Software/systemd/localed/
 */
class LocaledListener : public GDBusCXX::DBusRemoteObject
{
 public:
    /**
     * Singleton - at most one instance of LocaledListener will exist.
     * It lives as long as one of the create() callers keeps the reference.
     */
    static boost::shared_ptr<LocaledListener> create();

    /**
     * array of var=value, for example LANG, LC_NUMERIC, etc.
     */
    typedef std::vector<std::string> LocaleEnv;

    /**
     * Emitted for each new set of env variables from localed.
     * May or may not be different from what we have already.
     */
    boost::signals2::signal<void (const LocaleEnv &env)> m_localeValues;

    /**
     * The result callback is guaranteed to be invoked once,
     * either with the current settings from localed or, if
     * retrieving those fails, with the current environment.
     */
    void check(const boost::function<void (const LocaleEnv &env)> &result);

    /**
     * Updates current environment to match the one in the parameter.
     * Emits m_localeChanged if and only if something really changed.
     *
     * Not called by default. To ensure that the current environment
     * matches localed, do:
     * - use current settings
     * - m_localeValues -> setLocale
     * - check -> setLocale
     *
     * Alternatively, one could wait until check() completes and only
     * then use the current settings.
     */
    void setLocale(const LocaleEnv &locale);

    typedef boost::signals2::signal<void ()> LocaleChangedSignal;
    /**
     * Emitted by setLocale() only if something really changed in the
     * local environment.
     */
    LocaleChangedSignal m_localeChanged;

 private:
    boost::weak_ptr<LocaledListener> m_self;
    typedef boost::variant<LocaleEnv> LocaleVariant;
    typedef std::map<std::string, LocaleVariant> Properties;
    typedef std::vector<std::string> Invalidated;
    GDBusCXX::SignalWatch<std::string, Properties, Invalidated> m_propertiesChanged;
    GDBusCXX::DBusClientCall<LocaleVariant> m_propertiesGet;

    LocaledListener();
    void onPropertiesChange(const std::string &interface,
                            const Properties &properties,
                            const Invalidated &invalidated);
    typedef boost::function<void (const LocaleEnv &env)> ProcessLocalePropCB_t;
    void processLocaleProperty(const LocaleVariant &locale,
                               const std::string &error,
                               bool mustCall,
                               const ProcessLocalePropCB_t &result);
    void emitLocaleEnv(const LocaleEnv &env);
};

SE_END_CXX

#endif // INCL_LOCALED_LISTENER
