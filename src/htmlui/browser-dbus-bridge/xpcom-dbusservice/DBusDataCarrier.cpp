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
#include <dbus/dbus.h>

#include "DBusDataCarrier.h"

#include "bdb-debug.h"


NS_IMPL_ISUPPORTS1(DBusDataCarrier, IDBusDataCarrier)

DBusDataCarrier::DBusDataCarrier() :
  mSig(),
  mValue(0)
{
    BDBLOG(("%s\n", __FUNCTION__));
}

DBusDataCarrier::~DBusDataCarrier()
{
    BDBLOG(("%s\n", __FUNCTION__));
}

/* attribute ACString type; */
NS_IMETHODIMP DBusDataCarrier::GetType(nsACString & aType)
{
    BDBLOG(("%s\n", __FUNCTION__));
    aType.Assign(mType);
    return NS_OK;
}
NS_IMETHODIMP DBusDataCarrier::SetType(const nsACString & aType)
{
    BDBLOG(("%s\n", __FUNCTION__));
    mType.Assign(aType);
    return NS_OK;
}

/* attribute ACString signature; */
NS_IMETHODIMP DBusDataCarrier::GetSignature(nsACString & aSignature)
{
    BDBLOG(("%s\n", __FUNCTION__));
    aSignature.Assign(mSig);
    return NS_OK;
}
NS_IMETHODIMP DBusDataCarrier::SetSignature(const nsACString & aSignature)
{
    BDBLOG(("%s\n", __FUNCTION__));
    mSig.Assign(aSignature);
    return NS_OK;
}

/* attribute nsIVariant value; */
NS_IMETHODIMP DBusDataCarrier::GetValue(nsIVariant * *aValue)
{
    BDBLOG(("%s\n", __FUNCTION__));
    *aValue = mValue.get();
    NS_IF_ADDREF(*aValue);
    return NS_OK;
}
NS_IMETHODIMP DBusDataCarrier::SetValue(nsIVariant * aValue)
{
    BDBLOG(("%s\n", __FUNCTION__));
    mValue = aValue;
    return NS_OK;
}

