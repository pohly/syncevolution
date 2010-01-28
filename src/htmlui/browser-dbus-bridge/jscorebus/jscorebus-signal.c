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

/* Private data for signals */
typedef struct _SignalPrivate
{
  char *interface;
  char *signal_name;
  char *sender;
  char *object_path;

  DBusConnection *connection;
  char *match_rule;

  JSGlobalContextRef context;
  JSObjectRef this;
  JSObjectRef onemit;
  gboolean enabled;
} SignalPrivate;

/* Utility functions */
static
JSClassRef get_class ();

static
void add_match_and_handler(SignalPrivate *priv);
static
void remove_match_and_handler(SignalPrivate *priv);

/* JSCore methods */
static
void signal_finalize(JSObjectRef object);

static
bool signal_set_property(JSContextRef context,
                         JSObjectRef object,
                         JSStringRef propertyName,
                         JSValueRef value,
                         JSValueRef* exception);

/* We keep a hash table to track the signals callbacks */
static GHashTable *signal_hash;

/* The Signal Class */
static const
JSClassDefinition signal_jsclass_def =
{
  0,
  kJSClassAttributeNone,
  "DBusSignal",
  NULL,

  NULL,
  NULL,
  
  NULL, /* Initialize */
  signal_finalize,
  
  NULL,
  NULL,
  signal_set_property, /* SetProperty */
  NULL,
  NULL,
  
  NULL, /* CallAsFunction */
  NULL, /* Constructor */
  NULL,
  NULL
};

/*** JSCore methods */

static
void signal_finalize(JSObjectRef object)
{
  SignalPrivate *priv = (SignalPrivate *)JSObjectGetPrivate(object);

  if (priv != NULL)
  {
    remove_match_and_handler(priv);
    
    g_free(priv->object_path);
    g_free(priv->signal_name);
    g_free(priv->interface);
    g_free(priv->sender);
    g_free(priv->match_rule);
    dbus_connection_unref(priv->connection);

    priv->this = NULL;
    priv->onemit = NULL;
    priv->context = NULL;
    
    g_free(priv);
    priv = NULL;
  }
}

static
bool signal_set_property(JSContextRef context,
                         JSObjectRef object,
                         JSStringRef propertyName,
                         JSValueRef value,
                         JSValueRef* exception)
{
  SignalPrivate *priv;

  priv = (SignalPrivate *)JSObjectGetPrivate(object);
  g_assert(priv != NULL);

  if (JSStringIsEqualToUTF8CString(propertyName, "enabled"))
  {
    /* We don't enable unless we have a callback */
    if (priv->onemit == NULL)
    {
      /* TODO: Throw exception */
      return TRUE;
    }
    if (JSValueIsBoolean(context, value))
    {
      priv->enabled = JSValueToBoolean(context, value);
      if (priv->enabled)
      {
        add_match_and_handler(priv);
      }
      return TRUE;
    }
    
    /* TODO: set exception */
    g_warning("Tried to set a non-boolean to 'enabled'");
    return TRUE;
  } else if (JSStringIsEqualToUTF8CString(propertyName, "onemit")) {
    priv->onemit = function_from_jsvalue(context, value, exception);

    /* If the callback function goes away, we need to disable the signal,
      * remove match and remove our entry from the handlers
      */
    if (priv->onemit == NULL)
    {
      remove_match_and_handler(priv);
    }
    return TRUE;
  }

  return FALSE;
}

/*** Utility functions */

static
JSClassRef get_class ()
{
  JSClassRef jsclass = (JSClassRef)jsclass_lookup(&signal_jsclass_def);
  
  if (G_LIKELY(jsclass != NULL))
  {
    return jsclass;
  }
  
  jsclassdef_insert("DBusSignal", &signal_jsclass_def);
  jsclass = (JSClassRef)jsclass_lookup(&signal_jsclass_def);

  g_assert(jsclass != NULL);

  return jsclass;
}

