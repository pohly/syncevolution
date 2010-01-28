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

#include "jscorebus-classfactory.h"
#include "jscorebus-marshal.h"

static gboolean
jsvalue_append_basic(JSContextRef context,
                     JSValueRef jsvalue,
                     int type,
                     DBusMessageIter *iter);


gboolean jsvalue_array_append_to_message_iter (JSContextRef context,
                                               const JSValueRef jsvalues[],
                                               int n_values,
                                               DBusMessageIter *iter,
                                               const char *signature)
{
  DBusSignatureIter siter;
  DBusError error;
  char *sig = (char*) signature; /* This is a bit silly, I know */
  int i = 0;

  /* If there is no signature, we need to do some autodetection */
  if (signature == NULL)
  {
    char **parts;
    parts = g_new0(char*, n_values + 1);
    for (i = 0; i < n_values; i++)
    {
      parts[i] = jsvalue_to_signature(context, jsvalues[i]);
    }
    sig = g_strjoinv(NULL, parts);
    g_strfreev(parts);
  }

  /* If there *still* is no signature, or it is empty, we bork.
   * Empty messages have no business going through this code path.
   */
  if (sig == NULL || strlen(sig) == 0)
  {
    g_warning("Could not autodetect signature for message arguments!");
    g_free(sig);
    return FALSE;
  }

  /* First of all, we need to validate the signature */
  dbus_error_init(&error);
  if (!dbus_signature_validate(sig, &error))
  {
    g_warning("%s", error.message);
    if (sig != signature)
      g_free(sig);
    return FALSE;
  }
  
  dbus_signature_iter_init(&siter, sig);
  i = 0;
  do {
    char *arg_sig = dbus_signature_iter_get_signature(&siter);
    if (!jsvalue_append_to_message_iter(context, jsvalues[i++], iter, arg_sig))
    {
      g_warning("Appending '%s' to message failed!", arg_sig);
      dbus_free(arg_sig);
      if (sig != signature)
        g_free(sig);
      return FALSE;
    }
    dbus_free(arg_sig);
  } while (dbus_signature_iter_next(&siter));
  
  if (sig != signature)
    g_free(sig);

  return TRUE;
}

