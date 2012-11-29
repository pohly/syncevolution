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

#include <syncevo/declarations.h>
#include <syncevo/GLibSupport.h>

typedef SyncEvo::GListCXX<ESource, GList, SyncEvo::GObjectDestructor> ESourceListCXX;
SE_GOBJECT_TYPE(ESourceRegistry)
SE_GOBJECT_TYPE(ESource)
SE_GOBJECT_TYPE(EClient)

// TODO: create ESourceRegistry on demand and share it inside SyncEvolution.

#endif // HAVE_EDS && USE_EDS_CLIENT
#endif // INCL_SYNCEVO_EDS_CLIENT
