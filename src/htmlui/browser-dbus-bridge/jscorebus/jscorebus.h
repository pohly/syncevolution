/**
 * Browser D-Bus Bridge, JavaScriptCore version
 *
 * Copyright © 2008 Movial Creative Technologies Inc
 *  Contact: Movial Creative Technologies Inc, <info@movial.com>
 *  Authors: Kalle Vahlman, <kalle.vahlman@movial.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef __JSCOREBUS_H___
#define __JSCOREBUS_H___

#ifdef __cplusplus
extern "C"{
#endif

#include <dbus/dbus.h>
#include <JavaScriptCore/JavaScript.h>

/**
 * Initialize the JSCoreBus bindings.
 *
 * Pass NULL if you wish to omit one of the connections.
 */
void jscorebus_init(DBusConnection *session, DBusConnection *system);

/**
 * Export the D-Bus object to the JavaScript execution context.
 */
void jscorebus_export(JSGlobalContextRef context);

#ifdef __cplusplus
}
#endif

#endif /* __JSCOREBUS_H___ */

