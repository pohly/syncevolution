/**
 * Unit, a D-Bus tester for argument marshalling
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

/*
 * The Unit interface has methods that take a certain argument types and
 * send a reply and a subsequent signal with the received arguments.
 *
 * To build, execute this:
 *
 *   gcc -o unit unit.c $(pkg-config --cflags --libs dbus-glib-1)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>

#define OBJECT_PATH "/org/movial/Unit"
#define SERVICE_NAME "org.movial.Unit"

GMainLoop *loop;

static gboolean u_transfer_array(DBusMessageIter *to_iter, DBusMessageIter *from_iter);
static gboolean u_transfer_variant(DBusMessageIter *to_iter, DBusMessageIter *from_iter);
static gboolean u_transfer_dict(DBusMessageIter *to_iter, DBusMessageIter *from_iter);
static gboolean u_transfer_struct(DBusMessageIter *to_iter, DBusMessageIter *from_iter);

static void
u_unregister(DBusConnection *connection, void *user_data)
{
  g_debug("Object path unregistered");
  g_main_loop_quit(loop);
}

static void
u_transfer_arg(DBusMessageIter *to, DBusMessageIter *from)
{
  dbus_uint64_t value;
  int type = dbus_message_iter_get_arg_type(from);
  
  if (!dbus_type_is_basic(type))
  {
    switch (type)
    {
      case DBUS_TYPE_ARRAY:
        u_transfer_array(to, from);
        break;
      case DBUS_TYPE_VARIANT:
        u_transfer_variant(to, from);
        break;
      case DBUS_TYPE_DICT_ENTRY:
        u_transfer_dict(to, from);
        break;
      case DBUS_TYPE_STRUCT:
        u_transfer_struct(to, from);
        break;
      default:
        g_warning("Non-basic type '%c' in variants not yet handled", type);
        break;
    }
    return;
  }

  dbus_message_iter_get_basic(from, &value);
  if (type == DBUS_TYPE_STRING)
  {
    g_debug("Transferring string arg %s", value);
  }
  if (type == DBUS_TYPE_INT32)
  {
    g_debug("Transferring int arg %i", value);
  }
  if (type == DBUS_TYPE_DOUBLE)
  {
    g_debug("Transferring double arg %f", (double)value);
  }
  dbus_message_iter_append_basic(to, type, &value);
}

static gboolean
u_transfer_variant(DBusMessageIter *to_iter, DBusMessageIter *from_iter)
{
  DBusMessageIter to;
  DBusMessageIter from;
  const char *sig = NULL;
  
  dbus_message_iter_recurse(from_iter, &from);
  sig = dbus_message_iter_get_signature(&from);

  if (sig == NULL)
    return FALSE;

  dbus_message_iter_open_container(to_iter, DBUS_TYPE_VARIANT, sig, &to);

  do {
    u_transfer_arg(&to, &from);
  } while (dbus_message_iter_next(&from));
  
  dbus_message_iter_close_container(to_iter, &to);
  
  return TRUE;
}

static gboolean
u_transfer_array(DBusMessageIter *to_iter, DBusMessageIter *from_iter)
{
  DBusMessageIter to;
  DBusMessageIter from;
  int elem_type;
  char *sig = NULL;
  
  elem_type = dbus_message_iter_get_element_type(from_iter);
  switch (elem_type)
  {
    case DBUS_TYPE_ARRAY:
      {
        /* Fetch the element type for this too */
        sig = g_strdup(dbus_message_iter_get_signature(from_iter)+1);
        break;
      }
    case DBUS_TYPE_DICT_ENTRY:
      /* Oh, this was actually a dict */
      return u_transfer_dict(to_iter, from_iter);
      break;
    case DBUS_TYPE_STRUCT:
      u_transfer_struct(to_iter, from_iter);
      break;
    default:
      sig = g_strdup_printf("%c", dbus_message_iter_get_element_type(from_iter));
      break;
  }

  if (sig == NULL)
    return FALSE;

  dbus_message_iter_recurse(from_iter, &from);

  dbus_message_iter_open_container(to_iter, DBUS_TYPE_ARRAY, sig, &to);

  do {
    u_transfer_arg(&to, &from);
  } while (dbus_message_iter_next(&from));
  
  dbus_message_iter_close_container(to_iter, &to);

  return TRUE;
}

