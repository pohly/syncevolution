/*
 * Copyright (C) 2016 Emanoil Kotsev emanoil.kotsev@fincom.at

    This application is free software; you can redistribute it and/or modify it
    under the terms of the GNU Library General Public License as published by
    the Free Software Foundation; either version 2 of the License, or (at your
    option) any later version.

    This application is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
    License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this application; see the file COPYING.LIB.  If not, write to the
    Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301, USA.

 * $Id: TDEPIMSyncSource.cpp,v 1.4 2016/09/08 22:58:09 emanoil Exp $
 */

#include "TDEPIMSyncSource.h"

#include <syncevo/Logging.h>
#include <tdeaboutdata.h>
#include <tdecmdlineargs.h>

SE_BEGIN_CXX

TDEPIMSyncSource::TDEPIMSyncSource(TQString name) : 
	tdeappPtr(0)
{
	m_name = name;
	newApp = false;
	TDEAboutData aboutData(
		m_name.latin1(),			// internal program name
		"SyncEvolution-TDEPIM-plugin",		// displayable program name.
		"0.1",					// version string
		"SyncEvolution TDEPIM plugin",		// short porgram description
		TDEAboutData::License_GPL,		// license type
		"(c) 2016, emanoil.kotsev@fincom.at"	// copyright statement
	);

	static const char *argv[] = { "SyncEvolution" };
	static int argc = 1;

	TDECmdLineArgs::init(argc, (char **)argv, &aboutData );
	// Don't allow TDEApplication to mess with SIGINT/SIGTERM.
	// Restore current behavior after construction.
	struct sigaction oldsigint, oldsigterm;
	sigaction(SIGINT, nullptr, &oldsigint);
	sigaction(SIGTERM, nullptr, &oldsigterm);
	if ( kapp ) {
		tdeappPtr = kapp;
	} else {
		tdeappPtr = new TDEApplication( false, false );
		newApp=true;
	}
	// restore
	sigaction(SIGINT, &oldsigint, nullptr);
	sigaction(SIGTERM, &oldsigterm, nullptr);

// 	SE_LOG_DEBUG(NULL, "TDE base created OK");
}

TDEPIMSyncSource::~TDEPIMSyncSource()
{
}

SE_END_CXX
