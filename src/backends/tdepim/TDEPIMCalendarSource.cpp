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
 * $Id: TDEPIMCalendarSource.cpp,v 1.14 2016/09/20 12:56:49 emanoil Exp $
 *
 */


#include "TDEPIMCalendarSource.h"

#ifdef ENABLE_TDEPIMCAL

#include <syncevo/util.h>
#include <syncevo/Exception.h>
#include <syncevo/Logging.h>

#include <kstandarddirs.h>
#include <libkcal/icalformat.h>
//#include <libkcal/vcalformat.h>
#include <libkcal/calendarlocal.h>

SE_BEGIN_CXX

// "PRODID:-//K Desktop Environment//NONSGML libkcal 3.5//EN"
// "VERSION:2.0"

TDEPIMCalendarSource::TDEPIMCalendarSource( TDEPIMCalendarSourceType type, const SyncSourceParams &params ) : 
	TrackingSyncSource(params,1),
	m_type(type),
	calendarResPtr(0),
	calendarPtr(0)
{
 //NOTE m_typeName not really used right now
	switch (m_type) {
		case TDEPIM_TASKS:
			m_typeName = "calendar";
		break;
		case TDEPIM_TODO:
			m_typeName = "task list";
		break;
		case TDEPIM_JOURNAL:
			m_typeName = "memo list";
		break;
		default:
			Exception::throwError(SE_HERE, "internal init error, invalid calendar type");
		break;
	}

	//NOTE not really used
	m_contentMimeType = "text/calendar";

	app = new TDEPIMSyncSource("syncevo-tdepim-cal");

	TDEConfig config( locate( "config", "korganizerrc" ) );
	config.setGroup( "Time & Date" );
	TQString tz = config.readEntry( "TimeZoneId", "UTC" );

	calendarResPtr = new KCal::CalendarResources( tz );
	if (!calendarResPtr) {
		Exception::throwError(SE_HERE, "internal error, can not open the default calendar");
	}

	calendarResPtr->readConfig();
	calendarResPtr->setModified(false);

	SyncSourceLogging::init(InitList<std::string>("SUMMARY") + "LOCATION", " ", m_operations);
// 	SE_LOG_DEBUG(getDisplayName(), "TDE calendar for %s (mime type: %s)", 
// 		m_typeName.latin1(), m_contentMimeType.latin1());
}

TDEPIMCalendarSource::~TDEPIMCalendarSource() {
	delete app;
}

TQString TDEPIMCalendarSource::lastModified(const KCal::Incidence *e)
{
	TQDateTime d = e->lastModified();
	// if no modification date is available, always return the same 0-time stamp
	// to avoid that 2 calls deliver different times which would be treated as changed entry
	// this would result in 1.1.1970
	if (!d.isValid())
		d.setTime_t(0);

	// We pass UTC, because we open the calendar in UTC
// 	return d.toString(TQt::ISODate);
	return d.toString("yyyyMMddThhmmssZ");
}

TDEPIMCalendarSource::Databases TDEPIMCalendarSource::getDatabases()
{

	Databases result;
	bool first = true;

	KCal::CalendarResourceManager * mgr = calendarResPtr->resourceManager();
	/*
	 * we pull only active resources so the user has some freedom to decide 
	 * what will be visible for sync 
	 */
	for (KRES::Manager<KCal::ResourceCalendar>::ActiveIterator i = mgr->activeBegin(); i != mgr->activeEnd(); i++) {

// 		std::string info_str(( *i )->infoText( ).utf8(),( *i )->infoText( ).utf8().length());
// 		SE_LOG_DEBUG(getDisplayName(), "resource: NAME(%s), ID(%s), INFO(%s)", 
// 					name_str.c_str(), path_str.c_str(), info_str.c_str());

		result.push_back (
			Database ( 
				( *i )->resourceName().utf8().data(),		// the name of the resource
				( *i )->identifier().utf8().data(),		// the path - (we use the resource uid)
				first,			// default or not
				( *i )->readOnly()	// read only or not
			)
		);
		first = false;
	}
	return result;
}

void TDEPIMCalendarSource::open()
{

	std::string id = getDatabaseID();
// 	SE_LOG_DEBUG(getDisplayName(), "Search for resource id: %s ", id.c_str() );

	KCal::CalendarResourceManager * mgr = calendarResPtr->resourceManager();
	/*
	 * we pull only active resources so the user has some freedom to decide 
	 * what will be visible for sync 
	 */
	for (KRES::Manager<KCal::ResourceCalendar>::ActiveIterator i = mgr->activeBegin(); i != mgr->activeEnd(); i++) {
		if ( ( *i )->identifier().utf8().data() == id ) {
// 			SE_LOG_DEBUG(getDisplayName(), "Resource id: %s found", path_str.c_str() );
			calendarPtr = ( *i ) ;
			break;
		}
	}

	if ( ! calendarPtr )
		Exception::throwError(SE_HERE, "internal error, calendar not found");

	if ( ! calendarPtr->load()  ) // TODO this fails on vcf resource
		Exception::throwError(SE_HERE, "internal error, calendar failed loading");
// 	SE_LOG_DEBUG(getDisplayName(), "Resource id: %s open OK", id.c_str() );
}

