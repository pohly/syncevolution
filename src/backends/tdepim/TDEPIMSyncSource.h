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

 * $Id: TDEPIMSyncSource.h,v 1.2 2016/09/01 10:40:06 emanoil Exp $
 *
 */

#ifndef TDEPIMSYNCSOURCE_H
#define TDEPIMSYNCSOURCE_H

// #include "config.h"

//#ifdef ENABLE_TDEPIM

#include <tdeapplication.h>
#include <syncevo/util.h>

/**
 * General purpose TDEPIM Sync Source. 
 */

SE_BEGIN_CXX

class TDEPIMSyncSource
{
public:

	TDEPIMSyncSource(TQString);

	~TDEPIMSyncSource();

private:
	TDEApplication *tdeappPtr;
	TQString m_name;
	bool newApp;

};

SE_END_CXX
//#endif // ENABLE_TDEPIM
#endif // TDEPIMSYNCSOURCE_H
