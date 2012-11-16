#! /usr/bin/python -u
# -*- coding: utf-8 -*-
# vim: set fileencoding=utf-8 :#
#
# Copyright (C) 2012 Intel Corporation
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) version 3.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
# 02110-1301  USA

# Changes sorting, active address books and/or searches.
# Run with no arguments to see the current state.
#
# Examples:
# search.py --order=last/first
# search.py --order=first/last
# search.py --active-address-book '' --active-address-book 'peer-foobar'
# search.py --search "[]"
# search.py --search "[('any-contains', 'Joe')]"
# search.py --search "[('phone', '+49891234')]"
#
# When searching, the script will print the results as they come in,
# then continue waiting for changes until interrupted via CTRL-C.

import dbus
import dbus.service
import gobject
from dbus.mainloop.glib import DBusGMainLoop
import functools
import sys
import traceback
import itertools
from optparse import OptionParser

VERBOSITY_INFO = 0
VERBOSITY_NOTIFICATIONS = 1
VERBOSITY_DATA_SUMMARY = 2
VERBOSITY_DATA_FULL = 3
VERBOSITY_DEBUG = 4

parser = OptionParser()
parser.add_option("-a", "--active-address-book", dest="address_books",
                  action="append", default=[],
                  help="Set one active address book, repeat to activate more than one. "
                  "Default is to leave the current set unchanged.",
                  metavar="ADDRESS-BOOK-ID")
parser.add_option("-s", "--search", dest="search",
                  default=None,
                  help="Search expression in Python syntax. "
                  "Default is to not search at all.",
                  metavar="SEARCH-EXPRESSION")
parser.add_option("-o", "--order", dest="order",
                  default=None,
                  help="Set new global sort order. Default is to use the existing one.",
                  metavar="ORDER-NAME")
parser.add_option("--verbosity",
                  default=VERBOSITY_DATA_FULL,
                  type="int",
                  help="Determine what is printed. "
                  "0 = only minimal progress messages. "
                  "1 = also summary of view notifications. "
                  "2 = also a one-line entry per contact. "
                  "3 = also a dump of the contact. "
                  "4 = also debug output for the script itself. "
                  )
(options, args) = parser.parse_args()

DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
loop = gobject.MainLoop()

# The example does all calls to D-Bus with a very long timeout.  A
# real app should instead either never time out (because all of the
# calls can, in theory, take arbitrarily long to complete) or be
# prepared to deal with timeouts.
timeout = 100000

# Contact PIM Manager.
manager = dbus.Interface(bus.get_object('org._01.pim.contacts',
                                        '/org/01/pim/contacts'),
                         'org._01.pim.contacts.Manager')

# Simplify the output of values returned via D-Bus by replacing
# types like dbus.Dictionary with a normal Python dictionary
# and by sorting lists (the order of all list entries never matters
# in the contact dictionary).
dbus_type_mapping = {
    dbus.Array: list,
    dbus.Boolean: bool,
    dbus.Byte: int,
    dbus.Dictionary: dict,
    dbus.Double: float,
    dbus.Int16: int,
    dbus.Int32: int,
    dbus.Int64: long,
    dbus.ObjectPath: str,
    dbus.Signature: str,
    dbus.String: unicode,
    dbus.Struct: tuple,
    dbus.UInt16: int,
    dbus.UInt32: int,
    dbus.UInt64: long,
    dbus.UTF8String: unicode
    }

def strip_dbus(instance):
    base = dbus_type_mapping.get(type(instance), None)
    if base == dict or isinstance(instance, dict):
        return dict([(strip_dbus(k), strip_dbus(v)) for k, v in instance.iteritems()])
    if base == list or isinstance(instance, list):
        l = [strip_dbus(v) for v in instance]
        l.sort()
        return l
    if base == tuple or isinstance(instance, tuple):
        return tuple([strip_dbus(v) for v in instance])
    if base == None:
        return instance
    if base == unicode:
        # try conversion to normal string
        try:
            return str(instance)
        except UnicodeEncodeError:
            pass
    return base(instance)

def nothrow(fn):
    '''Function decorator which dumps exceptions to stdout. Use for callbacks.'''

    @functools.wraps(fn)
    def wrapper(*a, **b):
        try:
            fn(*a, **b)
        except:
            print traceback.format_exc()

    return wrapper