static gboolean
u_transfer_dict(DBusMessageIter *to_iter, DBusMessageIter *from_iter)
{
  DBusMessageIter to;
  DBusMessageIter from;
  char *sig = NULL;

  dbus_message_iter_recurse(from_iter, &from);
  sig = g_strdup(dbus_message_iter_get_signature(&from));

  if (sig == NULL)
    return FALSE;

  dbus_message_iter_open_container(to_iter, DBUS_TYPE_ARRAY, sig, &to);
  do {
    DBusMessageIter subfrom;
    DBusMessageIter dictiter;
    dbus_message_iter_open_container(&to, DBUS_TYPE_DICT_ENTRY,
                                                    NULL, &dictiter);
    dbus_message_iter_recurse(&from, &subfrom);

    u_transfer_arg(&dictiter, &subfrom);
    if (dbus_message_iter_next(&subfrom)) {
      u_transfer_arg(&dictiter, &subfrom);
    }
    dbus_message_iter_close_container(&to, &dictiter);
  } while (dbus_message_iter_next(&from));
  
  dbus_message_iter_close_container(to_iter, &to);
  
  return TRUE;
}

static gboolean
u_transfer_struct(DBusMessageIter *to_iter, DBusMessageIter *from_iter)
{
  DBusMessageIter to;
  DBusMessageIter from;

  dbus_message_iter_recurse(from_iter, &from);
  dbus_message_iter_open_container(to_iter, DBUS_TYPE_STRUCT, NULL, &to);

  do {
    u_transfer_arg(&to, &from);
  } while (dbus_message_iter_next(&from));
  
  dbus_message_iter_close_container(to_iter, &to);
  
  return TRUE;
}

