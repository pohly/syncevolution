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
 * $Id: TDEPIMAddressBookSource.h,v 1.4 2016/09/08 22:58:08 emanoil Exp $
 *
 */

#ifndef INCL_TDEPIMABCSOURCE
#define INCL_TDEPIMABCSOURCE

#include "config.h"
#include "TDEPIMSyncSource.h"

#ifdef ENABLE_TDEPIMABC

#include <syncevo/TrackingSyncSource.h>

#include <tqstring.h>
#include <tqstringlist.h>

#include <tdeabc/addressbook.h>
#include <tdeabc/stdaddressbook.h>


SE_BEGIN_CXX

// class TimeTrackingObserver;

typedef	enum {
		TDEPIM_CONTACT_V21,
		TDEPIM_CONTACT_V30
	} TDEPIMAddressBookSourceType;
/**
 * Implements access to TDE PIM address books.
 */
class TDEPIMAddressBookSource  : public TrackingSyncSource, public SyncSourceLogging 
{
public:
	
	TDEPIMAddressBookSource(TDEPIMAddressBookSourceType type, const SyncSourceParams &params) ;
	
	virtual ~TDEPIMAddressBookSource();
	/*
	 *  implementation of SyncSource
	 */
	virtual Databases getDatabases();
	virtual void open();
	virtual bool isEmpty();
	virtual void close(); 
	virtual std::string getMimeType() const;
	virtual std::string getMimeVersion() const;

	/* implementation of TrackingSyncSource interface */
	virtual void listAllItems(RevisionMap_t &revisions);
	virtual InsertItemResult insertItem(const string &luid, const std::string &item, bool raw);
	virtual void readItem(const std::string &luid, std::string &item, bool raw);
	virtual void removeItem(const string &luid);

	/* implementation of SyncSourceLogging interface */
	virtual std::string getDescription(const string &luid);

private:
	TDEPIMAddressBookSourceType m_type;         /* use v2.1 or v3.0 */

	TDEABC::AddressBook * addressbookPtr;
	TDEABC::Ticket * ticketPtr;

	/**
	* set when needed to save addressbook back
	*/
	bool modified;

	/**
	* mandatory TDE app class - needs to be initialized when creating the object
	*/
	TDEPIMSyncSource *app;

	/**
	* The type name associated with the collection
	*/
	TQString m_typeName; // NOTE not really used except for debug in the constructor
	
	/**
	* The actual type to be used inside the collection. Set after opening
	* the collection.
	*/
	TQString m_contentMimeType; // NOTE not really used except for debug in the constructor

	TQStringList m_categories; // TODO It is possible to fileter a category

	/**
	* This functions is used internally to normalize the revision field in
	* the specific entry.
	* If no revision is available, always return the same 0-time stamp
	*/
	TQString lastModified(TDEABC::Addressee &e);

//	/** TODO if it makes sense to sync up only specific categories
//	* return true if at least one item in the given list is included in the categories member
//	*/
//	bool hasCategory(const TQStringList &list) const;
//
//	/** TODO
//	* return the categories as list
//	*/
//	const TQStringList &getCategories() const { return m_categories; }

	/* All calendar storages must suppport UID/RECURRENCE-ID,
	 * it's part of the API. Therefore we can rely on it.
	 */
	virtual void getSynthesisInfo(SynthesisInfo &info,
			XMLConfigFragments &fragments);

};

SE_END_CXX

#endif // ENABLE_TDEPIMABC
#endif // INCL_TDEPIMABCSOURCE
