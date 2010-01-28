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

/* Functions to convert between D-Bus types and JSCore types */

#ifndef __JSCOREBUS_CONVERT_H___
#define __JSCOREBUS_CONVERT_H___
 
#include <dbus/dbus.h>
#include <JavaScriptCore/JavaScript.h>

/* The number class names are in guestimated usage frequency order to speed up
 * iteration over them.
 */
#define JSCOREBUS_N_NUMBER_CLASSES 8
char *jscorebus_number_class_names[JSCOREBUS_N_NUMBER_CLASSES];
int jscorebus_number_class_types[JSCOREBUS_N_NUMBER_CLASSES];

/* Append JSValues to a D-Bus message iter */
gboolean jsvalue_array_append_to_message_iter(JSContextRef context,
                                              const JSValueRef jsvalues[],
                                              int n_values,
                                              DBusMessageIter *iter,
                                              const char *signature);
gboolean jsvalue_append_to_message_iter(JSContextRef context,
                                        const JSValueRef jsvalue,
                                        DBusMessageIter *iter,
                                        const char *signature);

/* Call a JS function with the arguments from a message */
void call_function_with_message_args(JSContextRef context,
                                     JSObjectRef thisObject,
                                     JSObjectRef function,
                                     DBusMessage *message);

/* To JavaScript types */
JSValueRef jsvalue_from_message_iter(JSContextRef context,
                                     DBusMessageIter *iter);
JSObjectRef function_from_jsvalue(JSContextRef context,
                                  JSValueRef value,
                                  JSValueRef* exception);

/* To D-Bus types */
char *string_from_jsstring(JSContextRef context, JSStringRef jsstring);
char *string_from_jsvalue(JSContextRef context, JSValueRef jsvalue);
dbus_uint64_t jsvalue_to_number_value (JSContextRef context,
                                       JSValueRef jsvalue,
                                       int *number_type);

/* JSValue to D-Bus signature (autodetection) */
char *jsvalue_to_signature(JSContextRef context, JSValueRef jsvalue);
gboolean
jsarray_get_signature(JSContextRef context,
                      JSValueRef jsvalue,
                      JSPropertyNameArrayRef propNames,
                      char **signature);
gboolean
jsdict_get_signature(JSContextRef context,
                     JSValueRef jsvalue,
                     JSPropertyNameArrayRef propNames,
                     char **signature);


/* Helper functions */
gboolean jsvalue_typeof(JSContextRef context,
                        JSValueRef jsvalue,
                        const char *type);
gboolean jsvalue_instanceof(JSContextRef context,
                            JSValueRef jsvalue,
                            const char *constructor);

/* Variant data carrier */
typedef struct _variant_data
{
  char *signature;
  JSValueRef value;
} variant_data_t;

#endif /* __JSCOREBUS_CONVERT_H___ */

