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

#ifndef __DBUSMARSHALING_H__
#define __DBUSMARSHALING_H__

#include <dbus/dbus.h>

#include "nsIVariant.h"
#include "nsIMutableArray.h"

#include "jsapi.h"

/* Acquire signature from a variant */
void getSignatureFromVariant(JSContext* cx, nsIVariant *aVariant, nsCString &aResult);

/* Add a variant to a D-Bus message iter */
void addVariantToIter(JSContext* cx, nsIVariant *aVariant, DBusMessageIter *aIter, DBusSignatureIter *aSigIter);

/* Get array of variants from a D-Bus message iter */
already_AddRefed<nsIMutableArray> getArrayFromIter(JSContext* cx, DBusMessageIter *aIter);

#endif
