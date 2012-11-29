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

/**
 * Search in an EBook once. Uses folks-eds (= EDSF) to turn EContacts
 * into FolksPersonas and from that into FolksIndividuals. Results
 * will be sorted after the search is complete, then they will be
 * advertised with the "added" signal.
 */

#ifndef INCL_SYNCEVO_DBUS_SERVER_PIM_EDSF_VIEW
#define INCL_SYNCEVO_DBUS_SERVER_PIM_EDSF_VIEW

#include "view.h"
#include <folks/folks-eds.h>
#include <syncevo/EDSClient.h>

#include <boost/ptr_container/ptr_vector.hpp>

SE_GOBJECT_TYPE(EBookClient)
SE_GOBJECT_TYPE(EdsfPersonaStore);

#include <syncevo/declarations.h>
SE_BEGIN_CXX

class EDSFView : public StreamingView
{
    boost::weak_ptr<EDSFView> m_self;
    ESourceRegistryCXX m_registry;
    std::string m_uuid;
    std::string m_query;

    EBookClientCXX m_ebook;
    EdsfPersonaStoreCXX m_store;
    Bool m_isQuiescent;

    EDSFView(const ESourceRegistryCXX &registry,
             const std::string &uuid,
             const std::string &query);
    void init(const boost::shared_ptr<EDSFView> &self);

    void opened(gboolean success, const GError *gerror) throw();
    void read(gboolean success, GSList *contactlist, const GError *gerror) throw();

 public:
    static boost::shared_ptr<EDSFView> create(const ESourceRegistryCXX &registry,
                                              const std::string &uuid,
                                              const std::string &query);

    virtual bool isQuiescent() const { return m_isQuiescent; }

 protected:
    virtual void doStart();
};

SE_END_CXX

#endif // INCL_SYNCEVO_DBUS_SERVER_PIM_EDSF_VIEW
