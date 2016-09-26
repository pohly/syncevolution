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
 * $Id: TDEPIMAddressBookSource.cpp,v 1.9 2016/09/12 19:57:27 emanoil Exp $
 *
 */

#include "TDEPIMAddressBookSource.h"

#ifdef ENABLE_TDEPIMABC

#include <syncevo/util.h>
#include <syncevo/Exception.h>
#include <syncevo/Logging.h>

#include <tdeabc/resource.h>
#include <tdeabc/vcardconverter.h>


SE_BEGIN_CXX

TDEPIMAddressBookSource::TDEPIMAddressBookSource( TDEPIMAddressBookSourceType type, const SyncSourceParams &params) :
	TrackingSyncSource(params,1),
	m_type(type),
	addressbookPtr(0),
	ticketPtr(0),
	modified(false)
{

	switch (m_type) {
		case TDEPIM_CONTACT_V21:
			m_contentMimeType = "text/x-vcard";
			m_typeName = "vcard21 addressbook";
		break;
		case TDEPIM_CONTACT_V30:
			m_contentMimeType = "text/vcard";
			m_typeName = "vcard30 addressbook";
		break;
		default:
			Exception::throwError(SE_HERE, "internal init error, invalid addressbook type");
		break;
	}

	app = new TDEPIMSyncSource("syncevo-tdepim-abc");

	addressbookPtr = TDEABC::StdAddressBook::self(false);  // load synchronously
	TDEABC::StdAddressBook::setAutomaticSave(false);  // save only when asked

	if ( ! addressbookPtr ) 
		Exception::throwError(SE_HERE, "internal error, can not open the default addressbook");

	SyncSourceLogging::init(
		InitList<std::string>("N_FIRST") + "N_MIDDLE" + "N_LAST",
		" ",
		m_operations);

// 	SE_LOG_DEBUG(getDisplayName(), "TDE addressbook for %s (mime type: %s)",
// 			m_typeName.latin1(), m_contentMimeType.latin1());
}

TDEPIMAddressBookSource::~TDEPIMAddressBookSource()
{
	if (ticketPtr) { // make sure we release the ticket
		TDEABC::Resource *workbookPtr = ticketPtr->resource() ;
		workbookPtr->releaseSaveTicket(ticketPtr);
		ticketPtr = 0 ;
		SE_LOG_DEBUG(getDisplayName(), "TDE addressbook release ticket on close" );
	}
	delete app;
}

TQString TDEPIMAddressBookSource::lastModifiedNormalized(TDEABC::Addressee &e)
{
	//Get the revision date of the KDE addressbook entry.
	TQDateTime d = e.revision();
	// if no modification date is available, always return the same 0-time stamp
	// to avoid that 2 calls deliver different times which would be treated as changed entry
	// this would result in 1.1.1970
	if ( !d.isValid() ) {
		d.setTime_t(0);
		e.setRevision(d);
	}

// We pass UTC, because we open the calendar in UTC
// 	return d.toString(TQt::ISODate); 
	return d.toString("yyyyMMddThhmmssZ");
}

// TODO if it makes sense to sync up only specific categories
// bool TDEPIMAddressBookSource::hasCategory(const TQStringList &list) const
// {
// 	if ( m_categories.isEmpty() ) return true;  // no filter defined -> match all
// 	
// 	for (TQStringList::const_iterator it = list.begin(); it != list.end(); ++it ) {
// 		if ( m_categories.contains(*it) ) return true;
// 	}
// 	return false; // not found
// }

TDEPIMAddressBookSource::Databases TDEPIMAddressBookSource::getDatabases()
{

	bool first = true;
	Databases result;

	TQPtrList<TDEABC::Resource> lit = addressbookPtr->resources();
	TQPtrListIterator<TDEABC::Resource> it( lit );
	TDEABC::Resource *res;
	while ( (res = it.current()) != 0 ) {
		++it;

		std::string name_str(res->resourceName().utf8(),res->resourceName().utf8().length());
		std::string path_str(res->identifier().utf8(),res->identifier().utf8().length());
		SE_LOG_DEBUG(getDisplayName(), "SUB Name  : %s , ID: %s", name_str.c_str(), path_str.c_str() );
	/*
	 * we pull only active resources thus user has freedom to decide what wants to be visible for sync
	 */
		if ( res->isActive() ) {
			result.push_back (
				Database ( 
					name_str,		// the name of the resource
					path_str,		// the path - (we use the resource uid)
					first,			// default or not
					res->readOnly()		// read only or not
				)
			);
			first = false;
		}
	}
	return result;
}