gboolean jsvalue_append_to_message_iter(JSContextRef context,
                                        JSValueRef jsvalue,
                                        DBusMessageIter *iter,
                                        const char *signature)
{
  DBusSignatureIter siter;

  dbus_signature_iter_init(&siter, signature);

  switch (dbus_signature_iter_get_current_type(&siter))
  {
    /* JSValueToBoolean follows the JS rules of what's true and false so we can
     * simply take the value without checking the type of it
     */ 
    case DBUS_TYPE_BOOLEAN:
      {
        dbus_bool_t value = JSValueToBoolean(context, jsvalue);
        if (!dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &value))
        {
          g_warning("Could not append a boolean to message iter");
          return FALSE;
        }
        break;
      }
    /* Basic types */
    case DBUS_TYPE_INT16:
    case DBUS_TYPE_INT32:
    case DBUS_TYPE_INT64:
    case DBUS_TYPE_UINT16:
    case DBUS_TYPE_UINT32:
    case DBUS_TYPE_UINT64:
    case DBUS_TYPE_BYTE:
    case DBUS_TYPE_STRING:
    case DBUS_TYPE_OBJECT_PATH:
    case DBUS_TYPE_SIGNATURE:
      {
        int type = dbus_signature_iter_get_current_type(&siter);
        if (!jsvalue_append_basic(context, jsvalue, type, iter))
        {
          g_warning("Could not append a '%c' to message iter", type);
          return FALSE;
        }
        break;
      }
    case DBUS_TYPE_DOUBLE:
      {
        /* Conversions between dbus_uint64_t and double seem to loose precision,
         * that's why doubles are special-cased
         */
        double value = JSValueToNumber(context, jsvalue, NULL);
        if (!dbus_message_iter_append_basic(iter, DBUS_TYPE_DOUBLE, &value))
        {
          g_warning("Could not append a double to message iter");
          return FALSE;
        }
        break;
      }
    case DBUS_TYPE_ARRAY:
      {
        /* Dicts are implemented as arrays of entries in D-Bus */
        if (dbus_signature_iter_get_element_type(&siter) == DBUS_TYPE_DICT_ENTRY)
        {
          int i, props;
          JSPropertyNameArrayRef propnames;
          char *dict_signature;
          DBusMessageIter subiter;
          DBusSignatureIter dictsiter;
          DBusSignatureIter dictsubsiter;

          dbus_signature_iter_recurse(&siter, &dictsiter);
          dict_signature = dbus_signature_iter_get_signature(&dictsiter);

          if (!dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
                                                dict_signature, &subiter))
          {
            dbus_free(dict_signature);
            g_warning("Memory exhausted!");
            return FALSE;
          }
          dbus_free(dict_signature);
          propnames = JSObjectCopyPropertyNames(context, (JSObjectRef)jsvalue);
          props = JSPropertyNameArrayGetCount(propnames);

          /* Move the signature iter to the value part of the signature
           * We only support string->value dicts currently, though we coud do
           * numbers too...
           */
          dbus_signature_iter_recurse(&dictsiter, &dictsubsiter); /* Now at key */
          dbus_signature_iter_next(&dictsubsiter); /* Now at value */

          for (i = 0; i < props; i++)
          {
            JSStringRef jsstr;
            DBusMessageIter dictiter;
            char *cstr;
            if (!dbus_message_iter_open_container(&subiter, DBUS_TYPE_DICT_ENTRY,
                                                  NULL, &dictiter))
            {
              g_warning("Memory exhausted!");
              JSPropertyNameArrayRelease(propnames);
              return FALSE;
            }
            jsstr = JSPropertyNameArrayGetNameAtIndex(propnames, i);
            cstr = string_from_jsstring(context, jsstr);
            dbus_message_iter_append_basic(&dictiter, DBUS_TYPE_STRING, &cstr);
            g_free(cstr);
            cstr = dbus_signature_iter_get_signature(&dictsubsiter);
            jsvalue_append_to_message_iter(context,
              JSObjectGetProperty(context, (JSObjectRef)jsvalue, jsstr, NULL),
              &dictiter, cstr);
            dbus_free(cstr);
            dbus_message_iter_close_container(&subiter, &dictiter);
          }
          JSPropertyNameArrayRelease(propnames);
          dbus_message_iter_close_container(iter, &subiter);
          break;
        } else {
          int i, props;
          JSPropertyNameArrayRef propnames;
          JSValueRef *jsvalues;
          DBusMessageIter subiter;
          DBusSignatureIter arraysiter;
          char *array_signature = NULL;
          if (!jsvalue_instanceof(context, jsvalue, "Array"))
          {
            g_warning("Expected JavaScript Array type, got %i",
                      JSValueGetType(context, jsvalue));
            return FALSE;
          }
          
          dbus_signature_iter_recurse(&siter, &arraysiter);
          array_signature = dbus_signature_iter_get_signature(&arraysiter);
          
          propnames = JSObjectCopyPropertyNames(context, (JSObjectRef)jsvalue);
          if (!dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
                                                array_signature, &subiter))
          {
            g_warning("Memory exhausted!");
            JSPropertyNameArrayRelease(propnames);
            g_free(array_signature);
            return FALSE;
          }

          props = JSPropertyNameArrayGetCount(propnames);

          for (i = 0; i < props; i++)
          {
            JSValueRef value = JSObjectGetPropertyAtIndex(context,
                                                          (JSObjectRef)jsvalue,
                                                          i,
                                                          NULL);
            jsvalue_append_to_message_iter(context, value,
                                           &subiter, array_signature);
          }
          dbus_message_iter_close_container(iter, &subiter);
          g_free(array_signature);
          JSPropertyNameArrayRelease(propnames);
          break;
        }
      }
    case DBUS_TYPE_VARIANT:
      {
        DBusMessageIter subiter;
        DBusSignatureIter vsiter;
        char *vsignature;
        JSValueRef value = NULL;

        if (jsvalue_typeof(context, jsvalue, "DBusVariant"))
        {
          variant_data_t *data = (variant_data_t *)JSObjectGetPrivate((JSObjectRef)jsvalue);
          value = data->value;
        } else {
          value = jsvalue;
        }
        
        dbus_signature_iter_recurse(&siter, &vsiter);
        vsignature = jsvalue_to_signature(context, value);
        
        if (!dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
                                              vsignature, &subiter))
        {
          g_warning("Memory exhausted!");
          g_free(vsignature);
          return FALSE;
        }

        if (!jsvalue_append_to_message_iter(context, value,
                                            &subiter, vsignature))
        {
          g_warning("Failed to append variant contents with signature %s",
                    vsignature);
          g_free(vsignature);
          return FALSE;
        }
        
        g_free(vsignature);
        dbus_message_iter_close_container(iter, &subiter);
        break;
      }
    case DBUS_TYPE_STRUCT:
      {
        int i, props;
        JSPropertyNameArrayRef propnames;
        DBusMessageIter subiter;
        DBusSignatureIter stsiter;
        char *stsignature;
        JSValueRef value = NULL;

        if (jsvalue_typeof(context, jsvalue, "DBusStruct"))
        {
          value = (JSValueRef)JSObjectGetPrivate((JSObjectRef)jsvalue);
        } else {
          value = jsvalue;
        }

        propnames = JSObjectCopyPropertyNames(context, (JSObjectRef)value);
        props = JSPropertyNameArrayGetCount(propnames);

        if (props == 0)
        {
          g_warning("Empty struct not allowed");
          return FALSE;
        }
        
        if (!dbus_message_iter_open_container(iter, DBUS_TYPE_STRUCT,
                                              NULL, &subiter))
        {
          g_warning("Memory exhausted!");
          return FALSE;
        }

        dbus_signature_iter_recurse(&siter, &stsiter);

        for (i = 0; i < props; i++)
        {
          char *sig = dbus_signature_iter_get_signature(&stsiter);
          JSValueRef child_value = JSObjectGetProperty(context,
            (JSObjectRef)value,
            JSPropertyNameArrayGetNameAtIndex(propnames, i),
            NULL);

          if (!jsvalue_append_to_message_iter(context, child_value,
                                              &subiter, sig))
          {
            g_warning("Failed to append struct contents with signature %s",
                      sig);
            dbus_free(sig);
            return FALSE;
          }
          dbus_free(sig);

          if (!dbus_signature_iter_next(&stsiter))
          {
            break;
          }
        }
        JSPropertyNameArrayRelease(propnames);
        dbus_message_iter_close_container(iter, &subiter);
        break;
      }
    default:
      g_warning("Tried to append invalid or unsupported argument '%s' "
                "(base type '%c') to a message", signature,
                dbus_signature_iter_get_current_type(&siter));
      break;
  }

  return TRUE;
}

