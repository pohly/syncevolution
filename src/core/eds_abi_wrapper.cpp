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
#include "eds_abi_wrapper.h"
#include "EvolutionSyncClient.h"

#include <string>
#include <sstream>
#include <dlfcn.h>
#include <stdarg.h>

namespace {

std::string lookupDebug, lookupInfo;

}

int EDSAbiHaveEbook, EDSAbiHaveEcal, EDSAbiHaveEdataserver;

#ifdef EVOLUTION_COMPATIBILITY

struct EDSAbiWrapper EDSAbiWrapperSingleton;

namespace {

/**
 * Opens a <basename>.<num> shared object with <num> coming from a
 * range of known compatible versions, falling back to even more
 * recent ones only after warning about it. Then searches for
 * function pointers.
 *
 * Either all or none of the function pointers are set.
 *
 * End user information and debug information are added to
 * lookupDebug and lookupInfo.
 *
 * @param libname   full name including .so suffix; .<num> gets appended
 * @param minver    first known compatible version
 * @param maxver    last known compatible version
 * @return dlhandle which must be kept or freed by caller
 */
void *findSymbols(const char *libname, int minver, int maxver, ... /* function pointer address, name, ..., (void *)0 */)
{
    void *dlhandle = NULL;
    std::ostringstream debug, info;

    if (!dlhandle) {
        for (int ver = maxver;
             ver >= minver;
             --ver) {
            std::ostringstream soname;
            soname << libname << "." << ver;
            dlhandle = dlopen(soname.str().c_str(), RTLD_GLOBAL|RTLD_LAZY);
            if (dlhandle) {
                info << "using " << soname.str() << std::endl;
                break;
            }
        }
    }

    if (!dlhandle) {
        for (int ver = maxver + 1;
             ver < maxver + 50;
             ++ver) {
            std::ostringstream soname;
            soname << libname << "." << ver;
            dlhandle = dlopen(soname.str().c_str(), RTLD_GLOBAL|RTLD_LAZY);
            if (dlhandle) {
                info << "using " << soname.str() << " - might not be compatible!" << std::endl;
                break;
            }
        }
    }
    
    if (!dlhandle) {
        debug << libname << " not found (tried major versions " << minver << " to " << maxver + 49 << ")" << std::endl;
    } else {
        bool allfound = true;

        va_list ap;
        va_start(ap, maxver);
        void **funcptr = va_arg(ap, void **);
        const char *symname = NULL;
        int mandatory;
        while (funcptr && allfound) {
            symname = va_arg(ap, const char *);
            mandatory = va_arg(ap, int);
            *funcptr = dlsym(dlhandle, symname);
            if (!*funcptr && mandatory) {
                debug << symname << " not found" << std::endl;
                allfound = false;
            }
            funcptr = va_arg(ap, void **);
        }
        va_end(ap);

        if (!allfound) {
            /* unusable, clear symbols and free handle */
            va_start(ap, maxver);
            funcptr = va_arg(ap, void **);
            while (funcptr) {
                va_arg(ap, const char *);
                *funcptr = NULL;
                funcptr = va_arg(ap, void **);
            }
            va_end(ap);

            info << libname << " unusable, required function no longer available" << std::endl;
            dlclose(dlhandle);
            dlhandle = NULL;
        }
    }

    lookupInfo += info.str();
    lookupDebug += info.str();
    lookupDebug += debug.str();
    return dlhandle;
}

# ifdef HAVE_EDS
    void *edshandle;
# endif
# ifdef ENABLE_EBOOK
    void *ebookhandle;
# endif
# ifdef ENABLE_ECAL
    void *ecalhandle;
# endif

}

#endif // EVOLUTION_COMPATIBILITY

extern "C" int EDSAbiHaveEbook, EDSAbiHaveEcal, EDSAbiHaveEdataserver;

