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
 * $Id: TDEPlatformRegister.cpp,v 1.4 2016/09/01 10:41:38 emanoil Exp $
 *
 */

#include <config.h>

#ifdef ENABLE_TDEWALLET

#include "TDEPlatform.h"
#include <syncevo/SyncContext.h>
#include <syncevo/UserInterface.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

static class TDEInit
{
public:
    TDEInit()
    {
        GetLoadPasswordSignal().connect(0, TDEWalletLoadPasswordSlot);
        GetSavePasswordSignal().connect(0, TDEWalletSavePasswordSlot);
        SyncContext::GetInitMainSignal().connect(TDEInitMainSlot);
    }
} tdeinit;

SE_END_CXX

#endif // ENABLE_TDEWALLET