static
void add_match_and_handler(SignalPrivate *priv)
{
  char *signal_str;
  GPtrArray *handlers;

  dbus_bus_add_match(priv->connection, priv->match_rule, NULL);

  signal_str = g_strdup_printf("%s.%s", priv->interface, priv->signal_name);
  handlers = g_hash_table_lookup(signal_hash, signal_str);

  if (handlers == NULL)
  {
    handlers = g_ptr_array_new();
    g_hash_table_insert(signal_hash, signal_str, handlers);
  } else {
    g_free(signal_str);
  }

  g_ptr_array_add(handlers, priv);
}

static
void remove_match_and_handler(SignalPrivate *priv)
{
  char *signal_str;
  GPtrArray *handlers;

  signal_str = g_strdup_printf("%s.%s", priv->interface, priv->signal_name);

  handlers = g_hash_table_lookup(signal_hash, signal_str);

  if (handlers != NULL)
  {
    g_ptr_array_remove(handlers, priv);
    if (handlers->len == 0)
    {
      g_hash_table_remove(signal_hash, signal_str);
      g_ptr_array_free(handlers, TRUE);
    }
  }

  g_free(signal_str);

  dbus_bus_remove_match(priv->connection, priv->match_rule, NULL);

}

static
void call_onemit(SignalPrivate *priv,
                 DBusMessage *message)
{

  if (priv->enabled == FALSE
   || priv->onemit == NULL)
  {
    return;
  }

  /* If the signal was created with a sender, we need to check for it
   * and not deliver if it does not match
   */
  if (priv->sender != NULL)
  {
    if (!dbus_message_has_sender(message, priv->sender))
    {
      return;
    }
  }
  /* Ditto for object paths */
  if (priv->object_path != NULL)
  {
    if (!dbus_message_has_path(message, priv->object_path))
    {
      return;
    }
  }

  call_function_with_message_args(priv->context, priv->this, priv->onemit, message);
}

static
DBusHandlerResult signal_filter(DBusConnection *connection,
                                DBusMessage *message,
                                void *user_data)
{
  char *signal_str;
  GPtrArray *handlers;

  if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
  {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  signal_str = g_strdup_printf("%s.%s",
                               dbus_message_get_interface(message),
                               dbus_message_get_member(message));

  handlers = g_hash_table_lookup(signal_hash, signal_str);
  g_free(signal_str);
  
  if (handlers == NULL || handlers->len == 0)
  {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  g_ptr_array_foreach(handlers, (GFunc)call_onemit, message);

  return DBUS_HANDLER_RESULT_HANDLED;
}

/*** Public API */

/* NOTE: Takes ownership of the arguments! */
JSObjectRef jscorebus_create_signal (JSGlobalContextRef context,
                                     DBusConnection *connection,
                                     char *interface,
                                     char *signal_name,
                                     char *sender,
                                     char *object_path,
                                     JSObjectRef thisObject,
                                     JSValueRef* exception)
{
  int i;
  char **matchv;
  char *signal_str;
  GPtrArray *handlers;
  SignalPrivate *priv = NULL;
  static gboolean filter_added = FALSE;

  g_return_val_if_fail(interface != NULL
                       && signal_name != NULL,
                       NULL);

  priv = g_new0(SignalPrivate, 1);

  priv->object_path = object_path;
  priv->interface = interface;
  priv->signal_name = signal_name;
  priv->sender = sender;

  priv->enabled = false;

  priv->connection = dbus_connection_ref(connection);
  
  priv->context = context;
  priv->this = thisObject;

  /* If we don't already have a filter function for the signals, add one */
  if (G_UNLIKELY(!filter_added))
  {
    signal_hash = g_hash_table_new(g_str_hash, g_str_equal);
    dbus_connection_add_filter(connection, signal_filter, NULL, NULL);
    filter_added = TRUE;
  }

  /* Add the match rule for the signal */
  matchv = g_new0(char*, 4);
  i = 0;
  matchv[i++] = g_strdup_printf("type=signal,interface=%s,member=%s",
                                priv->interface, priv->signal_name);
  if (priv->sender != NULL)
  {
    matchv[i++] = g_strdup_printf("sender=%s", priv->sender);
  }
  if (priv->object_path != NULL)
  {
    matchv[i++] = g_strdup_printf("path=%s", priv->object_path);
  }

  priv->match_rule = g_strjoinv(",", matchv);
  g_strfreev(matchv);
  
  return JSObjectMake(context, get_class(), priv);
}

