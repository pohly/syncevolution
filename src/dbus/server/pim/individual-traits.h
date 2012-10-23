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

#ifndef INCL_SYNCEVO_DBUS_SERVER_INDIVIDUAL_TRAITS
#define INCL_SYNCEVO_DBUS_SERVER_INDIVIDUAL_TRAITS

#include <folks/folks.h>

#include "gdbus-cxx-bridge.h"
#include "../dbus-callbacks.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

class PersonaDetails;
class FolksIndividualCXX;
void DBus2PersonaDetails(GDBusCXX::ExtractArgs &context, GDBusCXX::reader_type &iter, PersonaDetails &details);
void FolksIndividual2DBus(const FolksIndividualCXX &individual, GDBusCXX::builder_type &builder);
void Details2Persona(const Result<void ()> &result, const PersonaDetails &details, FolksPersona *persona);

SE_END_CXX

namespace GDBusCXX {
    using namespace SyncEvo;

    /**
     * The mapping between the internal representation of a
     * FolksIndividual and D-Bus. Only a subset of the internal
     * properties are supported.
     *
     * The D-Bus side is a mapping from string keys to values which
     * are either plain text or another such mapping. Boost
     * cannot represent such a variant, so we cheat and only declare
     * std::string as content. It doesn't matter for the D-Bus
     * signature and decoding/encoding is done differently anyway.
     *
     * Like the rest of the code for the org._01 API, this code only
     * works with GDBus GIO.
     */
    template <> struct dbus_traits<FolksIndividualCXX> :
        public dbus_traits< std::map<std::string, boost::variant<std::string> >  >
    {
        typedef FolksIndividualCXX host_type;
        typedef const FolksIndividualCXX &arg_type;

        static void append(GDBusCXX::builder_type &builder, arg_type individual)
        {
            FolksIndividual2DBus(individual, builder);
        }
    };

    /**
     * The corresponding mapping from D-Bus to a GeeMap for add_persona_from_details.
     * See http://telepathy.freedesktop.org/doc/folks/vala/Folks.PersonaStore.add_persona_from_details.html
     * and http://telepathy.freedesktop.org/doc/folks-eds/vala/Edsf.PersonaStore.add_persona_from_details.html
     */
    template <> struct dbus_traits<PersonaDetails> :
        public dbus_traits< std::map<std::string, boost::variant<std::string> >  >
    {
        typedef PersonaDetails host_type;
        typedef const PersonaDetails &arg_type;

        static void get(ExtractArgs &context,
                        GDBusCXX::reader_type &iter,
                        host_type &individual)
        {
            DBus2PersonaDetails(context, iter, individual);
        }
    };
}

#endif // INCL_SYNCEVO_DBUS_SERVER_INDIVIDUAL_TRAITS
