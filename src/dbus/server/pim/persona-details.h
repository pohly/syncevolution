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
 * The D-Bus IPC binding for folks.h. Maps FolksIndividual to and
 * from the D-Bus dict described in pim-manager-api.txt.
 */

#ifndef INCL_SYNCEVO_DBUS_SERVER_PERSONA_DETAILS
#define INCL_SYNCEVO_DBUS_SERVER_PERSONA_DETAILS

#include <syncevo/GValueSupport.h>
#include <glib.h>
#include <map>
#include <boost/intrusive_ptr.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

// This is okay with g++ 4.7 and clang 3.0.
// typedef boost::intrusive_ptr<GHashTable> PersonaDetails;
// However, g++ 4.5 is more picky and rejects
//   template <> struct dbus_traits<PersonaDetails> :
// "template argument 1 is invalid"
//
// Besides working around this compiler bug,
// defining a real class also has the advantage that we can
// define D-Bus traits for multiple classes using boost::intrusive_ptr<GHashTable>
// as base, should that ever be necessary.

class PersonaDetails : public boost::intrusive_ptr<GHashTable>
{
 public:
    PersonaDetails() :
        // Keys are static (from folks_persona_store_detail_key()), values
        // are dynamically allocated GValueCXX instances owned by the hash.
        boost::intrusive_ptr<GHashTable>(g_hash_table_new_full(g_str_hash, g_str_equal,
                                                               NULL,
                                                               GValueCXX::destroy),
                                         false)
    {}
};

SE_END_CXX

#endif // INCL_SYNCEVO_DBUS_SERVER_PERSONA_DETAILS