static gboolean
jsvalue_append_basic(JSContextRef context,
                     JSValueRef jsvalue,
                     int type,
                     DBusMessageIter *iter)
{
  dbus_uint64_t v = 0;
  dbus_uint64_t *value = NULL;
  char *strvalue = NULL;
  switch (JSValueGetType(context, jsvalue))
  {
    case kJSTypeNumber:
      {
        v = (dbus_uint64_t)JSValueToNumber(context, jsvalue, NULL);
        value = &v;
        break;
      }
    case kJSTypeString:
      {
        strvalue = string_from_jsvalue(context, jsvalue);
        break;
      }
    case kJSTypeUndefined:
    case kJSTypeNull:
      {
        g_warning("Tried to pass undefined or null as basic type");
        break;
      }
    case kJSTypeObject:
      {
        int i;
        for (i = 0; i < JSCOREBUS_N_NUMBER_CLASSES; i++)
        {
          if (jscorebus_number_class_types[i] == type
           && jsvalue_typeof(context, jsvalue, jscorebus_number_class_names[i]))
          {
            value = (dbus_uint64_t *)JSObjectGetPrivate((JSObjectRef)jsvalue);
            break;
          }
        }

        if (value != NULL)
        {
          break;
        }

        if (jsvalue_typeof(context, jsvalue, "DBusObjectPath"))
        {
          strvalue = string_from_jsvalue(context,
                                         JSObjectGetPrivate((JSObjectRef)jsvalue));
          break;
        }

        if (jsvalue_typeof(context, jsvalue, "DBusSignature"))
        {
          strvalue = string_from_jsvalue(context,
                                         JSObjectGetPrivate((JSObjectRef)jsvalue));
          break;
        }

        /* Intentionally falls through to default */
      }
    default:
      g_warning("JSValue wasn't a '%c' (it was %i), or it is not supported",
                type, JSValueGetType(context, jsvalue));
      break;
  }
  
  if (value != NULL && dbus_message_iter_append_basic(iter, type, value))
  {
    return TRUE;
  } else if (strvalue != NULL
          && dbus_message_iter_append_basic(iter, type, &strvalue)) {
    g_free(strvalue);
    return TRUE;
  }
  
  g_free(strvalue);
  return FALSE;
}

