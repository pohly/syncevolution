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
 * $Id: TDEPlatform.h,v 1.3 2016/09/01 10:41:38 emanoil Exp $
 *
 */

#ifndef INCL_TDEPLATFORM
#define INCL_TDEPLATFORM

#include <string>

#include <syncevo/util.h>

#include <syncevo/declarations.h>

SE_BEGIN_CXX

struct ConfigPasswordKey;

void TDEInitMainSlot(const char *appname);

bool TDEWalletLoadPasswordSlot(const InitStateTri &keyring,
                             const std::string &passwordName,
                             const std::string &descr,
                             const ConfigPasswordKey &key,
                             InitStateString &password);

bool TDEWalletSavePasswordSlot(const InitStateTri &keyring,
                             const std::string &passwordName,
                             const std::string &password,
                             const ConfigPasswordKey &key);
SE_END_CXX

#endif // INCL_TDEPLATFORM
