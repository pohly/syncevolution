/*
 * Copyright (C) 2016 Emanoil Kotsev emanoil.kotsev@fincom.at
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
 *
 *
 * $Id: TDEPIMNotesSourceRegister.cpp,v 1.5 2016/09/12 19:57:27 emanoil Exp $
 *
 */


#include "TDEPIMNotesSource.h"
// #include "test.h"

#include "TDEPIMSyncSource.h"

SE_BEGIN_CXX

static SyncSource *createSource ( const SyncSourceParams &params )
{

	SourceType sourceType = SyncSource::getSourceType(params.m_nodes);

// 	SE_LOG_DEBUG("createSource() c1", "Requested Source format %s", sourceType.m_format.c_str());
// 	SE_LOG_DEBUG("createSource() c2", "Requested backend type  %s", sourceType.m_backend.c_str() );

	bool isMe = sourceType.m_backend == "TDE PIM Notes";
#ifndef ENABLE_TDEPIMNOTES
	if (isMe) return RegisterSyncSource::InactiveSource(params);
#else
	if (isMe) {
// 		SE_LOG_DEBUG("createSource() c3", "Calendar Source format %s", sourceType.m_format.c_str());
		if ( sourceType.m_format == "" || sourceType.m_format == "text/plain" ) 
			return new TDEPIMNotesSource ( params );
		else  return NULL;
	}
#endif

// 	SE_LOG_DEBUG("createSource() c6", "Calendar Source matching the format %s not found", sourceType.m_format.c_str() );
	return NULL;
}

static class RegisterTDEPIMNotesSyncSource : public RegisterSyncSource
{
public:
    RegisterTDEPIMNotesSyncSource() :
        RegisterSyncSource("TDE PIM Notes",
#ifdef ENABLE_TDEPIMCAL
                                       true,
#else
                                       false,
#endif
                                       createSource,
                                       "TDE PIM Notes = note = notes = tdepim-notes\n"
                                       "   plain text in UTF-8 (default) = text/plain\n",
                                       Values() +
                                       ( Aliases ( "TDE PIM Notes" ) + "note" + "notes" + "tdepim-notes" ) )
    {
        // configure and register our own property;
        // do this regardless whether the backend is enabled,
        // so that config migration always includes this property
/*        WebDAVCredentialsOkay().setHidden(true);
        SyncConfig::getRegistry().push_back(&WebDAVCredentialsOkay());
*/
    }
} registerMe;

// TODO finish unit tests

SE_END_CXX