#define NUMERIC_VALUE(NAME) \
  DBUS_TYPE_## NAME: \
    { \
      dbus_uint64_t value = 0; \
      dbus_message_iter_get_basic(iter, &value); \
      jsvalue = JSValueMakeNumber(context, (double)value); \
      break; \
    }

JSValueRef jsvalue_from_message_iter(JSContextRef context,
                                     DBusMessageIter *iter)
{
  JSValueRef jsvalue;
  
  switch (dbus_message_iter_get_arg_type (iter))
  {
    case DBUS_TYPE_BOOLEAN:
      {
        gboolean value;
        dbus_message_iter_get_basic(iter, &value);
        jsvalue = JSValueMakeBoolean(context, value);
        break;
      }
    case NUMERIC_VALUE(BYTE)
    case NUMERIC_VALUE(INT16)
    case NUMERIC_VALUE(UINT16)
    case NUMERIC_VALUE(INT32)
    case NUMERIC_VALUE(UINT32)
    case NUMERIC_VALUE(INT64)
    case NUMERIC_VALUE(UINT64)
    case DBUS_TYPE_DOUBLE:
    {
      double value = 0;
      dbus_message_iter_get_basic(iter, &value);
      jsvalue = JSValueMakeNumber(context, value);
      break;
    }
    case DBUS_TYPE_OBJECT_PATH:
    case DBUS_TYPE_SIGNATURE:
    case DBUS_TYPE_STRING:
      {
        const char *value;
        JSStringRef jsstr;
        dbus_message_iter_get_basic(iter, &value);
        jsstr = JSStringCreateWithUTF8CString(value);
        jsvalue = JSValueMakeString(context, jsstr);
        JSStringRelease(jsstr);
        break;
      }
    case DBUS_TYPE_ARRAY:
    case DBUS_TYPE_STRUCT:
      {
        JSStringRef arrayProperty;
        JSObjectRef arrayConstructor;
        DBusMessageIter child_iter;
        int i;
        
        arrayProperty = JSStringCreateWithUTF8CString("Array");
        arrayConstructor = JSValueToObject(context,
                            JSObjectGetProperty(context,
                              JSContextGetGlobalObject(context),
                              arrayProperty, NULL),
                            NULL);
        JSStringRelease(arrayProperty);

        jsvalue = JSObjectCallAsConstructor(context, arrayConstructor,
                                            0, NULL, NULL);

        dbus_message_iter_recurse(iter, &child_iter);
        
        i = 0;
        do
        {
          if (dbus_message_iter_get_arg_type(&child_iter) == DBUS_TYPE_DICT_ENTRY)
          {
            JSValueRef key;
            JSStringRef key_str;
            JSValueRef value;
            DBusMessageIter dictiter;
            
            dbus_message_iter_recurse(&child_iter, &dictiter);
            key = jsvalue_from_message_iter(context, &dictiter);
            key_str = JSValueToStringCopy(context, key, NULL);
            dbus_message_iter_next(&dictiter);
            value = jsvalue_from_message_iter(context, &dictiter);

            JSObjectSetProperty(context, (JSObjectRef)jsvalue,
                                key_str, value, 0, NULL);
            JSStringRelease(key_str);
          } else {
            JSObjectSetPropertyAtIndex(context, (JSObjectRef)jsvalue, i++, 
              jsvalue_from_message_iter(context, &child_iter), NULL);
          }
        } while (dbus_message_iter_next(&child_iter));
        
        break;
      }
    case DBUS_TYPE_VARIANT:
      {
        DBusMessageIter child_iter;
        dbus_message_iter_recurse(iter, &child_iter);
        jsvalue = jsvalue_from_message_iter(context, &child_iter);
        break;
      }
    case DBUS_TYPE_INVALID:
        /* Convert invalid to undefined */
        jsvalue = JSValueMakeUndefined(context);
        break;
    case DBUS_TYPE_DICT_ENTRY:
        /* Dict entries should always be handled in the array branch */
        g_assert_not_reached();
    default:
      g_warning("Could not convert value from type %c (%i)",
                dbus_message_iter_get_arg_type (iter),
                dbus_message_iter_get_arg_type (iter));
      jsvalue = JSValueMakeUndefined(context);
      break;
  }

  return jsvalue;
}


