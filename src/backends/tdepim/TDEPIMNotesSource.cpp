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
 * $Id: TDEPIMNotesSource.cpp,v 1.7 2016/09/12 19:57:27 emanoil Exp $
 *
 */

// #include <memory>
// using namespace std;

#include "config.h"

#ifdef ENABLE_TDEPIMNOTES

#include <syncevo/Exception.h>
#include <syncevo/SmartPtr.h>
#include <syncevo/Logging.h>

#include <tdeapplication.h>
#include <kmdcodec.h>
#include <dcopclient.h>

#include "TDEPIMNotesSource.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

typedef TQString TDENoteID_t;

TDEPIMNotesSource::TDEPIMNotesSource( const SyncSourceParams &params ) : 
	TrackingSyncSource(params,1)
{
	//connect to dcop
	DCOPClient *kn_dcop = TDEApplication::kApplication()->dcopClient();
	if (!kn_dcop)
		Exception::throwError(SE_HERE, "internal init error, unable to make new dcop instance for tdenotes");

	appId = kn_dcop->registerAs("knotes-sync");

	//check knotes running
	QCStringList apps = kn_dcop->registeredApplications();
	if (!apps.contains("knotes")) {
		//start knotes if not running
		knotesWasRunning = false;
		system("knotes");
		system("dcop knotes KNotesIface hideAllNotes");
		SE_LOG_DEBUG(getDisplayName(), "knotes not running started OK");
	} else {
		knotesWasRunning = true;
		SE_LOG_DEBUG(getDisplayName(), "knotes was running OK");
	}

	kn_iface = new KNotesIface_stub("knotes", "KNotesIface");
	if ( ! kn_iface )
		Exception::throwError(SE_HERE, "internal error, KnotesIface");
/*	SyncSourceLogging::init(InitList<std::string>("SUMMARY") + "LOCATION",
				" ",
				m_operations);
*/
}

TDEPIMNotesSource::~TDEPIMNotesSource() {
	if ( ! knotesWasRunning )
		system("dcop knotes MainApplication-Interface quit");
	delete kn_iface;
	kn_iface = NULL;
	SE_LOG_DEBUG(getDisplayName(), "kNotes exit OK");
}

TQString TDEPIMNotesSource::stripHtml(TQString input)
{
	TQString output = NULL;
	unsigned int i = 0;
	int inbraces = 0;
	for (i = 0; i < input.length(); i++) {
		TQCharRef cur = input[i];
		if (cur == '<')
			inbraces = 1;
		if (cur == '>') {
			inbraces = 0;
			continue;
		}
		if (!inbraces)
			output += input[i];
	}
	return output.stripWhiteSpace();
}

TDEPIMNotesSource::Databases TDEPIMNotesSource::getDatabases()
{

	Databases result;

	const std::string name("tdenotes");
	const std::string path("tdepimnotes");

	result.push_back (
		Database ( 
			name,		// the name of the resource
			path,		// the path - (we use the resource uid)
			true,		// default or not
			false		// read only or not
		)
	);

	SE_LOG_DEBUG(getDisplayName(), "tdenotes getting database %s path: %s", name.c_str(), path.c_str());
	return result;
}

void TDEPIMNotesSource::open()
{
	std::string id = getDatabaseID();
	SE_LOG_DEBUG(getDisplayName(), "Resource id: %s opened OK", id.c_str() );
}

bool TDEPIMNotesSource::isEmpty()
{

	TQMap <TDENoteID_t,TQString> fNotes = kn_iface->notes();
	if (kn_iface->status() != DCOPStub::CallSucceeded)
		Exception::throwError(SE_HERE, "internal error, DCOP call failed");

	TQMap<TDENoteID_t,TQString>::ConstIterator i;
	for (i = fNotes.begin(); i != fNotes.end(); i++) {
		if (i.key().length() > 0)
			return false;
	}
	return true;
}

void TDEPIMNotesSource::close()
{
	const std::string id = getDatabaseID();
	SE_LOG_DEBUG(getDisplayName(), "Resource id: %s closed OK", id.c_str() );
}