extern "C" void EDSAbiWrapperInit()
{
    static bool initialized;

    if (initialized) {
        return;
    } else {
        initialized = true;
    }

#ifdef EVOLUTION_COMPATIBILITY
# ifdef HAVE_EDS
    edshandle =
    findSymbols("libedataserver-1.2.so", 7, 11,
                &EDSAbiWrapperSingleton.e_source_get_type, "e_source_get_type", 1,
                &EDSAbiWrapperSingleton.e_source_get_uri, "e_source_get_uri", 1,
                &EDSAbiWrapperSingleton.e_source_group_get_type, "e_source_group_get_type", 1,
                &EDSAbiWrapperSingleton.e_source_group_peek_sources, "e_source_group_peek_sources", 1,
                &EDSAbiWrapperSingleton.e_source_list_peek_groups, "e_source_list_peek_groups", 1,
                &EDSAbiWrapperSingleton.e_source_peek_name, "e_source_peek_name", 1,
                (void *)0);
    EDSAbiHaveEdataserver = EDSAbiWrapperSingleton.e_source_group_peek_sources != 0;
# endif // HAVE_EDS

# ifdef ENABLE_EBOOK
    ebookhandle =
    findSymbols("libebook-1.2.so", 5, 9,
                &EDSAbiWrapperSingleton.e_book_add_contact, "e_book_add_contact", 1,
                &EDSAbiWrapperSingleton.e_book_authenticate_user, "e_book_authenticate_user", 1,
                &EDSAbiWrapperSingleton.e_book_commit_contact, "e_book_commit_contact", 1,
                &EDSAbiWrapperSingleton.e_contact_duplicate, "e_contact_duplicate", 1,
                &EDSAbiWrapperSingleton.e_contact_get_const, "e_contact_get_const", 1,
                &EDSAbiWrapperSingleton.e_contact_get, "e_contact_get", 1,
                &EDSAbiWrapperSingleton.e_contact_name_free, "e_contact_name_free", 1,
                &EDSAbiWrapperSingleton.e_contact_get_type, "e_contact_get_type", 1,
                &EDSAbiWrapperSingleton.e_contact_new_from_vcard, "e_contact_new_from_vcard", 1,
                &EDSAbiWrapperSingleton.e_contact_set, "e_contact_set", 1,
                &EDSAbiWrapperSingleton.e_book_error_quark, "e_book_error_quark", 1,
                &EDSAbiWrapperSingleton.e_book_get_addressbooks, "e_book_get_addressbooks", 1,
                &EDSAbiWrapperSingleton.e_book_get_changes, "e_book_get_changes", 1,
                &EDSAbiWrapperSingleton.e_book_get_contact, "e_book_get_contact", 1,
                &EDSAbiWrapperSingleton.e_book_get_contacts, "e_book_get_contacts", 1,
                &EDSAbiWrapperSingleton.e_book_get_supported_auth_methods, "e_book_get_supported_auth_methods", 1,
                &EDSAbiWrapperSingleton.e_book_get_uri, "e_book_get_uri", 1,
                &EDSAbiWrapperSingleton.e_book_new, "e_book_new", 1,
                &EDSAbiWrapperSingleton.e_book_new_default_addressbook, "e_book_new_default_addressbook", 1,
                &EDSAbiWrapperSingleton.e_book_new_from_uri, "e_book_new_from_uri", 1,
                &EDSAbiWrapperSingleton.e_book_new_system_addressbook, "e_book_new_system_addressbook", 1,
                &EDSAbiWrapperSingleton.e_book_open, "e_book_open", 1,
                &EDSAbiWrapperSingleton.e_book_query_any_field_contains, "e_book_query_any_field_contains", 1,
                &EDSAbiWrapperSingleton.e_book_query_unref, "e_book_query_unref", 1,
                &EDSAbiWrapperSingleton.e_book_remove_contact, "e_book_remove_contact", 1,
                &EDSAbiWrapperSingleton.e_vcard_to_string, "e_vcard_to_string", 1,
                &EDSAbiWrapperSingleton.e_book_check_static_capability, "e_book_check_static_capability", 1,
                &EDSAbiWrapperSingleton.e_book_commit_contact_instance, "e_book_commit_contact_instance", 0,
                &EDSAbiWrapperSingleton.e_book_remove_contact_instance, "e_book_remove_contact_instance", 0,
                (void *)0);
    EDSAbiHaveEbook = EDSAbiWrapperSingleton.e_book_new != 0;
# endif // ENABLE_EBOOK

# ifdef ENABLE_ECAL
    ecalhandle =
    findSymbols("libecal-1.2.so", 3, 7,
                &EDSAbiWrapperSingleton.e_cal_add_timezone, "e_cal_add_timezone", 1,
                &EDSAbiWrapperSingleton.e_cal_component_get_icalcomponent, "e_cal_component_get_icalcomponent", 1,
                &EDSAbiWrapperSingleton.e_cal_component_get_last_modified, "e_cal_component_get_last_modified", 1,
                &EDSAbiWrapperSingleton.e_cal_component_get_type, "e_cal_component_get_type", 1,
                &EDSAbiWrapperSingleton.e_cal_create_object, "e_cal_create_object", 1,
                &EDSAbiWrapperSingleton.e_calendar_error_quark, "e_calendar_error_quark", 1,
                &EDSAbiWrapperSingleton.e_cal_get_component_as_string, "e_cal_get_component_as_string", 1,
                &EDSAbiWrapperSingleton.e_cal_get_object, "e_cal_get_object", 1,
                &EDSAbiWrapperSingleton.e_cal_get_object_list_as_comp, "e_cal_get_object_list_as_comp", 1,
                &EDSAbiWrapperSingleton.e_cal_get_sources, "e_cal_get_sources", 1,
                &EDSAbiWrapperSingleton.e_cal_get_timezone, "e_cal_get_timezone", 1,
                &EDSAbiWrapperSingleton.e_cal_modify_object, "e_cal_modify_object", 1,
                &EDSAbiWrapperSingleton.e_cal_new, "e_cal_new", 1,
                &EDSAbiWrapperSingleton.e_cal_new_from_uri, "e_cal_new_from_uri", 1,
                &EDSAbiWrapperSingleton.e_cal_new_system_calendar, "e_cal_new_system_calendar", 1,
                &EDSAbiWrapperSingleton.e_cal_new_system_tasks, "e_cal_new_system_tasks", 1,
                &EDSAbiWrapperSingleton.e_cal_open, "e_cal_open", 1,
                &EDSAbiWrapperSingleton.e_cal_remove_object, "e_cal_remove_object", 1,
                &EDSAbiWrapperSingleton.e_cal_remove_object_with_mod, "e_cal_remove_object_with_mod", 1,
                &EDSAbiWrapperSingleton.e_cal_set_auth_func, "e_cal_set_auth_func", 1,
                &EDSAbiWrapperSingleton.icalcomponent_add_component, "icalcomponent_add_component", 1,
                &EDSAbiWrapperSingleton.icalcomponent_as_ical_string, "icalcomponent_as_ical_string",1,
                &EDSAbiWrapperSingleton.icalcomponent_free, "icalcomponent_free", 1,
                &EDSAbiWrapperSingleton.icalcomponent_get_first_component, "icalcomponent_get_first_component", 1,
                &EDSAbiWrapperSingleton.icalcomponent_get_first_property, "icalcomponent_get_first_property", 1,
                &EDSAbiWrapperSingleton.icalcomponent_get_next_component, "icalcomponent_get_next_component", 1,
                &EDSAbiWrapperSingleton.icalcomponent_get_next_property, "icalcomponent_get_next_property", 1,
                &EDSAbiWrapperSingleton.icalcomponent_get_recurrenceid, "icalcomponent_get_recurrenceid", 1,
                &EDSAbiWrapperSingleton.icalcomponent_get_timezone, "icalcomponent_get_timezone", 1,
                &EDSAbiWrapperSingleton.icalcomponent_get_uid, "icalcomponent_get_uid", 1,
                &EDSAbiWrapperSingleton.icalcomponent_isa, "icalcomponent_isa", 1,
                &EDSAbiWrapperSingleton.icalcomponent_new_clone, "icalcomponent_new_clone", 1,
                &EDSAbiWrapperSingleton.icalcomponent_new_from_string, "icalcomponent_new_from_string", 1,
                &EDSAbiWrapperSingleton.icalcomponent_remove_property, "icalcomponent_remove_property", 1,
                &EDSAbiWrapperSingleton.icalcomponent_set_uid, "icalcomponent_set_uid", 1,
                &EDSAbiWrapperSingleton.icalcomponent_vanew, "icalcomponent_vanew", 1,
                &EDSAbiWrapperSingleton.icalparameter_get_tzid, "icalparameter_get_tzid", 1,
                &EDSAbiWrapperSingleton.icalparameter_set_tzid, "icalparameter_set_tzid", 1,
                &EDSAbiWrapperSingleton.icalproperty_get_description, "icalproperty_get_description", 1,
                &EDSAbiWrapperSingleton.icalproperty_get_first_parameter, "icalproperty_get_first_parameter", 1,
                &EDSAbiWrapperSingleton.icalproperty_get_lastmodified, "icalproperty_get_lastmodified", 1,
                &EDSAbiWrapperSingleton.icalproperty_get_next_parameter, "icalproperty_get_next_parameter", 1,
                &EDSAbiWrapperSingleton.icalproperty_get_summary, "icalproperty_get_summary", 1,
                &EDSAbiWrapperSingleton.icalproperty_new_description, "icalproperty_new_description", 1,
                &EDSAbiWrapperSingleton.icalproperty_new_summary, "icalproperty_new_summary", 1,
                &EDSAbiWrapperSingleton.icalproperty_set_value_from_string, "icalproperty_set_value_from_string", 1,
                &EDSAbiWrapperSingleton.icaltime_as_ical_string, "icaltime_as_ical_string", 1,
                &EDSAbiWrapperSingleton.icaltimezone_free, "icaltimezone_free", 1,
                &EDSAbiWrapperSingleton.icaltimezone_get_builtin_timezone, "icaltimezone_get_builtin_timezone", 1,
                &EDSAbiWrapperSingleton.icaltimezone_get_builtin_timezone_from_tzid, "icaltimezone_get_builtin_timezone_from_tzid", 1,
                &EDSAbiWrapperSingleton.icaltimezone_get_component, "icaltimezone_get_component", 1,
                &EDSAbiWrapperSingleton.icaltimezone_get_tzid, "icaltimezone_get_tzid", 1,
                &EDSAbiWrapperSingleton.icaltimezone_new, "icaltimezone_new", 1,
                &EDSAbiWrapperSingleton.icaltimezone_set_component, "icaltimezone_set_component", 1,
                &EDSAbiWrapperSingleton.e_cal_get_static_capability, "e_cal_get_static_capability", 1,
                &EDSAbiWrapperSingleton.e_cal_modify_object_instance, "e_cal_modify_object_instance", 0,
                &EDSAbiWrapperSingleton.e_cal_remove_object_instance, "e_cal_remove_object_instance", 0,
                &EDSAbiWrapperSingleton.e_cal_remove_object_with_mod_instance, "e_cal_remove_object_instance", 0,
                (void *)0);
    EDSAbiHaveEcal = EDSAbiWrapperSingleton.e_cal_new != 0;
# endif // ENABLE_ECAL
#else // EVOLUTION_COMPATIBILITY
# ifdef HAVE_EDS
    EDSAbiHaveEdataserver = true;
# endif
# ifdef ENABLE_EBOOK
    EDSAbiHaveEbook = true;
# endif
# ifdef ENABLE_ECAL
    EDSAbiHaveEcal = true;
# endif
#endif // EVOLUTION_COMPATIBILITY
}

extern "C" const char *EDSAbiWrapperInfo() { return lookupInfo.c_str(); }
extern "C" const char *EDSAbiWrapperDebug() { return lookupDebug.c_str(); }
