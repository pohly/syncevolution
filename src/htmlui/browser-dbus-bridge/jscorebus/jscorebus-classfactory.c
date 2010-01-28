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

#include <JavaScriptCore/JavaScript.h>
#include <glib.h>

#include "jscorebus-classfactory.h"

static
GHashTable *definitions = NULL;
static
GHashTable *classes = NULL;

void
jsclassdef_insert(const char *class_name, const JSClassDefinition *definition)
{
  if (G_UNLIKELY(definitions == NULL))
  {
    definitions = g_hash_table_new(g_str_hash, g_str_equal);
  }

  g_hash_table_insert(definitions, (gpointer)class_name, (gpointer)definition);
}

const JSClassDefinition *
jsclassdef_lookup(const char *class_name)
{
  if (class_name == NULL)
  {
    return NULL;
  }
  
  if (G_UNLIKELY(definitions == NULL))
  {
    return NULL;
  }
  
  return (const JSClassDefinition *) g_hash_table_lookup(definitions,
                                                         (gpointer)class_name);
}

JSClassRef
jsclass_lookup(const JSClassDefinition *definition)
{
  JSClassRef jsclass;
  
  if (G_UNLIKELY(classes == NULL))
  {
    classes = g_hash_table_new(NULL, NULL);
  }
  
  jsclass = g_hash_table_lookup(classes, (gpointer)definition);
  if (G_UNLIKELY(jsclass == NULL))
  {
    jsclass = JSClassCreate(definition);
    g_hash_table_insert(classes, (gpointer)definition, (gpointer)jsclass);
  }
  
  return jsclass;
}

