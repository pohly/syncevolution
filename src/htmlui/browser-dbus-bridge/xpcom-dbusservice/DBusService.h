/**
 * Browser D-Bus Bridge, XPCOM version
 *
 * Copyright © 2008 Movial Creative Technologies Inc
 *  Contact: Movial Creative Technologies Inc, <info@movial.com>
 *  Authors: Lauri Mylläri, <lauri.myllari@movial.com>
 *           Kalle Vahlman, <kalle.vahlman@movial.com>
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

#ifndef __DBUSSERVICE_H__
#define __DBUSSERVICE_H__

#include <dbus/dbus-glib-lowlevel.h>

#include "nsTArray.h"
#include "nsCOMArray.h"
#include "nsIWeakReference.h"
#include "nsIWeakReferenceUtils.h"

typedef PRUint8 uint8; /* otherwise pldhash.h:199 is sad */
#include "nsClassHashtable.h"

//
// DBusService declarations
//

#define DBUSSERVICE_CID \
{ \
    0xe3b49db1, \
    0x5754, \
    0x4330, \
    { 0x92, 0xcd, 0xab, 0xe8, 0xf7, 0xea, 0x54, 0x3d } \
}

class DBusService : public IDBusService
{
public:
    NS_DECL_ISUPPORTS
    NS_DECL_IDBUSSERVICE

    static DBusService *GetSingleton();

    DBusService();

    DBusMessage *SendWithReplyAndBlock(PRUint32 aConnType,
                                       DBusMessage *aMessage,
                                       PRUint32 aTimeout,
                                       DBusError *aError);
    DBusPendingCall *SendWithReply(PRUint32 aConnType,
                                   DBusMessage *aMessage,
                                   PRUint32 aTimeout);
    void AddSignalObserver(IDBusSignal *aSignal);
    void RemoveSignalObserver(IDBusSignal *aSignal);
    
    void SetInsideEmit(PRBool inside) { mInsideEmit = inside; };
    void CheckSignalObserverQueue();
    
private:
    ~DBusService();

    DBusConnection *GetConnection(PRUint32 aConnType);

    JSContext *GetCurrentJSContext();

    DBusConnection *mSystemBus;
    DBusConnection *mSessionBus;

    nsClassHashtable<nsCStringHashKey, nsTArray<nsWeakPtr> > mSystemBusSignalObservers;
    nsClassHashtable<nsCStringHashKey, nsTArray<nsWeakPtr> > mSessionBusSignalObservers;
    PRBool mSystemBusHasFilter;
    PRBool mSessionBusHasFilter;
    
    /* We need to queue changes to signal observers within onemit callbacks
     * so that we don't alter the list while iterating over it...
     */
    PRBool mInsideEmit;
    nsTArray<IDBusSignal*> mRemovedSignals;
    nsTArray<IDBusSignal*> mAddedSignals;
};

#endif /* __DBUSSERVICE_H__ */

/* vim: set cindent ts=4 et sw=4: */