void TDEPIMAddressBookSource::open()
{
	std::string id = getDatabaseID();
	SE_LOG_DEBUG(getDisplayName(), "TDE search for address book id: %s ", id.c_str() );

	TQPtrList<TDEABC::Resource> lit = addressbookPtr->resources();
	TQPtrListIterator<TDEABC::Resource> it( lit );
	TDEABC::Resource *res;
	while ( (res = it.current()) != 0 ) {
		++it;
		std::string path_str(res->identifier().utf8(),res->identifier().utf8().length());
		if ( id.compare(path_str) == 0 ) {
			if ( ! res->isActive() )
				Exception::throwError(SE_HERE, "internal error, configured resource is not active");
			ticketPtr = res->requestSaveTicket();
			SE_LOG_DEBUG(getDisplayName(), "TDE address book id: %s ", path_str.c_str() );
			break;
		}
	}

	if ( ! ticketPtr )
		Exception::throwError(SE_HERE, "internal error, unable to set ticket on addressbook");
}

bool TDEPIMAddressBookSource::isEmpty()
{
	TDEABC::Resource *workbookPtr = ticketPtr->resource() ;
	TDEABC::Resource::Iterator it = workbookPtr->begin();
	TDEABC::Addressee a = ( *it ); // if the first addressee is empty, the address book is empty
	return a.isEmpty();
}

void TDEPIMAddressBookSource::close()
{
	TDEABC::Resource *workbookPtr = ticketPtr->resource() ;

	if ( modified ) {
		if ( ! workbookPtr->save(ticketPtr) )
			Exception::throwError(SE_HERE, "internal error,unable to use ticket on addressbook");
		modified= false;
	}
	if (ticketPtr)
		workbookPtr->releaseSaveTicket(ticketPtr);
	ticketPtr = 0 ;
}

void TDEPIMAddressBookSource::listAllItems(RevisionMap_t &revisions)
{
	TDEABC::Resource *workbookPtr = ticketPtr->resource() ;

	for (TDEABC::Resource::Iterator it=workbookPtr->begin(); it!=workbookPtr->end(); it++) {
		TDEABC::Addressee ab = (*it);
		TQString lm = lastModifiedNormalized(ab);
		std::string uid_str(ab.uid().utf8(),ab.uid().utf8().length());
		std::string lm_str(lm.utf8(),lm.utf8().length());
		revisions[uid_str] = lm_str;
// 		m_categories.append(a.categories()); // Set filter categories
		SE_LOG_DEBUG(getDisplayName(), "Addressee UID: %s last changed(%s)",uid_str.c_str(),lm_str.c_str() );
	}
}

TrackingSyncSource::InsertItemResult TDEPIMAddressBookSource::insertItem(const std::string &uid, const std::string &item, bool raw)
{

	TDEABC::VCardConverter converter;
	InsertItemResultState state = ITEM_OKAY;
	TQString uidOld = TQString::fromUtf8(uid.data(),uid.size());
	TDEABC::Resource *workbookPtr = ticketPtr->resource() ;
	if ( ! workbookPtr )
		Exception::throwError(SE_HERE, "internal insertItem error, addressbook resource lost");

	TDEABC::Addressee addressee = converter.parseVCard( TQString::fromUtf8(item.data(),item.size()) );

// TODO: add category if present
	// if we run with a configured category filter, but the received added vcard does
	// not contain that category, add the filter-categories so that the address will be
	// found again on the next sync
// 	if ( ! hasCategory(addressee.categories()) ) {
// 		for (TQStringList::const_iterator it = categories.begin(); it != categories.end(); ++it )
// 			addressee.insertCategory(*it);
// 	}

	// ensure it has the correct UID
	if ( uid.empty() )
		uidOld = addressee.uid(); // item push
	else
		addressee.setUid(uidOld); // item replace

// It is not ok to return ITEM_REPLACED
// If we add the addressee to the calendar with the same UID 
// it will overwrite the old one - 
// TODO we need merge here
// 	TDEABC::Addressee addresseeOld = workbookPtr->findByUid(uidOld);
// 	if ( ! addresseeOld.isEmpty() ) {
// 		std::string ret_uid(uidOld.utf8(), uidOld.utf8().length());
// 		return InsertItemResult(ret_uid, "", ITEM_NEEDS_MERGE);
// 	}

	workbookPtr->insertAddressee(addressee);
	modified = true;	

	/* TODO 
		This shouldn't be here in first place, but otherwise the plugin crashes
		It should be in the close(), but no time to investigate what is the reason
		or how it can be improved.
	*/
	if ( ! workbookPtr->save(ticketPtr) ) {
		Exception::throwError(SE_HERE, "internal error, unable to save addressbook item");
// 		return InsertItemResult("", "", ITEM_OKAY);
	}

	// read out the new addressee to get the new revision
	TDEABC::Addressee addresseeNew = workbookPtr->findByUid(uidOld);

	TQString revision = lastModifiedNormalized(addresseeNew);
	std::string uid_str(uidOld.utf8(),uidOld.utf8().length());
	std::string rev_str(revision.utf8(),revision.utf8().length());
	SE_LOG_DEBUG(getDisplayName(), "TDE addressbook UID= %s ADD/UPDATE (REV=%s) OK",uid_str.c_str(),rev_str.c_str() );
	return InsertItemResult(uid_str, rev_str, state);
}

