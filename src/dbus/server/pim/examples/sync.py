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

# Configure, sync and/or remove a PBAP-capable phone.
# The peer UID is set to the Bluetooth MAC address.
#
# Examples:
# sync.py --bt-mac A0:4E:04:1E:AD:30 --configure
# sync.py --bt-mac A0:4E:04:1E:AD:30 --sync
# sync.py --bt-mac A0:4E:04:1E:AD:30 --remove
# sync.py --bt-mac A0:4E:04:1E:AD:30 --configure --sync --remove

import dbus
import dbus.service
import gobject
from dbus.mainloop.glib import DBusGMainLoop
import functools
import sys
import traceback
import itertools
from optparse import OptionParser

parser = OptionParser()
parser.add_option("-b", "--bt-mac", dest="mac",
                  default=None,
                  help="Set the Bluetooth MAC address and thus UID of the phone peer.",
                  metavar="aa:bb:cc:dd:ee:ff")
parser.add_option("-c", "--configure",
                  action="store_true", default=False,
                  help="Enable configuring the peer.")
parser.add_option("-s", "--sync",
                  action="store_true", default=False,
                  help="Cache data of peer.")
parser.add_option("-r", "--remove",
                  action="store_true", default=False,
                  help="Remove peer configuration and data.")
(options, args) = parser.parse_args()
if options.configure or options.sync or options.remove:
    if not options.mac:
        sys.exit('--bt-mac parameter must be given')

    # Use MAC address as UID of peer, but with underscores instead of colons
    # and all in lower case. See https://bugs.freedesktop.org/show_bug.cgi?id=56436
    uid = options.mac.replace(':', '_').lower()

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


peers = strip_dbus(manager.GetAllPeers())
print 'peers: %s' % peers
print 'available databases: %s' % ([''] + ['peer-' + uid for uid in peers.keys()])

if options.configure:
    peer = {'protocol': 'PBAP',
            'address': options.mac}
    print 'adding peer config %s = %s' % (uid, peer)
    manager.SetPeer(uid, peer)

if options.sync:
    # Give the operation plenty of time to complete...
    print 'syncing peer %s' % uid
    manager.SyncPeer(uid, timeout=100000)

if options.remove:
    print 'removing peer %s' % uid
    manager.RemovePeer(uid)
