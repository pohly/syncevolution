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

#include "jscorebus-marshal.h"
#include "jscorebus-method.h"

/* Private data for methods */
typedef struct _MethodPrivate
{
  char *destination;
  char *object_path;
  char *method_name;
  char *interface;
  char *signature;

  DBusConnection *connection;
  DBusMessage *message;
  DBusPendingCall *pending_reply;

  JSGlobalContextRef context;
  JSObjectRef this;
  JSObjectRef onreply;
  JSObjectRef onerror;
  gboolean async;
} MethodPrivate;

/* Utility functions */
static
JSClassRef get_class ();

static
JSValueRef call_sync(JSContextRef context, MethodPrivate *priv);
static
JSValueRef call_async(JSContextRef context, MethodPrivate *priv);
static
void call_onreply(JSContextRef context,
                  MethodPrivate *priv,
                  DBusMessage *message);
static
void call_onerror(JSContextRef context,
                  MethodPrivate *priv,
                  DBusMessage *message);

/* JSCore methods */
static
void method_finalize(JSObjectRef object);

static
bool method_set_property(JSContextRef context,
                         JSObjectRef object,
                         JSStringRef propertyName,
                         JSValueRef value,
                         JSValueRef* exception);

static
JSValueRef method_call (JSContextRef context,
                        JSObjectRef function,
                        JSObjectRef thisObject,
                        size_t argumentCount,
                        const JSValueRef arguments[],
                        JSValueRef *exception);

/* The Method Class */
static const
JSClassDefinition method_jsclass_def =
{
  0,
  kJSClassAttributeNone,
  "DBusMethod",
  NULL,

  NULL,
  NULL,
  
  NULL, /* Initialize */
  method_finalize,
  
  NULL,
  NULL,
  method_set_property, /* SetProperty */
  NULL,
  NULL,
  
  method_call, /* CallAsFunction */
  NULL, /* Constructor */
  NULL,
  NULL
};

/*** JSCore methods */

static
void method_finalize(JSObjectRef object)
{
  MethodPrivate *priv = (MethodPrivate *)JSObjectGetPrivate(object);

  if (priv != NULL)
  {

    if (priv->pending_reply != NULL)
    {
      dbus_pending_call_cancel(priv->pending_reply);
    }
  
    g_free(priv->destination);
    g_free(priv->object_path);
    g_free(priv->method_name);
    g_free(priv->interface);
    g_free(priv->signature);
    
    dbus_connection_unref(priv->connection);

    priv->this = NULL;
    priv->onreply = NULL;
    priv->onerror = NULL;
    priv->context = NULL;
    
    g_free(priv);
    priv = NULL;
  }
}

static
bool method_set_property(JSContextRef context,
                         JSObjectRef object,
                         JSStringRef propertyName,
                         JSValueRef value,
                         JSValueRef* exception)
{
  MethodPrivate *priv;

  priv = (MethodPrivate *)JSObjectGetPrivate(object);
  g_assert(priv != NULL);

  if (JSStringIsEqualToUTF8CString(propertyName, "async"))
  {
    if (JSValueIsBoolean(context, value))
    { 
      priv->async = JSValueToBoolean(context, value);
      return TRUE;
    }
    
    /* TODO: set exception */
    g_warning("Tried to set a non-boolean to 'async'");
    return TRUE;
  } else if (JSStringIsEqualToUTF8CString(propertyName, "onreply")) {
    priv->onreply = function_from_jsvalue(context, value, exception);
    return TRUE;
  } else if (JSStringIsEqualToUTF8CString(propertyName, "onerror")) {
    priv->onerror = function_from_jsvalue(context, value, exception);
    return TRUE;
  }

  return FALSE;
}

static
JSValueRef method_call (JSContextRef context,
                        JSObjectRef function,
                        JSObjectRef thisObject,
                        size_t argumentCount,
                        const JSValueRef arguments[],
                        JSValueRef *exception)
{
  int i;
  MethodPrivate *priv;
  DBusMessageIter iter;
  JSValueRef ret;
 	
  priv = (MethodPrivate *)JSObjectGetPrivate(function);
  g_assert(priv != NULL);

  priv->message = dbus_message_new_method_call(priv->destination,
                                               priv->object_path,
                                               priv->interface,
                                               priv->method_name);
  
  if (argumentCount > 0)
  {
    /* Push arguments to the message */
    dbus_message_iter_init_append (priv->message, &iter);
    if (!jsvalue_array_append_to_message_iter(context,
                                              arguments, argumentCount,
                                              &iter, priv->signature))
    {
      dbus_message_unref(priv->message);
      priv->message = NULL;
    }
  }
  
  if (priv->async)
  {
    ret = call_async(context, priv);
  } else {
    ret = call_sync(context, priv);
  }

  if (priv->message != NULL)
  {
    dbus_message_unref(priv->message);
  }
  priv->message = NULL;

  return ret;
}

/*** Utility functions */

static
JSClassRef get_class ()
{
  JSClassRef jsclass = (JSClassRef)jsclass_lookup(&method_jsclass_def);
  
  if (G_LIKELY(jsclass != NULL))
  {
    return jsclass;
  }
  
  jsclassdef_insert("DBusMethod", &method_jsclass_def);
  jsclass = (JSClassRef)jsclass_lookup(&method_jsclass_def);

  g_assert(jsclass != NULL);

  return jsclass;
}

