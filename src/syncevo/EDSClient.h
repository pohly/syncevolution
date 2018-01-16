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

#ifndef INCL_SYNCEVO_EDS_CLIENT
#define INCL_SYNCEVO_EDS_CLIENT

#include <config.h>

#include <memory>

#if defined(HAVE_EDS) && defined(USE_EDS_CLIENT)

#include <syncevo/GLibSupport.h>

#include <libedataserver/libedataserver.h>
#include <boost/utility.hpp>
#include <functional>

#include <functional>
#include <list>

typedef SyncEvo::GListCXX<ESource, GList, SyncEvo::GObjectDestructor> ESourceListCXX;
SE_GOBJECT_TYPE(ESourceRegistry)
SE_GOBJECT_TYPE(ESource)
SE_GOBJECT_TYPE(EClient)

#endif // HAVE_EDS && USE_EDS_CLIENT

#include <syncevo/declarations.h>
SE_BEGIN_CXX

// This code must always be compiled into libsyncevolution.
// It may get used by backends which were compiled against
// EDS >= 3.6 even when the libsyncevolution itself wasn't.
class EDSRegistryLoader;
EDSRegistryLoader &EDSRegistryLoaderSingleton(const std::shared_ptr<EDSRegistryLoader> &loader);

// The following code implements EDSRegistryLoader.
// For the sake of simplicity, its all in the header file,
// so all users of it end up with a copy of the code and
// then they can activate that code when instantiating the object
// and passing it to EDSRegistryLoaderSingleton().

#if defined(HAVE_EDS) && defined(USE_EDS_CLIENT)

/**
 * Creates ESourceRegistry on demand and shares it inside
 * SyncEvolution. It's never freed once used.
 */
class EDSRegistryLoader : private boost::noncopyable
{
 public:
    typedef std::function<void (const ESourceRegistryCXX &registry,
                                  const GError *gerror)> Callback_t;

    /**
     * Callback gets invoked exactly once. If the registry pointer is empty,
     * then the error will explain why.
     */
    static void getESourceRegistryAsync(const Callback_t &cb)
    {
        EDSRegistryLoaderSingleton(std::make_shared<EDSRegistryLoader>()).async(cb);
    }

    /**
     * Returns shared ESourceRegistry, throws error if creation failed.
     */
    static ESourceRegistryCXX getESourceRegistry()
    {
        return EDSRegistryLoaderSingleton(std::make_shared<EDSRegistryLoader>()).sync();
    }

 private:
    Bool m_loading;
    ESourceRegistryCXX m_registry;
    GErrorCXX m_gerror;
    std::list<Callback_t> m_pending;

    void async(const Callback_t &cb)
    {
        if (m_registry || m_gerror) {
            cb(m_registry, m_gerror);
        } else {
            m_pending.push_back(cb);
	    ESourceRegistry *registry;
	    GErrorCXX gerror;
	    registry = e_source_registry_new_sync(NULL, gerror);
	    created(registry, gerror);
        }
    }

    ESourceRegistryCXX sync()
    {
        if (!m_registry) {
            ESourceRegistry *registry;
            GErrorCXX gerror;
            registry = e_source_registry_new_sync(NULL, gerror);
            created(registry, gerror);
        }

        if (m_registry) {
            return m_registry;
        }
        if (m_gerror) {
            m_gerror.throwError(SE_HERE, "creating source registry");
        }
        return m_registry;
    }


    void created(ESourceRegistry *registry, const GError *gerror) throw ()
    {
        try {
            m_registry = ESourceRegistryCXX::steal(registry);
            m_gerror = gerror;
            for (const auto &cb: m_pending) {
                cb(m_registry, m_gerror);
            }
        } catch (...) {
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};

#endif // HAVE_EDS && USE_EDS_CLIENT

SE_END_CXX

#endif // INCL_SYNCEVO_EDS_CLIENT