bool TDEPIMCalendarSource::isEmpty()
{

	bool status = true;
	switch (m_type) {
		case TDEPIM_TASKS:
			status =  calendarPtr->rawEvents( KCal::EventSortUnsorted , KCal::SortDirectionAscending ).isEmpty();
			break;
		case TDEPIM_TODO:	
			status =  calendarPtr->rawTodos( KCal::TodoSortUnsorted , KCal::SortDirectionAscending ).isEmpty();
			break;
		case TDEPIM_JOURNAL:
			status =  calendarPtr->rawJournals( KCal::JournalSortUnsorted , KCal::SortDirectionAscending ).isEmpty();
			break;
		default:
			Exception::throwError(SE_HERE, "internal error, invalid calendar type");
			break;
	}
	return status;
}

void TDEPIMCalendarSource::close()
{
	calendarResPtr->save();
	calendarPtr->close();
	delete calendarPtr;
	calendarPtr = 0;
}

void TDEPIMCalendarSource::listAllItems(SyncSourceRevisions::RevisionMap_t &revisions)
{
	KCal::Event::List e;
	KCal::Todo::List t;
	KCal::Journal::List j;
	TQString id;
	TQString lm;

	switch (m_type) {
	  case TDEPIM_TASKS:
		e = calendarPtr->rawEvents( KCal::EventSortUnsorted , KCal::SortDirectionAscending );
		for (KCal::Event::List::ConstIterator i = e.begin(); i != e.end(); i++) {
			lm = lastModified((*i));
			revisions[(*i)->uid().utf8().data()] = lm.utf8().data();
        		SE_LOG_DEBUG(getDisplayName(), "Event UID: %s modified( %s )",
        			(*i)->uid().utf8().data(),
        			lm.utf8().data());
		}
		break;
	  case TDEPIM_TODO:
		t = calendarPtr->rawTodos( KCal::TodoSortUnsorted , KCal::SortDirectionAscending );
		for (KCal::Todo::List::ConstIterator i = t.begin(); i != t.end(); i++) {
			lm = lastModified((*i));
			revisions[(*i)->uid().utf8().data()] = lm.utf8().data();
        		SE_LOG_DEBUG(getDisplayName(), "Todos UID: %s modified( %s )",
        			(*i)->uid().utf8().data(),
        			lm.utf8().data());
		}
		break;
	  case TDEPIM_JOURNAL:
		j = calendarPtr->rawJournals( KCal::JournalSortUnsorted , KCal::SortDirectionAscending );
		for (KCal::Journal::List::ConstIterator i = j.begin(); i != j.end(); i++) {
			lm = lastModified((*i));
			revisions[(*i)->uid().utf8().data()] = lm.utf8().data();
        		SE_LOG_DEBUG(getDisplayName(), "Journal UID: %s modified( %s )",
        			(*i)->uid().utf8().data(),
        			lm.utf8().data());
		}
		break;
	  default:
		Exception::throwError(SE_HERE, "internal error, invalid calendar type");
		break;
	}
}

