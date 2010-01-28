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

#include <string.h>
 
#include <glib.h>
#include <dbus/dbus.h>
#include <JavaScriptCore/JavaScript.h>

#include "jscorebus-marshal.h"
#include "jscorebus-method.h"
#include "jscorebus-signal.h"

/* Globals */
/* TODO: Not like this :( */
static DBusConnection *session;
static DBusConnection *system;
static JSGlobalContextRef gcontext = NULL;



/* Getters for the bus type properties */
static
JSValueRef _get_bus_type (JSContextRef context,
                          JSObjectRef object,
                          JSStringRef propertyName,
                          JSValueRef *exception)
{
  if (JSStringIsEqualToUTF8CString(propertyName, "SESSION"))
  {
    return JSValueMakeNumber(context, DBUS_BUS_SESSION);
  }
  if (JSStringIsEqualToUTF8CString(propertyName, "SYSTEM"))
  {
    return JSValueMakeNumber(context, DBUS_BUS_SYSTEM);
  }
  
  return JSValueMakeUndefined(context);
}

/* Static members */
static const
JSStaticValue dbus_jsclass_staticvalues[] = 
{
  { "SESSION", _get_bus_type, NULL, kJSPropertyAttributeReadOnly },
  { "SYSTEM",  _get_bus_type, NULL, kJSPropertyAttributeReadOnly },
  { NULL, NULL, NULL, 0 }
};

static
void _number_finalize (JSObjectRef object)
{
  g_free(JSObjectGetPrivate(object));
}

static inline
JSValueRef _get_number_object (JSContextRef context,
                               JSObjectRef function,
                               JSObjectRef thisObject,
                               size_t argumentCount,
                               const JSValueRef arguments[],
                               JSValueRef *exception,
                               JSClassRef number_class)
{
  dbus_uint64_t *value;
  
  if (argumentCount != 1)
  {
    /* TODO: set exception */
    return JSValueMakeUndefined(context);
  }

  /* dbus_uint64_t should be large enough to hold any number so we just
   * carry the value in private data as a pointer to dbus_uint64_t.
   * The object class tells us later which type it is.
   */
  value = g_new0(dbus_uint64_t, 1);
  *value = (dbus_uint64_t)JSValueToNumber(context, arguments[0], NULL);

  return JSObjectMake(context, number_class, value);
}

JSValueRef _convert_number_object(JSContextRef context,
                                  JSObjectRef object,
                                  JSType type,
                                  JSValueRef* exception)
{
  /* FIXME: this isn't called at all... */
  g_debug("%s(%p to %d)", __FUNCTION__, object, type);
#if 0
  switch (type)
  {
    case kJSTypeNumber:
      {
        dbus_uint64_t value = jsvalue_to_number_value(context, object, NULL);
        return JSValueMakeNumber(context, (double)value);
      }
    case kJSTypeString:
      {
        JSValueRef jsvalue;
        JSStringRef jsstr;
        char *str;
        dbus_uint64_t value = jsvalue_to_number_value(context, object, NULL);
        str = g_strdup_printf("%llu", value);
        jsstr = JSStringCreateWithUTF8CString(str);
        g_free(str);
        jsvalue = JSValueMakeString(context, jsstr);
        JSStringRelease(jsstr);
        return jsvalue;
      }
    default:
      break;
  }
#endif
  return NULL;
}

