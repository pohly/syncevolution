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

#include <glib.h>
#include <dbus/dbus.h>
#include <JavaScriptCore/JavaScript.h>

#include "jscorebus-classfactory.h"
#include "jscorebus-marshal.h"

gboolean jsvalue_typeof(JSContextRef context,
                        JSValueRef jsvalue,
                        const char *type)
{
  const JSClassDefinition *jsclassdef;
  JSClassRef jsclass;
  
  jsclassdef = jsclassdef_lookup(type);
  if (G_UNLIKELY(jsclassdef == NULL))
    return FALSE;
  jsclass = jsclass_lookup(jsclassdef);

  return JSValueIsObjectOfClass(context, jsvalue, jsclass);
}

gboolean jsvalue_instanceof(JSContextRef context,
                            JSValueRef jsvalue,
                            const char *constructor)
{
  JSStringRef property;
  JSObjectRef ctor;

  property = JSStringCreateWithUTF8CString(constructor);
  ctor = JSValueToObject(context,
                         JSObjectGetProperty(context,
                                             JSContextGetGlobalObject(context),
                                             property,
                                             NULL),
                         NULL);
  JSStringRelease(property);

  return JSValueIsInstanceOfConstructor(context, jsvalue, ctor, NULL);
}

char *jsvalue_to_signature(JSContextRef context,
                           JSValueRef jsvalue)
{
  char *signature = NULL;
  
  switch (JSValueGetType(context, jsvalue))
  {
    case kJSTypeBoolean:
      {
        signature = g_strdup(DBUS_TYPE_BOOLEAN_AS_STRING);
        break;
      }
    case kJSTypeNumber:
      {
        /* JavaScript numbers are always doubles */
        signature = g_strdup(DBUS_TYPE_DOUBLE_AS_STRING);
        break;
      }
    case kJSTypeString:
      {
        signature = g_strdup(DBUS_TYPE_STRING_AS_STRING);
        break;
      }
    case kJSTypeObject:
      {
        int i;
        char *dict_signature = NULL;
        JSPropertyNameArrayRef propnames;

        /* Check for number types */
        for (i = 0; i < JSCOREBUS_N_NUMBER_CLASSES; i++)
        {
          if (jsvalue_typeof(context, jsvalue, jscorebus_number_class_names[i]))
          {
            switch (jscorebus_number_class_types[i])
            {
#define NUMBER_SIGNATURE(t) \
              case DBUS_TYPE_## t: \
                signature = g_strdup(DBUS_TYPE_## t ##_AS_STRING); \
                break;
              NUMBER_SIGNATURE(UINT32)
              NUMBER_SIGNATURE(INT32)
              NUMBER_SIGNATURE(BYTE)
              NUMBER_SIGNATURE(UINT64)
              NUMBER_SIGNATURE(INT64)
              NUMBER_SIGNATURE(UINT16)
              NUMBER_SIGNATURE(INT16)

              default:
                break;
            }
          }
        }

        /* Check for arrays */
        if (jsvalue_instanceof(context, jsvalue, "Array"))
        {
          char *array_signature;

          propnames = JSObjectCopyPropertyNames(context, (JSObjectRef)jsvalue);
          if (!jsarray_get_signature(context, jsvalue, propnames, &array_signature))
          { 
            g_warning("Could not create array signature");
            JSPropertyNameArrayRelease(propnames);
            break;
          }
          signature = g_strdup_printf("a%s", array_signature);
          g_free(array_signature);
          JSPropertyNameArrayRelease(propnames);
          break;
        }

        /* Check variants */
        if (jsvalue_typeof(context, jsvalue, "DBusVariant"))
        {
          signature = g_strdup("v");
          break;
        }

        if (jsvalue_typeof(context, jsvalue, "DBusObjectPath"))
        {
          signature = g_strdup(DBUS_TYPE_OBJECT_PATH_AS_STRING);
          break;
        }

        if (jsvalue_typeof(context, jsvalue, "DBusSignature"))
        {
          signature = g_strdup(DBUS_TYPE_SIGNATURE_AS_STRING);
          break;
        }

        /* Check structs */
        if (jsvalue_typeof(context, jsvalue, "DBusStruct"))
        {
          JSObjectRef value = (JSObjectRef)JSObjectGetPrivate((JSObjectRef)jsvalue);
          propnames = JSObjectCopyPropertyNames(context, value);
          jsstruct_get_signature(context, value, propnames, &signature);
          JSPropertyNameArrayRelease(propnames);
          break;
        }

        /* Default conversion is to dict */
        propnames = JSObjectCopyPropertyNames(context, (JSObjectRef)jsvalue);
        jsdict_get_signature(context, jsvalue, propnames, &dict_signature);
        if (dict_signature != NULL)
        {
          signature = g_strdup_printf("a%s", dict_signature);
          g_free(dict_signature);
        }
        JSPropertyNameArrayRelease(propnames);
        break;
      }
    case kJSTypeUndefined:
    case kJSTypeNull:
    default:
      g_warning("Signature lookup failed for unsupported type %i", JSValueGetType(context, jsvalue));
      break;
  }
  return signature;
}

gboolean
jsarray_get_signature(JSContextRef context,
                      JSValueRef jsvalue,
                      JSPropertyNameArrayRef propNames,
                      char **signature)
{
  int i, props;
  
  *signature = NULL;
  props = JSPropertyNameArrayGetCount(propNames);
  /* Arrays are restricted to single complete types so we only need to look
   * at the first property
   */
  if (props > 0)
  {
    *signature = jsvalue_to_signature(context,
      JSObjectGetPropertyAtIndex(context, (JSObjectRef)jsvalue, 0, NULL));
  }
  return *signature == NULL ? FALSE : TRUE;
}

gboolean
jsdict_get_signature(JSContextRef context,
                     JSValueRef jsvalue,
                     JSPropertyNameArrayRef propNames,
                     char **signature)
{
  int i, props;
  
  *signature = NULL;
  props = JSPropertyNameArrayGetCount(propNames);
  /* Dicts support only string keys currently, though numbers would be another
   * possibility...
   */
  if (props > 0)
  {
    char **signatures = g_new0(char*, 5);
    
    signatures[0] = g_strdup(DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING);
    signatures[1] = g_strdup(DBUS_TYPE_STRING_AS_STRING);
    signatures[2] = jsvalue_to_signature(context,
      JSObjectGetProperty(context, (JSObjectRef)jsvalue,
        JSPropertyNameArrayGetNameAtIndex(propNames, 0), NULL));
    signatures[3] = g_strdup(DBUS_DICT_ENTRY_END_CHAR_AS_STRING);

    *signature = g_strjoinv(NULL, signatures);
    g_strfreev(signatures);
  }
  return *signature == NULL ? FALSE : TRUE;
}

gboolean
jsstruct_get_signature(JSContextRef context,
                       JSValueRef jsvalue,
                       JSPropertyNameArrayRef propNames,
                       char **signature)
{
  int props;
  
  *signature = NULL;
  props = JSPropertyNameArrayGetCount(propNames);
  if (props > 0)
  {
    char **signatures = g_new0(char*, props + 2);
    int i = 0;
    signatures[i] = g_strdup(DBUS_STRUCT_BEGIN_CHAR_AS_STRING);
    while (i < props)
    {
      signatures[i+1] = jsvalue_to_signature(context,
        JSObjectGetProperty(context, (JSObjectRef)jsvalue,
          JSPropertyNameArrayGetNameAtIndex(propNames, i++), NULL));
    }
    signatures[props + 1] = g_strdup(DBUS_STRUCT_END_CHAR_AS_STRING);

    *signature = g_strjoinv(NULL, signatures);
    g_strfreev(signatures);
  }
  return *signature == NULL ? FALSE : TRUE;
}

