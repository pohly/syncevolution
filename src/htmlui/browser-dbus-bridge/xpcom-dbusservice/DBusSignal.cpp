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

#include <stdio.h>

#include "IDBusService.h"

#include "DBusSignal.h"

#include "bdb-debug.h"


//
// DBusSignal implementation
//

NS_IMPL_ISUPPORTS2(DBusSignal, IDBusSignal, nsISupportsWeakReference)

DBusSignal::DBusSignal(DBusService *aDBusService,
                       PRUint32 aBusType,
                       const nsACString& aInterface,
                       const nsACString& aSignal,
                       const nsACString& aSender,
                       const nsACString& aObject,
                       JSContext *cx) :
    mDBusService(aDBusService),
    mBusType(aBusType),
    mInterface(aInterface),
    mSignal(aSignal),
    mSender(aSender),
    mObject(aObject),
    mCallback(0),
    mEnabled(PR_FALSE),
    mFilterActive(PR_FALSE),
    mJScx(cx)
{
    BDBLOG(("DBusSignal::DBusSignal()\n"));
    BDBLOG(("  mBusType   : %d\n", mBusType));
    BDBLOG(("  aInterface : %s\n", PromiseFlatCString(aInterface).get()));
    BDBLOG(("  aSignal    : %s\n", PromiseFlatCString(aSignal).get()));
    BDBLOG(("  aSender    : %s\n", PromiseFlatCString(aSender).get()));
    BDBLOG(("  aObject    : %s\n", PromiseFlatCString(aObject).get()));
    BDBLOG(("  mEnabled   : %d\n", mEnabled));
}

DBusSignal::~DBusSignal()
{
    BDBLOG(("DBusSignal::~DBusSignal()\n"));
    if (mFilterActive)
        filterDisable();
    if (mCallback)
        NS_RELEASE(mCallback);
}

NS_IMETHODIMP
DBusSignal::GetOnEmit(IDBusSignalObserver **aCallback)
{
    BDBLOG(("DBusSignal::GetOnEmit()\n"));
    NS_IF_ADDREF(*aCallback = mCallback);
    return NS_OK;
}

NS_IMETHODIMP
DBusSignal::SetOnEmit(IDBusSignalObserver *aCallback)
{
    BDBLOG(("DBusSignal::SetOnEmit(%08x)\n", (unsigned int)aCallback));
    if (mCallback)
    {
        if (mFilterActive)
            filterDisable();
        NS_RELEASE(mCallback);
    }
    mCallback = aCallback;
    NS_IF_ADDREF(mCallback);
    if (mEnabled && mCallback)
        filterEnable();
    return NS_OK;
}

NS_IMETHODIMP
DBusSignal::GetEnabled(PRBool *aEnabled)
{
    BDBLOG(("DBusSignal::GetEnabled()\n"));
    *aEnabled = mEnabled;
    return NS_OK;
}

NS_IMETHODIMP
DBusSignal::SetEnabled(PRBool aEnabled)
{
    BDBLOG(("DBusSignal::SetEnabled(%s)\n", aEnabled ? "PR_TRUE" : "PR_FALSE"));

    if (aEnabled && !mCallback)
    {
        BDBLOG(("  ERROR: trying to enable with no onEmit set!\n"));
        return NS_ERROR_NOT_AVAILABLE;
    }

    /* change filter state if necessary */
    if (mFilterActive && !aEnabled)
        filterDisable();
    if (!mFilterActive && aEnabled && mCallback)
        filterEnable();

    mEnabled = aEnabled;

    return NS_OK;
}

NS_IMETHODIMP
DBusSignal::GetBusType(PRUint32 *aBusType)
{
    *aBusType = mBusType;
    return NS_OK;
}

NS_IMETHODIMP
DBusSignal::GetInterfaceName(nsACString& aInterface)
{
    aInterface.Assign(mInterface);
    return NS_OK;
}

NS_IMETHODIMP
DBusSignal::GetSignalName(nsACString& aSignal)
{
    aSignal.Assign(mSignal);
    return NS_OK;
}

NS_IMETHODIMP
DBusSignal::GetSender(nsACString& aSender)
{
    aSender.Assign(mSender);
    return NS_OK;
}

NS_IMETHODIMP
DBusSignal::GetObjectPath(nsACString& aObject)
{
    aObject.Assign(mObject);
    return NS_OK;
}

NS_IMETHODIMP
DBusSignal::GetJSContext(JSContext **aJSContext)
{
    *aJSContext = mJScx;
    return NS_OK;
}

void
DBusSignal::filterEnable()
{
    BDBLOG(("DBusSignal::filterEnable()\n"));
    mFilterActive = PR_TRUE;

    mDBusService->AddSignalObserver(this);
}

void
DBusSignal::filterDisable()
{
    BDBLOG(("DBusSignal::filterDisable()\n"));
    mFilterActive = PR_FALSE;
    
    mDBusService->RemoveSignalObserver(this);
}

/* vim: set cindent ts=4 et sw=4: */