#define MAKE_NUMBER_CLASS_AND_GETTER(classname, shortname) \
  static const JSClassDefinition shortname ##_jsclass_def = \
  { \
    0, kJSClassAttributeNone, classname, \
    NULL, NULL, NULL, NULL, _number_finalize, NULL, NULL, \
    NULL, NULL, NULL, NULL, NULL, NULL, _convert_number_object \
  }; \
  \
  static JSValueRef _get_ ##shortname (JSContextRef context, \
                                    JSObjectRef function, \
                                    JSObjectRef thisObject, \
                                    size_t argumentCount, \
                                    const JSValueRef arguments[], \
                                    JSValueRef *exception) \
  { \
    return _get_number_object(context, function, thisObject, \
                              argumentCount, arguments, exception, \
                              (JSClassRef)jsclass_lookup(&shortname ##_jsclass_def)); \
  }

MAKE_NUMBER_CLASS_AND_GETTER("DBusUInt32", uint32)
MAKE_NUMBER_CLASS_AND_GETTER("DBusInt32", int32)
MAKE_NUMBER_CLASS_AND_GETTER("DBusByte", byte)
MAKE_NUMBER_CLASS_AND_GETTER("DBusUInt64", uint64)
MAKE_NUMBER_CLASS_AND_GETTER("DBusInt64", int64)
MAKE_NUMBER_CLASS_AND_GETTER("DBusUInt16", uint16)
MAKE_NUMBER_CLASS_AND_GETTER("DBusInt16", int16)

/* Variants */

static
void _variant_finalize (JSObjectRef object)
{
  variant_data_t *data = (variant_data_t *)JSObjectGetPrivate(object);
  g_assert(data != NULL);
  g_free(data->signature);
  JSValueUnprotect(gcontext, data->value);
  g_free(data);
}

static const JSClassDefinition variant_jsclass_def =
{
  0, kJSClassAttributeNone, "DBusVariant",
  NULL, NULL, NULL, NULL, _variant_finalize, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static
JSValueRef _construct_variant (JSContextRef context,
                               JSObjectRef function,
                               JSObjectRef thisObject,
                               size_t argumentCount,
                               const JSValueRef arguments[],
                               JSValueRef *exception)
{
  variant_data_t *data;

  if (argumentCount < 2)
  {
    /* TODO: set exception */
    return JSValueMakeUndefined(context);
  }

  data = g_new0(variant_data_t, 1);
  data->signature = string_from_jsvalue(context, arguments[0]);
  data->value = arguments[1];
  JSValueProtect(context, data->value);

  return JSObjectMake(context, (JSClassRef)jsclass_lookup(&variant_jsclass_def), (void*)data);
}

static const JSClassDefinition struct_jsclass_def =
{
  0, kJSClassAttributeNone, "DBusStruct",
  NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static
JSValueRef _construct_struct (JSContextRef context,
                              JSObjectRef function,
                              JSObjectRef thisObject,
                              size_t argumentCount,
                              const JSValueRef arguments[],
                              JSValueRef *exception)
{
  if (argumentCount != 1)
  {
    /* TODO: set exception */
    return JSValueMakeUndefined(context);
  }

  /* Just carry the object in private data */
  return JSObjectMake(context,
                      (JSClassRef)jsclass_lookup(&struct_jsclass_def),
                      (void*)arguments[0]);
}

static const JSClassDefinition object_path_jsclass_def =
{
  0, kJSClassAttributeNone, "DBusObjectPath",
  NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static
gboolean is_valid_path (const char *path)
{
  const char *this = path;
  const char *prev = this;

  if (path == NULL || strlen(path) == 0)
    return FALSE;

  /* MUST begin with zero */
  if (*this++ != '/')
    return FALSE;

  /* The path is guranteed to be null-terminated */
  while (*this != '\0')
  {
    /* Two slashes can't be together */
    if (*this == '/' && *prev == '/')
    {
      return FALSE;
    } else if (!(((*this) >= '0' && (*this) <= '9') ||
                 ((*this) >= 'A' && (*this) <= 'Z') ||
                 ((*this) >= 'a' && (*this) <= 'z') ||
                  (*this) == '_' || (*this) == '/')) {
      return FALSE;
    }
    prev = this;
    this++;
  }
  
  return TRUE;
}

static
JSValueRef _construct_object_path (JSContextRef context,
                               JSObjectRef function,
                               JSObjectRef thisObject,
                               size_t argumentCount,
                               const JSValueRef arguments[],
                               JSValueRef *exception)
{
  const char *path;

  if (argumentCount != 1)
  {
    /* TODO: set exception */
    return JSValueMakeUndefined(context);
  }

  /* D-Bus doesn't like invalid object paths _at all_, so instead of risking
   * disconnection, we'll validate the path now.
   */
  path = string_from_jsvalue(context, arguments[0]);
  if (!is_valid_path(path))
  {
    g_free((gpointer)path);
    /* TODO: set exception */
    return JSValueMakeUndefined(context);
  }
  g_free((gpointer)path);

  /* Just carry the value in private data */
  return JSObjectMake(context,
                      (JSClassRef)jsclass_lookup(&object_path_jsclass_def),
                      (void*)arguments[0]);
}

static const JSClassDefinition signature_jsclass_def =
{
  0, kJSClassAttributeNone, "DBusSignature",
  NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static
JSValueRef _construct_signature (JSContextRef context,
                                 JSObjectRef function,
                                 JSObjectRef thisObject,
                                 size_t argumentCount,
                                 const JSValueRef arguments[],
                                 JSValueRef *exception)
{
  if (argumentCount != 1)
  {
    /* TODO: set exception */
    return JSValueMakeUndefined(context);
  }

  /* Just carry the value in private data */
  return JSObjectMake(context,
                      (JSClassRef)jsclass_lookup(&signature_jsclass_def),
                      (void*)arguments[0]);
}

#define JSVALUE_TO_CONNECTION(ctx, val) (JSValueToNumber(ctx, val, NULL) == DBUS_BUS_SYSTEM) ? system : session

static
JSValueRef getMethod (JSContextRef context,
                      JSObjectRef function,
                      JSObjectRef thisObject,
                      size_t argumentCount,
                      const JSValueRef arguments[],
                      JSValueRef *exception)
{
  JSGlobalContextRef global_context = JSObjectGetPrivate(thisObject);
  
  if (argumentCount < 4)
  {
    /* TODO: set exception */
    return JSValueMakeUndefined(context);
  }

  return jscorebus_create_method(global_context,
                                 JSVALUE_TO_CONNECTION(context, arguments[0]),
                                 string_from_jsvalue(context, arguments[1]),
                                 string_from_jsvalue(context, arguments[2]),
                                 string_from_jsvalue(context, arguments[3]),
                                 argumentCount > 4 ? 
                                   string_from_jsvalue(context, arguments[4])
                                   : NULL,
                                 argumentCount > 5 ? 
                                   string_from_jsvalue(context, arguments[5])
                                   : NULL,
                                 argumentCount > 6 ? 
                                   JSValueToObject(context, arguments[6], NULL)
                                   : NULL,
                                 exception);
}

static
JSValueRef getSignal (JSContextRef context,
                      JSObjectRef function,
                      JSObjectRef thisObject,
                      size_t argumentCount,
                      const JSValueRef arguments[],
                      JSValueRef *exception)
{
  JSGlobalContextRef global_context = JSObjectGetPrivate(thisObject);

  if (argumentCount < 3)
  {
    /* TODO: set exception */
    return JSValueMakeUndefined(context);
  }

  return jscorebus_create_signal(global_context,
                                 JSVALUE_TO_CONNECTION(context, arguments[0]),
                                 string_from_jsvalue(context, arguments[1]),
                                 string_from_jsvalue(context, arguments[2]),
                                 argumentCount > 3 ? 
                                   string_from_jsvalue(context, arguments[3])
                                   : NULL,
                                 argumentCount > 4 ? 
                                   string_from_jsvalue(context, arguments[4])
                                   : NULL,
                                 argumentCount > 5 ? 
                                   JSValueToObject(context, arguments[5], NULL)
                                   : NULL,
                                 exception);
}

static
JSValueRef emitSignal (JSContextRef context,
                       JSObjectRef function,
                       JSObjectRef thisObject,
                       size_t argumentCount,
                       const JSValueRef arguments[],
                       JSValueRef *exception)
{
  DBusMessage *message;
  DBusConnection *connection;
  char *path;
  char *interface;
  char *member;
  char *signature;

  if (argumentCount < 4)
  {
    /* TODO: set exception */
    g_warning("Not enough arguments for emitSignal");
    return JSValueMakeBoolean(context, FALSE);
  }

  connection = JSVALUE_TO_CONNECTION(context, arguments[0]);
  path       = string_from_jsvalue(context, arguments[1]);
  interface  = string_from_jsvalue(context, arguments[2]);
  member     = string_from_jsvalue(context, arguments[3]);

  if (connection == NULL || path == NULL
   || interface == NULL || member == NULL)
  {
    g_free(path);
    g_free(interface);
    g_free(member);
    g_warning("Buggy application: Required emitSignal() argument was null");
    return JSValueMakeBoolean(context, FALSE);
  }

  message = dbus_message_new_signal(path, interface, member);

  g_free(path);
  g_free(interface);
  g_free(member);

  if (message == NULL)
  {
    return JSValueMakeBoolean(context, FALSE);
  }

  if (argumentCount > 5)
  {
    int i, c;
    JSValueRef *args;
    DBusMessageIter iter;

    /* "Splice" the array */
    args = g_new(JSValueRef, argumentCount - 5);
    c = 0;
    for (i = 5; i < argumentCount; i++)
      args[c++] = arguments[i];

    /* Push arguments to the message */
    signature = string_from_jsvalue(context, arguments[4]);
    dbus_message_iter_init_append (message, &iter);
    if (!jsvalue_array_append_to_message_iter(context,
                                              args, c,
                                              &iter, signature))
    {
      /* TODO: set exception */
      for (i = 0; i < c; i++)
        args[i] = NULL;
      g_free(args);
      g_free(signature);
      dbus_message_unref(message);
      return JSValueMakeBoolean(context, FALSE);
    }

    for (i = 0; i < c; i++)
      args[i] = NULL;
    g_free(args);
    g_free(signature);
  }

  if (dbus_connection_send(connection, message, NULL))
  {
    dbus_message_unref(message);
    return JSValueMakeBoolean(context, TRUE);
  }

  dbus_message_unref(message);
  return JSValueMakeBoolean(context, FALSE);
}

static
void dbus_finalize(JSObjectRef object)
{
  JSObjectSetPrivate(object, NULL);
}

static const
JSStaticFunction dbus_jsclass_staticfuncs[] = 
{
  /* Type constructors */
  { "Int32",   _get_int32, kJSPropertyAttributeReadOnly },
  { "UInt32",  _get_uint32, kJSPropertyAttributeReadOnly },
  { "Byte",    _get_byte, kJSPropertyAttributeReadOnly },
  { "Int64",   _get_int64, kJSPropertyAttributeReadOnly },
  { "UInt64",  _get_uint64, kJSPropertyAttributeReadOnly },
  { "Int16",   _get_int16, kJSPropertyAttributeReadOnly },
  { "UInt16",  _get_uint16, kJSPropertyAttributeReadOnly },
  { "ObjectPath", _construct_object_path, kJSPropertyAttributeReadOnly },
  { "Signature", _construct_signature, kJSPropertyAttributeReadOnly },
  { "Variant", _construct_variant, kJSPropertyAttributeReadOnly },
  { "Struct", _construct_struct, kJSPropertyAttributeReadOnly },

  /* Methods */
  { "getMethod", getMethod, kJSPropertyAttributeReadOnly },
  { "getSignal", getSignal, kJSPropertyAttributeReadOnly },
  { "emitSignal", emitSignal, kJSPropertyAttributeReadOnly },
  { NULL, NULL, 0 }
};

static
JSObjectRef dbus_constructor (JSContextRef context,
                              JSObjectRef constructor,
                              size_t argumentCount,
                              const JSValueRef arguments[],
                              JSValueRef* exception);

/* The DBus Class */
static const
JSClassDefinition dbus_jsclass_def =
{
  0,
  kJSClassAttributeNone,
  "DBus",
  NULL,

  dbus_jsclass_staticvalues,
  dbus_jsclass_staticfuncs,
  
  NULL,
  dbus_finalize,
  
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  
  NULL,
  NULL,
  NULL,
  NULL
};

static
JSObjectRef dbus_constructor (JSContextRef context,
                              JSObjectRef constructor,
                              size_t argumentCount,
                              const JSValueRef arguments[],
                              JSValueRef* exception)
{
  return JSObjectMake(context,
                      (JSClassRef)jsclass_lookup(&dbus_jsclass_def),
                      gcontext);
}

/**
 * Public API
 */
 
void jscorebus_init(DBusConnection *psession, DBusConnection *psystem)
{
  session = psession;
  system = psystem;

  jsclassdef_insert("DBus", &dbus_jsclass_def);

#define INIT_NUMBER_CLASS(name, def, type, num) \
  jsclassdef_insert(name, def); \
  jscorebus_number_class_names[num] = name; \
  jscorebus_number_class_types[num] = type;

  INIT_NUMBER_CLASS("DBusInt32",  &int32_jsclass_def,  DBUS_TYPE_INT32,  0);
  INIT_NUMBER_CLASS("DBusUInt32", &uint32_jsclass_def, DBUS_TYPE_UINT32, 1);
  INIT_NUMBER_CLASS("DBusByte",   &byte_jsclass_def,   DBUS_TYPE_BYTE,   3);
  INIT_NUMBER_CLASS("DBusUInt64", &uint64_jsclass_def, DBUS_TYPE_UINT64, 4);
  INIT_NUMBER_CLASS("DBusInt64",  &int64_jsclass_def,  DBUS_TYPE_INT64,  5);
  INIT_NUMBER_CLASS("DBusUInt16", &uint16_jsclass_def, DBUS_TYPE_UINT16, 6);
  INIT_NUMBER_CLASS("DBusInt16",  &int16_jsclass_def,  DBUS_TYPE_INT16,  7);

  jsclassdef_insert("DBusObjectPath", &object_path_jsclass_def);
  jsclassdef_insert("DBusSignature", &signature_jsclass_def);

  jsclassdef_insert("DBusVariant", &variant_jsclass_def);
  jsclassdef_insert("DBusStruct", &struct_jsclass_def);

}

void jscorebus_export(JSGlobalContextRef context)
{
  JSObjectRef globalObject;
  JSObjectRef dbus;
  JSStringRef jsstr;

  dbus = JSObjectMakeConstructor(context,
                                 (JSClassRef)jsclass_lookup(&dbus_jsclass_def),
                                 dbus_constructor);
  gcontext = context;

  globalObject = JSContextGetGlobalObject(context);
  jsstr = JSStringCreateWithUTF8CString("DBus");
  JSObjectSetProperty(context, globalObject,
                      jsstr, dbus,
                      kJSPropertyAttributeNone, NULL);
  JSStringRelease(jsstr);
}

