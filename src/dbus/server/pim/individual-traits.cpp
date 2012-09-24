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

#include "individual-traits.h"
#include "folks.h"

SE_BEGIN_CXX

static const char * const INDIVIDUAL_DICT = "a{sv}";
static const char * const INDIVIDUAL_DICT_ENTRY = "{sv}";

void DBus2FolksIndividual(GDBusCXX::reader_type &iter, FolksIndividualCXX &individual)
{
    individual = FolksIndividualCXX::steal(folks_individual_new(NULL));
}

void FolksIndividual2DBus(const FolksIndividualCXX &individual, GDBusCXX::builder_type &builder)
{
    g_variant_builder_open(&builder, G_VARIANT_TYPE(INDIVIDUAL_DICT)); // dict

    // full name
    const gchar *fullname = folks_name_details_get_full_name(FOLKS_NAME_DETAILS(individual.get()));
    if (fullname) {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(INDIVIDUAL_DICT_ENTRY)); // dict entry
        GDBusCXX::dbus_traits<std::string>::append(builder, "full-name"); // variant
        g_variant_builder_open(&builder, G_VARIANT_TYPE("v"));
        GDBusCXX::dbus_traits<std::string>::append(builder, fullname);
        g_variant_builder_close(&builder); // variant
        g_variant_builder_close(&builder); // dict entry
    }

    g_variant_builder_close(&builder); // dict
}

SE_END_CXX