void TDEPIMNotesSource::listAllItems(SyncSourceRevisions::RevisionMap_t &revisions)
{

	KMD5 hash_value;
	TQMap <TDENoteID_t,TQString> fNotes = kn_iface->notes();
	if (kn_iface->status() != DCOPStub::CallSucceeded)
		Exception::throwError(SE_HERE, "internal error, DCOP call failed");

	TQMap<TDENoteID_t,TQString>::ConstIterator i;
	for (i = fNotes.begin(); i != fNotes.end(); i++) {

		TQString data = i.data() + '\n' + stripHtml(kn_iface->text(i.key()));
		hash_value.update(data.utf8(),data.utf8().length());

		std::string uid_str(i.key().utf8(),i.key().utf8().length());
		revisions[uid_str] = hash_value.base64Digest().data();
		hash_value.reset();
/* DEBUG
		SE_LOG_DEBUG(getDisplayName(), "KNotes UID: %s", static_cast<const char*>(i.key().utf8()) );
		SE_LOG_DEBUG(getDisplayName(), "KNotes DATA: %s", static_cast<const char*>(data.utf8()));
*/
	}
}

TrackingSyncSource::InsertItemResult TDEPIMNotesSource::insertItem(const std::string &luid, const std::string &item, bool raw)
{

		InsertItemResultState state = ITEM_OKAY;
		TrackingSyncSource::InsertItemResult result;
		KMD5 hash_value;

		TQString uid  = TQString::fromUtf8(luid.data(), luid.size());
		TQString data = TQString::fromUtf8(item.data(), item.size());

		// store the hashed value of data to be able to find our new note later and get the id
		hash_value.update(data.utf8(),data.utf8().length());
		TQCString rev = hash_value.base64Digest();
		hash_value.reset();

		TQString summary = data.section('\n', 0, 0);  // first line is our title == summary
		TQString body = data.section('\n', 1);  // rest
/* DEBUG
		SE_LOG_DEBUG(getDisplayName(), "KNotes SUM : %s", static_cast<const char*>(summary.utf8()) );
		SE_LOG_DEBUG(getDisplayName(), "KNotes BODY: %s", static_cast<const char*>(body.utf8()) );
 */
		TQString newuid;

		TQString d = kn_iface->text( uid );
		if ( d.length() > 0 ) { // we have this note
				kn_iface->setName( uid, summary );
				kn_iface->setText( uid, body );
				newuid = uid;
		}
		else {
				newuid = kn_iface->newNote( summary, body );
				if (kn_iface->status() != DCOPStub::CallSucceeded)
					Exception::throwError(SE_HERE, "internal error, DCOP call failed");
				if ( ! newuid.length() > 0 )
						Exception::throwError(SE_HERE, "internal error, add note failed");
		}

		std::string ret_uid(newuid.utf8(), newuid.utf8().length());
		return InsertItemResult(ret_uid, rev.data(), state);

}

void TDEPIMNotesSource::readItem(const std::string &luid, std::string &item, bool raw)
{
	TQString uid = TQString::fromUtf8(luid.data(),luid.size());
	TQString data = kn_iface->name( uid ) + '\n' + stripHtml(kn_iface->text( uid ));

	std::string data_str( data.utf8(), data.utf8().length() );
	item.assign( data_str.c_str() );
}

void TDEPIMNotesSource::removeItem(const std::string &luid)
{
	TQString uid = TQString::fromUtf8(luid.data(),luid.size());
	TQString data = kn_iface->text( uid );
	if ( data.length() > 0 ) {
		kn_iface->killNote( uid );
		if (kn_iface->status() != DCOPStub::CallSucceeded)
			Exception::throwError(SE_HERE, "internal error, DCOP call failed");
	}
	else
		SE_LOG_DEBUG(getDisplayName(), "Item not found: id=%s", luid.c_str() );
}

std::string TDEPIMNotesSource::getDescription(const std::string &luid)
{
	TQString uid = TQString::fromUtf8(luid.data(),luid.size());
	TQString data = kn_iface->name( uid );
	if ( data.length() > 0 ) {
		std::string sum_str(data.utf8(),data.utf8().length());
		return sum_str;
	}
        SE_LOG_DEBUG(getDisplayName(), "Resource id(%s) not found", luid.c_str() );
	return "";
}

void TDEPIMNotesSource::getSynthesisInfo(SynthesisInfo &info, XMLConfigFragments &fragments)
{
	TrackingSyncSource::getSynthesisInfo(info, fragments);
	info.m_backendRule = "TDE";
	info.m_beforeWriteScript = "";
}

SE_END_CXX

#endif /* ENABLE_TDEPIMNOTES */

#ifdef ENABLE_MODULES
# include "TDEPIMNotesSourceRegister.cpp"
#endif