void TDEPIMAddressBookSource::readItem(const std::string &luid, std::string &item, bool raw)
{

	TDEABC::Resource *workbookPtr = ticketPtr->resource() ;
	if ( ! workbookPtr )
		Exception::throwError(SE_HERE, "internal readItem error,unable to find the addressbook id");

	TDEABC::VCardConverter converter;
	TQString data = "";
	// read out the addressee
	TQString uid = TQString::fromUtf8(luid.data(),luid.size());
	TDEABC::Addressee addressee = workbookPtr->findByUid(uid);

	if ( addressee.isEmpty() )
		Exception::throwError(SE_HERE, "internal readItem error: invalid contact");

	if (m_type == TDEPIM_CONTACT_V21 )
		data = converter.createVCard(addressee, TDEABC::VCardConverter::v2_1);
	else
		data = converter.createVCard(addressee, TDEABC::VCardConverter::v3_0);

	std::string data_str(data.utf8(),data.utf8().length());

	item.assign(data_str.c_str());
/* DEBUG
 	SE_LOG_DEBUG(getDisplayName(), "Item id ( %s )", luid.c_str() );
 	SE_LOG_DEBUG(getDisplayName(), "data %s", data_str.c_str());
*/
}

void TDEPIMAddressBookSource::removeItem(const std::string &uid)
{

	TDEABC::Resource *workbookPtr = ticketPtr->resource() ;
	if ( ! workbookPtr )
		Exception::throwError(SE_HERE, "internal readItem error,unable to find the addressbook id");

	//find addressbook entry with matching UID and delete it
	TDEABC::Addressee addressee = workbookPtr->findByUid(TQString::fromUtf8(uid.data(),uid.size()));
	if(!addressee.isEmpty()) {
		workbookPtr->removeAddressee(addressee);
		modified = true;
		SE_LOG_DEBUG(getDisplayName(), "TDE addressbook ENTRY DELETED (UID= %s )",uid.c_str() );

		if ( ! workbookPtr->save(ticketPtr) )
			Exception::throwError(SE_HERE, "internal error, unable to save addressbook item");
	}
// 	else {
// 		SE_LOG_DEBUG(getDisplayName(), "WARN: TDE addressbook ENTRY EMPTY (UID= %s )",uid.c_str() );
// 	}
}

std::string TDEPIMAddressBookSource::getDescription(const string &luid)
{
	TDEABC::Resource *workbookPtr = ticketPtr->resource() ;
	if ( ! workbookPtr )
		Exception::throwError(SE_HERE, "internal getDescription error,unable to find the addressbook id");

	TDEABC::Addressee addressee = workbookPtr->findByUid(TQString::fromUtf8(luid.data(),luid.size()));
	if ( addressee.isEmpty() )
		Exception::throwError(SE_HERE, "internal getDescription error, addressbook not found");

	TDEABC::PhoneNumber::List phonelist = addressee.phoneNumbers();
		
	TQString desc;
	desc.append("Name: ")
		.append(addressee.assembledName()).append(", Nick: ")
		.append(", URI: ").append(addressee.uri())
		.append(addressee.nickName()).append("\nPhone#: ");
	
	for (TDEABC::PhoneNumber::List::Iterator it=phonelist.begin(); it!=phonelist.end(); it++ ) {
		desc.append((*it).number()).append(", ");
	}
	desc.append("\n");
	std::string desc_str(desc.utf8(),desc.utf8().length());
	SE_LOG_DEBUG(getDisplayName(), "User summary %s", desc_str.c_str());
	return desc_str;
}

void TDEPIMAddressBookSource::getSynthesisInfo(SynthesisInfo &info,
                                           XMLConfigFragments &fragments)
{
	TrackingSyncSource::getSynthesisInfo(info, fragments);
	info.m_backendRule = "TDE";
	info.m_beforeWriteScript = "";
}

std::string TDEPIMAddressBookSource::getMimeType() const
{
	switch( m_type ) {
	  case TDEPIM_CONTACT_V21:
		return "text/x-vcard";
		break;
	  case TDEPIM_CONTACT_V30:
	  default:
		return "text/vcard";
		break;
	}
}

std::string TDEPIMAddressBookSource::getMimeVersion() const
{
	switch( m_type ) {
	  case TDEPIM_CONTACT_V21:
		return "2.1";
		break;
	  case TDEPIM_CONTACT_V30:
	  default:
		return "3.0";
		break;
	}
}

SE_END_CXX

#endif /* ENABLE_TDEPIMABC */

#ifdef ENABLE_MODULES
# include "TDEPIMAddressBookSourceRegister.cpp"
#endif
