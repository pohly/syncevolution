/**
 * Browser D-Bus Bridge, XPCOM version
 *
 * Copyright © 2008 Movial Creative Technologies Inc
 *  Contact: Movial Creative Technologies Inc, <info@movial.com>
 *  Authors: Kalle Vahlman, <kalle.vahlman@movial.com>
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is the Browser D-Bus Bridge, XPCOM version.
 *
 * The Initial Developer of the Original Code is Movial Creative Technologies
 * Inc. Portions created by Initial Developer are Copyright (C) 2008
 * Movial Creative Technologies Inc. All Rights Reserved.
 *
 */

#ifndef __DBUSDATACARRIER_H__
#define __DBUSDATACARRIER_H__

#include "nsEmbedString.h"
#include "nsWeakReference.h"
#include "nsIXPConnect.h"

#include "IDBusService.h"

#define DBUSDATACARRIER_CID \
{ \
    0x6dbaa8b4, \
    0x3d38, \
    0x4897, \
    { 0x81, 0xb0, 0x52, 0xa1, 0xba, 0xfb, 0x38, 0xec } \
}

class DBusDataCarrier : public IDBusDataCarrier
{
public:
  NS_DECL_ISUPPORTS
  NS_DECL_IDBUSDATACARRIER

  DBusDataCarrier();

private:
  ~DBusDataCarrier();

protected:

  nsCString mType;
  nsCString mSig;
  nsCOMPtr<nsIVariant> mValue;
};

#endif /* __DBUSDATACARRIER_H__ */

/* vim: set cindent ts=4 et sw=4: */
