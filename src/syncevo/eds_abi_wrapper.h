/*
 * Copyright (C) 2005-2009 Patrick Ohly <patrick.ohly@gmx.de>
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
 * The main purpose of this file was to separate SyncEvolution from
 * ABI changes by never depending directly
 * on any symbol in libraries. Instead all functions were
 * called via function pointers found via dlopen/dlsym. Originally
 * meant for Evolution Data Server (EDS), hence the name.
 *
 * This is more flexible than linking against a specific revision of
 * the libs, but circumvents the usual lib versioning and therefore
 * may fail when the functions needed by SyncEvolution change.
 *
 * History showed that in particular EDS changed sonames quite
 * frequently although the actual functions needed by SyncEvolution
 * didn't change.
 *
 * Nowadays, normal linking is used again, with code sensitive to
 * library versions located in dynamically loaded backends (sometimes
 * recompiled, sometimes copied and patched to support multiple
 * different library versions), so this wrapper is mostly empty now.
 *
 * It's kept around to minimize changes elsewhere.
 */

#ifndef INCL_EDS_ABI_WRAPPER
#define INCL_EDS_ABI_WRAPPER

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_EDS
#include <glib-object.h>
#if defined(USE_EDS_CLIENT)
#include <libedataserver/libedataserver.h>
#else
#ifdef HAVE_LIBEDATASERVER_EDS_VERSION_H
#include <libedataserver/eds-version.h>
#endif
#include <libedataserver/e-source.h>
#include <libedataserver/e-source-list.h>
#endif
#ifdef ENABLE_EBOOK
#ifdef USE_EDS_CLIENT
#include <libebook/libebook.h>
#else
#include <libebook/e-book.h>
#include <libebook/e-vcard.h>
#include <libebook/e-book-query.h>
#endif
#endif
#ifdef ENABLE_ECAL
# define HANDLE_LIBICAL_MEMORY 1
#ifdef USE_EDS_CLIENT
#include <libecal/libecal.h>
#else
#include <libecal/e-cal.h>
#endif
#endif
#endif
#ifdef ENABLE_BLUETOOTH
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#endif

#if !defined(ENABLE_ECAL) && defined(ENABLE_ICAL)
# define HANDLE_LIBICAL_MEMORY 1
# include <libical/ical.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

# if !defined(EDS_ABI_WRAPPER_NO_REDEFINE) && defined(HAVE_LIBICAL_R)
#  ifdef ENABLE_ICAL
#   ifndef LIBICAL_MEMFIXES
     /* This changes the semantic of the normal functions the same way as libecal did. */
#    define LIBICAL_MEMFIXES 1
#    define icaltime_as_ical_string icaltime_as_ical_string_r
#    define icalcomponent_as_ical_string icalcomponent_as_ical_string_r
#    define icalproperty_get_value_as_string icalproperty_get_value_as_string_r
#   endif /* LIBICAL_MEMFIXES */
#  endif /* ENABLE_ICAL */
# endif /* EDS_ABI_WRAPPER_NO_REDEFINE */

const char *EDSAbiWrapperInfo();
const char *EDSAbiWrapperDebug();
void EDSAbiWrapperInit();

#ifdef __cplusplus
}
#endif

#endif /* INCL_EDS_ABI_WRAPPER */