static
JSValueRef call_sync(JSContextRef context, MethodPrivate *priv)
{
  /**
   * Set up reply callback from the method.onReply property if available and
   * send the message
   */

  if (priv->message == NULL)
  {
    call_onerror(context, priv, NULL);
    return JSValueMakeUndefined(context);
  }

  if (priv->onreply != NULL)
  {
    DBusMessage *reply_message;
    
    reply_message = dbus_connection_send_with_reply_and_block(
                      priv->connection,
                      priv->message, -1, NULL);
    if (reply_message != NULL)
    {
      if (dbus_message_get_type(reply_message) == DBUS_MESSAGE_TYPE_ERROR)
      {
        call_onerror(context, priv, reply_message);
      } else if (dbus_message_get_type(reply_message) == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
        call_onreply(context, priv, reply_message);
      } else {
        g_warning("Unknown reply!");
      }
      dbus_message_unref(reply_message);
    } else {
      // TODO: set exception
      g_warning("Failed to send message to %s", priv->destination);
      call_onerror(context, priv, NULL);
    }
  } else {
    if (!dbus_connection_send(priv->connection, priv->message, NULL))
    {
      // TODO: set exception
      g_warning("Failed to send message to %s", priv->destination);
      call_onerror(context, priv, NULL);
    }
  }  

  return JSValueMakeUndefined(context);
}

static
void pending_call_notify(DBusPendingCall *pending,
                         void *user_data)
{
  DBusMessage *reply_message;
  MethodPrivate *priv;
  
  g_assert(user_data != NULL);
  
  priv = (MethodPrivate *)user_data;

  if (pending == NULL)
  {
    /* Disconnected!
     * TODO: How do we handle these?
     */
    g_warning("Disconnected from the bus!");
    priv->pending_reply = NULL;
    return;
  }
 
  priv->pending_reply = NULL;
  reply_message = dbus_pending_call_steal_reply(pending);

  if (dbus_message_get_type(reply_message) == DBUS_MESSAGE_TYPE_ERROR)
  {
    call_onerror(priv->context, priv, reply_message);
  } else if (dbus_message_get_type(reply_message) == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
    call_onreply(priv->context, priv, reply_message);
  } else {
    g_warning("Unknown reply!");
  }
  if (reply_message != NULL)
  {
    dbus_message_unref(reply_message);
  }
}

static
JSValueRef call_async(JSContextRef context, MethodPrivate *priv)
{
 	dbus_uint32_t serial = 0;

  if (priv->message == NULL)
  {
    call_onerror(context, priv, NULL);
  }

  if (priv->onreply != NULL)
  {
    if (dbus_connection_send_with_reply(
          priv->connection,
          priv->message, &priv->pending_reply, -1))
    {
      dbus_pending_call_set_notify(priv->pending_reply, pending_call_notify,
                                   priv, NULL);
    } else {
      // TODO: set exception
      call_onerror(context, priv, NULL);
    }
  } else {
    dbus_message_set_no_reply(priv->message, TRUE);

    if (!dbus_connection_send(priv->connection, priv->message, &serial))
    {
      // TODO: set exception
      call_onerror(context, priv, NULL);
    }
  }
  
  return JSValueMakeUndefined(context);
}

static
void call_onreply(JSContextRef context,
                  MethodPrivate *priv,
                  DBusMessage *message)
{
  if (priv->onreply == NULL)
  {
    return;
  }

  call_function_with_message_args(context, priv->this, priv->onreply, message);
}

static
void call_onerror(JSContextRef context,
                  MethodPrivate *priv,
                  DBusMessage *message)
{
  if (priv->onerror == NULL)
  {
    return;
  }

  if (message == NULL)
  {
    /* We couldn't send the message */
    JSStringRef name = JSStringCreateWithUTF8CString("MessageError");
    JSStringRef str = JSStringCreateWithUTF8CString("Could not send message");
    JSValueRef *args = g_new(JSValueRef, 2);
    
    args[0] = JSValueMakeString(context, name);
    args[1] = JSValueMakeString(context, str);
    
    JSObjectCallAsFunction(context, priv->onerror, priv->this,
                           2, args, NULL);
    JSStringRelease(name);
    JSStringRelease(str);
    g_free(args);
    return;
  }

  call_function_with_message_args(context, priv->this, priv->onerror, message);
}

/*** Public API */

/* NOTE: Takes ownership of the arguments! */
JSObjectRef jscorebus_create_method (JSGlobalContextRef context,
                                     DBusConnection *connection,
                                     char *destination,
                                     char *object_path,
                                     char *method_name,
                                     char *interface,
                                     char *signature,
                                     JSObjectRef thisObject,
                                     JSValueRef* exception)
{
  MethodPrivate *priv = NULL;

  g_return_val_if_fail(destination != NULL
                       && object_path != NULL
                       && method_name != NULL,
                       NULL);

  priv = g_new0(MethodPrivate, 1);

  /* TODO: Maybe these should be JSStrings after all, to avoid copies? */
  priv->destination = destination;
  priv->object_path = object_path;
  priv->method_name = method_name;

  priv->interface = interface;
  priv->signature = signature;

  priv->async = true;

  priv->connection = dbus_connection_ref(connection);
  
  priv->context = context;

  priv->this = thisObject;

  return JSObjectMake(context, get_class(), priv);
}
