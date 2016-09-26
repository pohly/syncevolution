/*
 *
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
 * $Id: TDEPIMCalendarSource.h,v 1.7 2016/09/08 22:58:08 emanoil Exp $
 *
 */

#ifndef INCL_TDEPIM_CALENDARSOURCE
#define INCL_TDEPIM_CALENDARSOURCE

#include "config.h"

#include "TDEPIMSyncSource.h"

#ifdef ENABLE_TDEPIMCAL

#include <syncevo/TrackingSyncSource.h>

#include <tqstring.h>
#include <libkcal/calendarresources.h>


SE_BEGIN_CXX

// class TimeTrackingObserver;

typedef	enum {
		TDEPIM_TASKS,
		TDEPIM_TODO,
		TDEPIM_JOURNAL
	} TDEPIMCalendarSourceType;

class TDEPIMCalendarSource : public TrackingSyncSource, public SyncSourceLogging
{
public:

//	TDEPIMCalendarSource();
	TDEPIMCalendarSource(TDEPIMCalendarSourceType m_type, const SyncSourceParams &params);

	virtual ~TDEPIMCalendarSource();
	//
	// implementation of SyncSource
	//
	virtual Databases getDatabases();
	virtual void open();
	virtual bool isEmpty();
	virtual void close();
	virtual std::string getMimeType() const;
	virtual std::string getMimeVersion() const;

	/* implementation of TrackingSyncSource interface */
	virtual void listAllItems(RevisionMap_t &revisions);
	virtual InsertItemResult insertItem(const string &luid, const std::string &item, bool raw);
	void readItem(const std::string &luid, std::string &item, bool raw);
	virtual void removeItem(const string &luid);

    /* implementation of SyncSourceLogging interface */
	virtual std::string getDescription(const string &luid);

private:
	TDEPIMCalendarSourceType m_type;         /**< use events, tasks or memos? */
	KCal::CalendarResources *calendarResPtr;
	KCal::ResourceCalendar *calendarPtr;

	TQString m_typeName; //NOTE not really used

	TDEPIMSyncSource *app;

	/**
	* The actual type to be used inside the collection. Set after opening
	* the collection.
	*/
	TQString m_contentMimeType; //NOTE not really used

	/**
	* This functions is used internally to normalize the revision field 
	* If no revision is available, always return the same 0-time stamp
	*/
	TQString lastModifiedNormalized(const KCal::Incidence *e);

	/* All calendar storages must suppport UID/RECURRENCE-ID,
	 * it's part of the API. Therefore we can rely on it.
	 */
	virtual void getSynthesisInfo(SynthesisInfo &info,
					XMLConfigFragments &fragments);

};

SE_END_CXX

#endif // ENABLE_TDEPIMCAL
#endif // INCL_TDEPIM_CALENDARSOURCE