class ContactsView(dbus.service.Object):
     '''Implements ViewAgent. Logs changes to stdout and maintains+shows the current content.'''

     def __init__(self):
          '''Create ViewAgent with the chosen path.'''
          # A real app would have to ensure that the path is unique for the
          # process. This example only has one ViewAgent and thus can use
          # a fixed path.
          self.path = '/org/syncevolution/search'
          # Currently known contact data, size matches view.
          self.contacts = []

          dbus.service.Object.__init__(self, dbus.SessionBus(), self.path)

     def search(self, filter):
          '''Start a search.'''
          print 'searching: %s' % filter
          self.viewPath = manager.Search(filter, self.path,
                                         timeout=100000)
          # This example uses the ViewControl to read contact data.
          # It does not close the view explicitly when
          # terminating. Instead it relies on the PIM Manager to
          # detect that the client disconnects from D-Bus.
          # Alternatively a client can also remove only its ViewAgent,
          # which will be noticed by the PIM Manager the next time it
          # tries to send a change.
          self.view = dbus.Interface(bus.get_object(manager.bus_name,
                                                    self.viewPath),
                                     'org._01.pim.contacts.ViewControl')

     def read(self, ids):
         '''Read contact data which was modified or added.'''
         self.view.ReadContacts(ids,
                                timeout=100000,
                                reply_handler=lambda x: self.ContactsRead(ids, x),
                                error_handler=lambda x: self.ReadFailed(ids, x))

     def dump(self, start, count):
         '''Show content of view. Highlight the contacts in the given range.'''

         if options.verbosity < VERBOSITY_DATA_SUMMARY:
             return

         for index, contact in enumerate(self.contacts):
             if start == index:
                 # empty line with marker where range starts
                 print '=> '
             print '%s %03d %s' % \
                 (start != None and index >= start and index < start + count and '*' or ' ',
                  index,
                  isinstance(contact, dict) and contact.get('full-name', '<<unnamed>>') or '<<reading...>>')
             if options.verbosity >= VERBOSITY_DATA_FULL:
                 print '    ', strip_dbus(contact)
                 print

     @nothrow
     def ContactsRead(self, ids, contacts):
         if options.verbosity >= VERBOSITY_DATA_FULL:
             print 'got contact data %s => %s ' % (ids, strip_dbus(contacts))
         min = len(contacts)
         max = -1
         for index, contact in contacts:
             if index >= 0:
                 self.contacts[index] = contact
                 if min > index:
                     min = index
                 if max < index:
                     max = index
         if max > 0:
             self.dump(min, max - min + 1)

     @nothrow
     def ReadFailed(self, ids, error):
         print 'request for contact data %s failed: %s' % \
             (ids, error)

     @nothrow
     @dbus.service.method(dbus_interface='org._01.pim.contacts.ViewAgent',
                          in_signature='oias', out_signature='')
     def ContactsModified(self, view, start, ids):
         if options.verbosity >= VERBOSITY_NOTIFICATIONS:
             print 'contacts modified: %s, start %d, count %d, ids %s' % \
                 (view, start, len(ids),
                  options.verbosity >= VERBOSITY_DATA_SUMMARY and strip_dbus(ids) or '<...>')
         self.contacts[start:start + len(ids)] = ids
         self.dump(start, len(ids))
         self.read(ids)

     @nothrow
     @dbus.service.method(dbus_interface='org._01.pim.contacts.ViewAgent',
                          in_signature='oias', out_signature='')
     def ContactsAdded(self, view, start, ids):
         if options.verbosity >= VERBOSITY_NOTIFICATIONS:
             print 'contacts added: %s, start %d, count %d, ids %s' % \
                 (view, start, len(ids),
                  options.verbosity >= VERBOSITY_DATA_SUMMARY and strip_dbus(ids) or '<...>')
         self.contacts[start:start] = ids
         self.dump(start, len(ids))
         self.read(ids)

     @nothrow
     @dbus.service.method(dbus_interface='org._01.pim.contacts.ViewAgent',
                          in_signature='oii', out_signature='')
     def ContactsRemoved(self, view, start, count):
         if options.verbosity >= VERBOSITY_NOTIFICATIONS:
             print 'contacts removed: %s, start %d, count %d, ids %s' % \
                 (view, start, len(ids),
                  options.verbosity >= VERBOSITY_DATA_SUMMARY and strip_dbus(ids) or '<...>')
         # Remove obsolete entries.
         del self.contacts[start:start + len(ids)]
         self.dump(start, 0)

     @nothrow
     @dbus.service.method(dbus_interface='org._01.pim.contacts.ViewAgent',
                          in_signature='o', out_signature='')
     def Quiesent(self, view):
         if options.verbosity >= VERBOSITY_NOTIFICATIONS:
             print 'view is stable'

peers = strip_dbus(manager.GetAllPeers())
print 'peers: %s' % peers
print 'available databases: %s' % ([''] + ['peer-' + uid for uid in peers.keys()])

address_books = strip_dbus(manager.GetActiveAddressBooks())
if options.address_books:
    print 'active address books %s -> %s' % (address_books, options.address_books)
    manager.SetActiveAddressBooks(options.address_books)
else:
    print 'active address books: %s' % options.address_books

order = strip_dbus(manager.GetSortOrder())
if options.order:
    print 'active sort order %s -> %s' % (order, options.order)
    manager.SetSortOrder(options.order)
else:
    print 'active sort order: %s' % order

if options.search != None:
    view = ContactsView()
    view.search(eval(options.search))
    loop.run()
else:
    print 'no search expression given, quitting'
