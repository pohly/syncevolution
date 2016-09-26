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
 * $Id: TDEPIMNotesSource.h,v 1.6 2016/09/20 12:56:49 emanoil Exp $
 *
 */

#ifndef INCL_TDEPIMNOTESSOURCE
#define INCL_TDEPIMNOTESSOURCE

#include "config.h"

#include "KNotesIface_stub.h"
#include <syncevo/TrackingSyncSource.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

#ifdef ENABLE_TDEPIMNOTES

/**
 * Implements access to TDE memo lists (stored as knotes items),
 * exporting/importing the memos in plain UTF-8 text. 
 */
class TDEPIMNotesSource : public TrackingSyncSource, public SyncSourceLogging
{
  public:
    TDEPIMNotesSource( const SyncSourceParams &params );
	virtual ~TDEPIMNotesSource();
	//
	// implementation of SyncSource
	//
	virtual Databases getDatabases();
	virtual void open();
	virtual bool isEmpty();
	virtual void close();
	virtual std::string getMimeType() const { return "text/plain"; }
	virtual std::string getMimeVersion() const { return "1.0"; }

	/* implementation of TrackingSyncSource interface */
	virtual void listAllItems(RevisionMap_t &revisions);
	virtual InsertItemResult insertItem(const string &luid, const std::string &item, bool raw);
	void readItem(const std::string &luid, std::string &item, bool raw);
	virtual void removeItem(const string &luid);

	/* implementation of SyncSourceLogging interface */
	virtual std::string getDescription(const string &luid);

private:
	TQString appId;

	KNotesIface_stub *kn_iface;
	DCOPClient *kn_dcop;

	/** Ugly hack to restart KNotes if it was running */
	bool knotesWasRunning;

	/**
	* This functions is used internally to strip HTML from the note
	*/
	static TQString stripHtml(TQString input);

    TQString lastModifiedNormalized(TQDateTime &d) const;

	/** 
	 * Implement some brief information extraction from the note
	 */
	virtual void getSynthesisInfo(SynthesisInfo &info,
					XMLConfigFragments &fragments);

};

#endif // ENABLE_TDEPIMNOTES


SE_END_CXX
#endif // INCL_TDEPIMNOTESSOURCE