static DBusHandlerResult
u_message(DBusConnection *connection, DBusMessage *message, void *user_data)
{
#ifdef DEBUG  
  const char *path = dbus_message_get_path(message);
  const char *interface = dbus_message_get_interface(message);
  const char *member = dbus_message_get_member(message);
  const char *signature = dbus_message_get_signature(message);

  g_debug("Message received, path %s, interface %s, member %s, signature %s",
          path == NULL ? "not set" : path,
          interface == NULL ? "not set" : interface,
          member == NULL ? "not set" : member,
          signature == NULL ? "not set" : signature);
#endif
  
  if (!dbus_message_has_path(message, OBJECT_PATH))
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  if (dbus_message_is_method_call(message, SERVICE_NAME, "start"))
  {
    DBusMessage *reply;

    /* TODO: start logging */
    
    reply = dbus_message_new_method_return(message);
    dbus_connection_send(connection, reply, NULL);
    dbus_message_unref(reply);
    
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  if (dbus_message_is_method_call(message, SERVICE_NAME, "end"))
  {
    DBusMessage *reply;

    /* TODO: stop logging */
    
    reply = dbus_message_new_method_return(message);
    dbus_connection_send(connection, reply, NULL);
    dbus_message_unref(reply);
    
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  if (strlen(dbus_message_get_signature(message)) == 0)
  {
    DBusMessage *reply = dbus_message_new_error(message,
                                                SERVICE_NAME ".ArgError",
                                                "Empty signature");
    dbus_connection_send(connection, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
  }

#define BASIC_TYPE(method_name, dtype) \
  if (dbus_message_is_method_call(message, SERVICE_NAME, method_name)) \
  { \
    dbus_uint64_t value = 0; \
    DBusError err; \
    DBusMessage *reply = NULL; \
    DBusMessage *signal = NULL; \
    dbus_error_init(&err); \
    if (!dbus_message_get_args(message, &err, \
                               DBUS_TYPE_ ## dtype, &value, \
                               DBUS_TYPE_INVALID)) \
    { \
      reply = dbus_message_new_error(message, \
                                     SERVICE_NAME ".ArgError", \
                                     err.message); \
      g_debug(err.message); \
      g_assert(reply != NULL); \
    } else { \
      reply = dbus_message_new_method_return(message); \
      g_assert(reply != NULL); \
      dbus_message_append_args(reply, \
                               DBUS_TYPE_ ## dtype, &value, \
                               DBUS_TYPE_INVALID); \
      signal = dbus_message_new_signal(OBJECT_PATH, SERVICE_NAME, method_name); \
      g_debug("Appending %llu to signal", value); \
      dbus_message_append_args(signal, \
                               DBUS_TYPE_ ## dtype, &value, \
                               DBUS_TYPE_INVALID); \
    } \
    dbus_connection_send(connection, reply, NULL); \
    dbus_message_unref(reply); \
    if (signal != NULL) \
    { \
      dbus_connection_send(connection, signal, NULL); \
      dbus_message_unref(signal); \
    } \
    return DBUS_HANDLER_RESULT_HANDLED; \
  }
  
  BASIC_TYPE("Boolean", BOOLEAN);
  BASIC_TYPE("Byte", BYTE);
  BASIC_TYPE("Int16", INT16);
  BASIC_TYPE("Int32", INT32);
  BASIC_TYPE("Int64", INT64);
  BASIC_TYPE("UInt16", UINT16);
  BASIC_TYPE("UInt32", UINT32);
  BASIC_TYPE("UInt64", UINT64);
  BASIC_TYPE("Double", DOUBLE);
  BASIC_TYPE("String", STRING);
  BASIC_TYPE("ObjectPath", OBJECT_PATH);
  BASIC_TYPE("Signature", SIGNATURE);
  
  if (dbus_message_is_method_call(message, SERVICE_NAME, "Array"))
  {
    DBusMessageIter iter;
    DBusMessageIter reply_iter;
    DBusMessage *reply;
    DBusMessage *signal;

    dbus_message_iter_init(message, &iter);

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
    {
      reply = dbus_message_new_error(message,
                                     SERVICE_NAME ".ArgError",
                                     "Signature mismatch");
      dbus_connection_send(connection, reply, NULL);
      dbus_message_unref(reply);
      return DBUS_HANDLER_RESULT_HANDLED;
    }

    reply = dbus_message_new_method_return(message);

    dbus_message_iter_init_append(reply, &reply_iter);
    u_transfer_array(&reply_iter, &iter);

    dbus_message_iter_init(message, &iter);
    signal = dbus_message_new_signal(OBJECT_PATH, SERVICE_NAME, "Array");
    dbus_message_iter_init_append(signal, &reply_iter);
    u_transfer_array(&reply_iter, &iter);

    dbus_connection_send(connection, reply, NULL);
    dbus_message_unref(reply);
    dbus_connection_send(connection, signal, NULL);
    dbus_message_unref(signal);
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  if (dbus_message_is_method_call(message, SERVICE_NAME, "Variant"))
  {
    DBusMessageIter iter;
    DBusMessageIter reply_iter;
    DBusMessage *reply;
    DBusMessage *signal = NULL;

    if (!g_str_equal(dbus_message_get_signature(message), "v"))
    {
      reply = dbus_message_new_error(message,
                                     SERVICE_NAME ".ArgError",
                                     "Signature mismatch");
      dbus_connection_send(connection, reply, NULL);
      dbus_message_unref(reply);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
    dbus_message_iter_init(message, &iter);

    reply = dbus_message_new_method_return(message);
    dbus_message_iter_init_append(reply, &reply_iter);
    u_transfer_variant(&reply_iter, &iter);

    dbus_message_iter_init(message, &iter);
    signal = dbus_message_new_signal(OBJECT_PATH, SERVICE_NAME, "Variant");
    dbus_message_iter_init_append(signal, &reply_iter);
    u_transfer_variant(&reply_iter, &iter);

    dbus_connection_send(connection, reply, NULL);
    dbus_message_unref(reply);
    dbus_connection_send(connection, signal, NULL);
    dbus_message_unref(signal);
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  if (dbus_message_is_method_call(message, SERVICE_NAME, "Dict"))
  {
    DBusMessageIter iter;
    DBusMessageIter reply_iter;
    DBusMessage *reply;
    DBusMessage *signal;

    dbus_message_iter_init(message, &iter);

    reply = dbus_message_new_method_return(message);
    dbus_message_iter_init_append(reply, &reply_iter);

    u_transfer_dict(&reply_iter, &iter);

    dbus_message_iter_init(message, &iter);
    signal = dbus_message_new_signal(OBJECT_PATH, SERVICE_NAME, "Dict");
    dbus_message_iter_init_append(signal, &reply_iter);
    u_transfer_dict(&reply_iter, &iter);

    dbus_connection_send(connection, reply, NULL);
    dbus_message_unref(reply);
    dbus_connection_send(connection, signal, NULL);
    dbus_message_unref(signal);
    
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  if (dbus_message_is_method_call(message, SERVICE_NAME, "Struct"))
  {
    DBusMessageIter iter;
    DBusMessageIter reply_iter;
    DBusMessage *reply;
    DBusMessage *signal;
    const char *signature = dbus_message_get_signature(message);
    
    if (signature[0] != '(')
    	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    dbus_message_iter_init(message, &iter);

    reply = dbus_message_new_method_return(message);
    dbus_message_iter_init_append(reply, &reply_iter);

    u_transfer_struct(&reply_iter, &iter);

    dbus_message_iter_init(message, &iter);
    signal = dbus_message_new_signal(OBJECT_PATH, SERVICE_NAME, "Struct");
    dbus_message_iter_init_append(signal, &reply_iter);
    u_transfer_struct(&reply_iter, &iter);

    dbus_connection_send(connection, reply, NULL);
    dbus_message_unref(reply);
    dbus_connection_send(connection, signal, NULL);
    dbus_message_unref(signal);
    
    return DBUS_HANDLER_RESULT_HANDLED;
  }

#ifdef DEBUG
  g_debug("Didn't handle the message");
#endif

  
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int main(int argc, char **argv)
{
  DBusError err;
  DBusConnection* conn;
  DBusObjectPathVTable vtable;
  int ret;

  dbus_error_init(&err);
  conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
  if (dbus_error_is_set(&err)) { 
    g_warning("Connection Error (%s)\n", err.message); 
    dbus_error_free(&err); 
  }
  g_assert(conn != NULL);

  vtable.unregister_function = u_unregister;
  vtable.message_function = u_message;

  if (!dbus_connection_register_object_path(conn, OBJECT_PATH,
                                            &vtable, NULL))
  {
    g_error("Could not register object path");
  }

  ret = dbus_bus_request_name(conn, SERVICE_NAME, 
                              DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
  if (dbus_error_is_set(&err)) { 
    g_warning("Requesting service name failed (%s)\n", err.message); 
    dbus_error_free(&err); 
  }
  g_assert(DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER == ret);

  dbus_bus_add_match(conn, "type='method_call'", &err);
  if (dbus_error_is_set(&err)) { 
    g_warning("Add match failed (%s)\n", err.message); 
    dbus_error_free(&err);
    exit(1);
  }

  loop = g_main_loop_new(NULL, FALSE);
  dbus_connection_setup_with_g_main(conn, NULL);
  
	g_print("Unit ready to accept method calls\n");
  g_main_loop_run(loop);
  
  return 0;
}
