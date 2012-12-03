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

#if defined(HAVE_EDS) && defined(USE_EDS_CLIENT)

#include <syncevo/GLibSupport.h>

#include <libedataserver/libedataserver.h>
#include <boost/function.hpp>
#include <boost/utility.hpp>
#include <list>

typedef SyncEvo::GListCXX<ESource, GList, SyncEvo::GObjectDestructor> ESourceListCXX;
SE_GOBJECT_TYPE(ESourceRegistry)
SE_GOBJECT_TYPE(ESource)
SE_GOBJECT_TYPE(EClient)

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * Creates ESourceRegistry on demand and shares it inside
 * SyncEvolution. It's never freed once used.
 */
class EDSRegistryLoader : private boost::noncopyable
{
 public:
    typedef boost::function<void (const ESourceRegistryCXX &registry,
                                  const GError *gerror)> Callback_t;

    /**
     * Callback gets invoked exactly once. If the registry pointer is empty,
     * then the error will explain why.
     */
    static void getESourceRegistryAsync(const Callback_t &cb);

    /**
     * Returns shared ESourceRegistry, throws error if creation failed.
     */
    static ESourceRegistryCXX getESourceRegistry();

 private:
    Bool m_loading;
    ESourceRegistryCXX m_registry;
    GErrorCXX m_gerror;
    std::list<Callback_t> m_pending;

    static EDSRegistryLoader &singleton();
    void async(const Callback_t &cb);
    ESourceRegistryCXX sync();
    void created(ESourceRegistry *registry, const GError *gerror) throw ();
};

SE_END_CXX

#endif // HAVE_EDS && USE_EDS_CLIENT
#endif // INCL_SYNCEVO_EDS_CLIENT
