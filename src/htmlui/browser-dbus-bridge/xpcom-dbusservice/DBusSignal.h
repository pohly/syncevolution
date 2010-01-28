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

#ifndef __DBUSSIGNAL_H__
#define __DBUSSIGNAL_H__

#include "nsEmbedString.h"
#include "nsWeakReference.h"
#include "nsIXPConnect.h"

#include "DBusService.h"

//
// DBusSignal declarations
//

#define DBUSSIGNAL_CID \
{ \
    0xde515b88, \
    0xb8a0, \
    0x416e, \
    { 0xb4, 0x38, 0x52, 0x4e, 0xf7, 0x96, 0xfb, 0x13 } \
}

class DBusSignal : public IDBusSignal, public nsSupportsWeakReference
{
public:
    NS_DECL_ISUPPORTS
    NS_DECL_IDBUSSIGNAL

    DBusSignal(DBusService *aDBusService,
               PRUint32 aBusType,
               const nsACString& aInterface,
               const nsACString& aSignal,
               const nsACString& aSender,
               const nsACString& aObject,
               JSContext *cx);

private:
    ~DBusSignal();

    void filterEnable();
    void filterDisable();

protected:
    DBusService *mDBusService;
    PRUint32 mBusType;
    const nsCString mInterface;
    const nsCString mSignal;
    const nsCString mSender;
    const nsCString mObject;
    IDBusSignalObserver *mCallback;
    PRBool mEnabled;
    PRBool mFilterActive;
    JSContext *mJScx;
};


#endif /* __DBUSSIGNAL_H__ */

/* vim: set cindent ts=4 et sw=4: */