TrackingSyncSource::InsertItemResult TDEPIMCalendarSource::insertItem(const std::string &luid, const std::string &item, bool raw)
{
// 	SE_LOG_DEBUG(getDisplayName(), "Item payload: ( %s )", item.data() );
	InsertItemResultState state = ITEM_OKAY;
	TrackingSyncSource::InsertItemResult result;
	KCal::ICalFormat format;
	bool replaced = false;

	TQString uid  = TQString::fromUtf8(luid.data(), luid.size());
	TQString data = TQString::fromUtf8(item.data(), item.size());

	SE_LOG_DEBUG(getDisplayName(), "Item to save: ( %s )", data.latin1() );

	/*
	* Check if item already exists. If yes notify the engine and do nothing here
	*/
	KCal::Incidence *oldinc = calendarPtr->incidence(uid);
	if (oldinc) {
		if ( ! calendarPtr->deleteIncidence(oldinc))
				Exception::throwError(SE_HERE, "internal error, unable to delete item from calendar");
// If it was deleted we add it below no need to save now
// 		if ( ! calendarPtr->save() ) 
// 				Exception::throwError(SE_HERE, "internal error, unable to save calendar");
		SE_LOG_DEBUG(getDisplayName(), "Item deleted for merge: ( %s )", uid.latin1() );
		replaced = true;
// FIXME unfortunately the ITEM_NEEDS_MERGE does not work well with updated items
// 		std::string ret_uid(uid.utf8(), uid.utf8().length());
// 		return InsertItemResult(ret_uid, "", ITEM_NEEDS_MERGE);
	}

	/*
	 * Create incidence and set the uid to the old one if replaced
	 */
	KCal::Incidence *e = format.fromString(data);

	if (e==0)
	    Exception::throwError(SE_HERE, "internal error, unable to convert calendar data");

    if ( replaced )
        e->setUid( uid );
    else
        uid = e->uid();

    if ( ! calendarPtr->addIncidence(e))
        Exception::throwError(SE_HERE, "internal error, unable to add item to calendar");

    if ( ! calendarPtr->save(e) )
        Exception::throwError(SE_HERE, "internal error, unable to save item to calendar");
    SE_LOG_DEBUG(getDisplayName(), "Item saved: ( %s )", uid.latin1() );

	calendarResPtr->setModified(true);

	KCal::Incidence *newinc = calendarPtr->incidence(uid);
	if ( ! newinc )
		Exception::throwError(SE_HERE, "internal error, unable to get item from calendar");

	TQString lm=lastModified(newinc);
	SE_LOG_DEBUG(getDisplayName(), "Item ( %s : %s ) done.", 
		newinc->uid().utf8().data(),
		lm.utf8().data());
   return InsertItemResult(newinc->uid().utf8().data(), lm.utf8().data(), state);
}

void TDEPIMCalendarSource::readItem(const std::string &luid, std::string &item, bool raw)
{
	KCal::ICalFormat iCalFmt;
//	NOTE: The libkcal vCal (v.1.0) does not work pretty well - the support is disabled for now
//	KCal::VCalFormat vCalFmt;

	TQString uid = TQString::fromUtf8(luid.data(),luid.size());
	
	/* Build a local calendar for the incidence data */
	KCal::CalendarLocal cal( calendarResPtr->timeZoneId() );
	switch (m_type) {
	  case TDEPIM_TASKS:
		cal.addIncidence(calendarPtr->event(uid)->clone());
	  break;
	  case TDEPIM_TODO:
		cal.addIncidence(calendarPtr->todo(uid)->clone());
	  break;
	  case TDEPIM_JOURNAL:
		cal.addIncidence(calendarPtr->journal(uid)->clone());
	  break;
	  default:
		Exception::throwError(SE_HERE, "internal error, invalid calendar type");
	  break;
	}

	// Convert the data to string
	TQString data = iCalFmt.toString( &cal );
	item.assign(data.utf8().data());
	SE_LOG_DEBUG(getDisplayName(), "Item id ( %s )", luid.c_str() );
// 	SE_LOG_DEBUG(getDisplayName(), "TDE calendar Data: %s\n", data.utf8().data() );
}


void TDEPIMCalendarSource::removeItem(const std::string &luid)
{
	KCal::Incidence *inc = calendarPtr->incidence(TQString::fromUtf8(luid.data(),luid.size()));
	if (inc) {
		calendarPtr->deleteIncidence(inc);
// 			Q: do we really need to save it here?
// 			A: yes definitely
// 			TODO implement save via ticket in future
		calendarPtr->save(); 
		calendarResPtr->setModified(true);
	}
	else
		SE_LOG_DEBUG(getDisplayName(), "Item not found: id=%s", luid.c_str() );
}


std::string TDEPIMCalendarSource::getDescription(const std::string &luid)
{
	KCal::Incidence *inc = calendarPtr->incidence(TQString::fromUtf8(luid.data(),luid.size()));
	if ( inc )
		return inc->summary().utf8().data();
        SE_LOG_DEBUG(getDisplayName(), "Resource id(%s) not found", luid.c_str() );
	return "";
}


void TDEPIMCalendarSource::getSynthesisInfo(SynthesisInfo &info, XMLConfigFragments &fragments)
{
	TrackingSyncSource::getSynthesisInfo(info, fragments);
	info.m_backendRule = "TDE";
//	info.m_backendRule = "LOCALSTORAGE";
	info.m_beforeWriteScript = "";
//	info.m_profile = "\"vCalendar\", 2";
}


std::string TDEPIMCalendarSource::getMimeType() const
{
     return "text/calendar";
}

std::string TDEPIMCalendarSource::getMimeVersion() const
{
    return "2.0";
}

SE_END_CXX

#endif /* ENABLE_TDEPIMCAL */

#ifdef ENABLE_MODULES
# include "TDEPIMCalendarSourceRegister.cpp"
#endif
