/**
 * Browser D-Bus Bridge, XPCOM version
 *
 * Copyright © 2008 Movial Creative Technologies Inc
 *  Contact: Movial Creative Technologies Inc, <info@movial.com>
 *  Authors: Lauri Mylläri, <lauri.myllari@movial.com>
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

#ifndef __DBUSMETHOD_H__
#define __DBUSMETHOD_H__

#include "nsEmbedString.h"
#include "nsIWeakReference.h"
#include "nsIWeakReferenceUtils.h"
#include "nsIVariant.h"

#include "DBusService.h"

//
// DBusMethod declarations
//

#define DBUSMETHOD_CID \
{ \
    0x2832f621, \
    0xad9b, \
    0x4034, \
    { 0x91, 0x0b, 0xcd, 0x8e, 0xea, 0xdf, 0x5c, 0x42 } \
}

class DBusMethod : public IDBusMethod
{
public:
    NS_DECL_ISUPPORTS
    NS_DECL_IDBUSMETHOD

    DBusMethod(DBusService *aDBusService,
               PRUint32 aBusType,
               const nsACString& aDestination,
               const nsACString& aObjectPath,
               const nsACString& aMethodName,
               const nsACString& aInterfaceName,
               const nsACString& aSignature,
               JSContext *cx);

private:
    ~DBusMethod();

protected:
    DBusService *mDBusService;
    PRUint32    mBusType;
    nsCString   mDestination;
    nsCString   mObject;
    nsCString   mMethod;
    nsCString   mInterface;
    nsCString   mSignature;
    PRBool      mAsync;
    IDBusMethodCallback *mCallback;
    IDBusMethodCallback *mErrorCallback;
    JSContext *mJScx;
};


#endif /* __DBUSMETHOD_H__ */

/* vim: set cindent ts=4 et sw=4: */
