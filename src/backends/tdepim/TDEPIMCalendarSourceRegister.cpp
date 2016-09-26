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
 * $Id: TDEPIMCalendarSourceRegister.cpp,v 1.5 2016/09/08 22:58:08 emanoil Exp $
 *
 */


// #include "TDEPIMNotesSource.h"
// #include "test.h"

#include "TDEPIMSyncSource.h"

SE_BEGIN_CXX

static SyncSource *createSource ( const SyncSourceParams &params )
{
/*
* NOTE: The libkcal vCal (v.1.0) does not work pretty well I had to leave
*       support only for iCal
*/

	SourceType sourceType = SyncSource::getSourceType(params.m_nodes);

// 	SE_LOG_DEBUG("createSource() c1", "Requested Source format %s", sourceType.m_format.c_str());
// 	SE_LOG_DEBUG("createSource() c2", "Requested backend type  %s", sourceType.m_backend.c_str() );

	bool isMe = sourceType.m_backend == "TDE PIM Calendar";
#ifndef ENABLE_TDEPIMCAL
	if (isMe || sourceType.m_backend == "calendar" ) return RegisterSyncSource::InactiveSource(params);
#else
	if (isMe || sourceType.m_backend == "calendar" ) {
		if ( sourceType.m_format == "" || 
			sourceType.m_format == "text/calendar" /*||
			sourceType.m_format == "text/x-calendar" || 
			sourceType.m_format == "text/x-vcalendar"*/ )
				return new TDEPIMCalendarSource ( TDEPIM_TASKS, params );
		else  return NULL;
	}
#endif
	
	isMe = sourceType.m_backend == "TDE PIM Task List";
#ifndef ENABLE_TDEPIMCAL
	if (isMe || sourceType.m_backend == "todo") return RegisterSyncSource::InactiveSource(params);
#else
	if (isMe || sourceType.m_backend == "todo") {
		if ( sourceType.m_format == "" || 
			sourceType.m_format == "text/calendar" /*|| 
			sourceType.m_format == "text/x-calendar" ||
			sourceType.m_format == "text/x-vcalendar"*/)
				return new TDEPIMCalendarSource ( TDEPIM_TODO, params );
		else  return NULL;
	}
#endif
	
	isMe = sourceType.m_backend == "TDE PIM Memos";
#ifndef ENABLE_TDEPIMCAL
	if (isMe || sourceType.m_backend == "memo") return RegisterSyncSource::InactiveSource(params);
#else
	if (isMe || sourceType.m_backend == "memo") {
		if ( sourceType.m_format == "" || 
			sourceType.m_format == "text/calendar" /*||
			sourceType.m_format == "text/x-calendar" || 
			sourceType.m_format == "text/x-vcalendar"*/)
				return new TDEPIMCalendarSource ( TDEPIM_JOURNAL, params );
		else  return NULL;
	}
#endif
// 	SE_LOG_DEBUG("createSource() c6", "Calendar Source matching the format %s not found", sourceType.m_format.c_str() );
	return NULL;
}

static class RegisterTDEPIMCalSyncSource : public RegisterSyncSource
{
public:
    RegisterTDEPIMCalSyncSource() :
        RegisterSyncSource("TDE PIM Calendar/Tasks/Memos",
#ifdef ENABLE_TDEPIMCAL
                                       true,
#else
                                       false,
#endif
                                       createSource,
                                       "TDE PIM Calendar = calendar = events = tdepim-events\n"
                                       "   iCalendar 2.0 (default) = text/calendar\n"
//                                        "   vCalendar 1.0 = text/x-calendar\n"
//                                        "   vCalendar 1.0 = text/x-vcalendar\n"
                                       "TDE PIM Task List = TDE Tasks = todo = tasks = tdepim-tasks\n"
                                       "   iCalendar 2.0 (default) = text/calendar\n"
//                                        "   vCalendar 1.0 = text/x-calendar\n"
//                                        "   vCalendar 1.0 = text/x-vcalendar\n"
                                       "TDE PIM Memos = memo = memos = tdepim-memos\n"
                                       "   iCalendar 2.0 (default) = text/calendar\n"
                                       /*"   vCalendar 1.0 = text/x-vcalendar\n"
                                       "   vCalendar 1.0 = text/x-vcalendar\n"*/,
                                       Values() +
                                       ( Aliases ( "TDE PIM Calendar" ) + "TDE PIM Events" + "calendar" + "events" + "tdepim-calendar" ) +
                                       ( Aliases ( "TDE PIM Task List" ) + "TDE PIM Tasks" + "todo" + "todos" + "tasks" + "tdepim-tasks" ) +
                                       ( Aliases ( "TDE PIM Memos" ) + "TDE PIM Journal" + "memo" + "memos" + "tdepim-memos" ) )
    {
        // configure and register our own property;
        // do this regardless whether the backend is enabled,
        // so that config migration always includes this property
/*        WebDAVCredentialsOkay().setHidden(true);
        SyncConfig::getRegistry().push_back(&WebDAVCredentialsOkay());
*/
    }
} registerMe;


SE_END_CXX
