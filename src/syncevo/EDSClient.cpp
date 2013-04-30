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

#include <syncevo/EDSClient.h>

#include <config.h>

#if defined(HAVE_EDS) && defined(USE_EDS_CLIENT)

#include <boost/bind.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

EDSRegistryLoader &EDSRegistryLoader::singleton()
{
    static EDSRegistryLoader self;
    return self;
}

void EDSRegistryLoader::getESourceRegistryAsync(const Callback_t &cb)
{
    singleton().async(cb);
}

void EDSRegistryLoader::async(const Callback_t &cb)
{
    if (m_registry || m_gerror) {
        cb(m_registry, m_gerror);
    } else {
        m_pending.push_back(cb);
        m_loading = true;
        SYNCEVO_GLIB_CALL_ASYNC(e_source_registry_new,
                                boost::bind(&EDSRegistryLoader::created,
                                            this,
                                            _1, _2),
                                NULL);
    }
}

ESourceRegistryCXX EDSRegistryLoader::getESourceRegistry()
{
    return singleton().sync();
}

ESourceRegistryCXX EDSRegistryLoader::sync()
{
    if (!m_loading) {
        m_loading = true;
        SYNCEVO_GLIB_CALL_ASYNC(e_source_registry_new,
                                boost::bind(&EDSRegistryLoader::created,
                                            this,
                                            _1, _2),
                                NULL);
    }

    while (true) {
        if (m_registry) {
            return m_registry;
        }
        if (m_gerror) {
            m_gerror.throwError("creating source registry");
        }
        // Only master thread can drive the event processing.
	if (g_main_context_is_owner(g_main_context_default())) {
            g_main_context_iteration(NULL, true);
        } else {
            Sleep(0.1);
        }
    }
}

void EDSRegistryLoader::created(ESourceRegistry *registry, const GError *gerror) throw ()
{
    try {
        m_registry = ESourceRegistryCXX::steal(registry);
        m_gerror = gerror;
        BOOST_FOREACH (const Callback_t &cb, m_pending) {
            cb(m_registry, m_gerror);
        }
    } catch (...) {
        Exception::handle(HANDLE_EXCEPTION_FATAL);
    }
}

SE_END_CXX

#endif // HAVE_EDS && USE_EDS_CLIENT
