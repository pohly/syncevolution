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

#ifndef __JSCOREBUS_CLASSFACTORY_H___
#define __JSCOREBUS_CLASSFACTORY_H___

G_BEGIN_DECLS

void
jsclassdef_insert(const char *class_name, const JSClassDefinition *definition);

const JSClassDefinition *
jsclassdef_lookup(const char *class_name);

JSClassRef
jsclass_lookup(const JSClassDefinition *definition);

G_END_DECLS

#endif /* __JSCOREBUS_CLASSFACTORY_H___ */