char *string_from_jsstring(JSContextRef context, JSStringRef jsstr)
{
  size_t len;
  char *cstr;

  len = JSStringGetMaximumUTF8CStringSize(jsstr);
  cstr = g_new(char, len);
  JSStringGetUTF8CString(jsstr, cstr, len);

  return cstr;
}

char *string_from_jsvalue(JSContextRef context, JSValueRef jsvalue)
{
  JSStringRef jsstr;
  char *cstr;

  if (!JSValueIsString(context, jsvalue))
  {
    return NULL;
  }

  jsstr = JSValueToStringCopy(context, jsvalue, NULL);
  cstr = string_from_jsstring(context, jsstr);
  JSStringRelease(jsstr);
  
  return cstr;
}

JSObjectRef function_from_jsvalue(JSContextRef context,
                                  JSValueRef value,
                                  JSValueRef* exception)
{
  JSObjectRef function;

  if (JSValueIsNull(context, value))
  {
    return NULL;
  }

  if (!JSValueIsObject(context, value) )
  {
    /* TODO: set exception */
    g_warning("%s: Value wasn't an object", G_STRFUNC);
    return NULL;
  }

  function = JSValueToObject(context, value, exception);
  if (!JSObjectIsFunction(context, function))
  {
    /* TODO: set exception */
    g_warning("%s: Value wasn't a function", G_STRFUNC);
    return NULL;
  }

  return function;
}

void call_function_with_message_args(JSContextRef context,
                                     JSObjectRef thisObject,
                                     JSObjectRef function,
                                     DBusMessage *message)
{
  size_t argumentCount;
  JSValueRef *args;
  int arg_type;
  DBusMessageIter iter;

  /**
   * Iterate over the message arguments and append them to args
   */
  dbus_message_iter_init (message, &iter);
  argumentCount = 0;
  args = NULL;

  /* Error messages should have the error name as the first param */
  if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_ERROR)
  {
    const char *error_name = dbus_message_get_error_name(message);
    
    if (error_name != NULL)
    {
      JSValueRef *tmp;
      JSStringRef jsstr;
      
      jsstr = JSStringCreateWithUTF8CString(error_name);
      argumentCount++;
      tmp = g_renew(JSValueRef, args, argumentCount);
      args = tmp;
      args[argumentCount-1] = JSValueMakeString(context, jsstr);;
      JSStringRelease(jsstr);
     }
  }

  while ((arg_type = dbus_message_iter_get_arg_type (&iter)) != DBUS_TYPE_INVALID)
  {
    JSValueRef *tmp;
    
    argumentCount++;
    tmp = g_renew(JSValueRef, args, argumentCount);
    args = tmp;
    args[argumentCount-1] = (JSValueRef)jsvalue_from_message_iter(context, &iter);
    if (args[argumentCount-1] == NULL) {
      g_warning("Couldn't get argument from argument type %c", arg_type);
      args[argumentCount-1] = JSValueMakeUndefined(context);
    }
    dbus_message_iter_next (&iter);
  }

  JSObjectCallAsFunction(context, function, thisObject,
                         argumentCount, args, NULL);
  g_free(args);
}

