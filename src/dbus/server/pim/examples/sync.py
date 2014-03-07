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
import time
from optparse import OptionParser

# Needed for --poll-progress.
glib = None
try:
    import glib
except ImportError:
    try:
         from gi.repository import GLib as glib
    except ImportError:
         pass

parser = OptionParser()
parser.add_option("-b", "--bt-mac", dest="mac",
                  default=None,
                  help="Set the Bluetooth MAC address and thus UID of the phone peer.",
                  metavar="aa:bb:cc:dd:ee:ff")
parser.add_option("-d", "--debug",
                  action="store_true", default=False,
                  help="Print debug output coming from SyncEvolution server.")
parser.add_option("-p", "--progress",
                  action="store_true", default=False,
                  help="Print progress information during a sync, triggered by PIM Manager signals.")
parser.add_option("", "--poll-progress",
                  action="store", type="float", default=None,
                  help="Print progress information during a sync, pulled via polling at the given frequency.")
parser.add_option("-m", "--mode",
                  action="store", default='',
                  help="Override default PBAP sync mode. One of 'all', 'text', 'incremental' (default).")
parser.add_option("-f", "--progress-frequency",
                  action="store", type="float", default=0.0,
                  help="Override default progress event frequency.")
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
    peername = options.mac.replace(':', '').lower()

DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
loop = gobject.MainLoop()

# Contact PIM Manager.
manager = dbus.Interface(bus.get_object('org._01.pim.contacts',
                                        '/org/01/pim/contacts'),
                         'org._01.pim.contacts.Manager')

# Capture and print debug output.
def log_output(path, level, output, component):
    print '%s %s: %s' % (level, (component or 'sync'), output)

# Format seconds as mm:ss[.mmm].
def format_seconds(seconds, with_milli):
    if with_milli:
        format = '%02d:%06.3f'
    else:
        format = '%02d:%02d'
    return format % (seconds / 60, seconds % 60)

# Keep track of time when progress messages were received.
last = time.time()
start = last
BAR_LENGTH = 20
def log_progress(uid, event, data):
    global last, start
    now = time.time()
    prefix = '%s/+%s:' % (format_seconds(now - start, False),
                          format_seconds(now - last, True))
    if event == 'progress':
        percent = data['percent']
        del data['percent']
        bar = int(percent * BAR_LENGTH) * '-'
        if len(bar) > 0 and len(bar) < BAR_LENGTH:
            bar = bar[0:-1] + '>'
        print prefix, '|%s%s| %.1f%% %s' % (bar, (BAR_LENGTH - len(bar)) * ' ', percent * 100, strip_dbus(data))
    else:
        print prefix, '%s = %s' % (event, strip_dbus(data))
    last = now

if options.debug:
    bus.add_signal_receiver(log_output,
                            "LogOutput",
                            "org.syncevolution.Server",
                            "org.syncevolution",
                            None)
if options.progress:
    bus.add_signal_receiver(log_progress,
                            "SyncProgress",
                            "org._01.pim.contacts.Manager",
                            "org._01.pim.contacts",
                            None)

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

# Call all methods asynchronously, to avoid timeouts and
# to capture debug output while the methods run.
error = None
result = None
def failed(err):
    global error
    error = err
    loop.quit()
def done(*args):
    global result
    loop.quit()
    if len(args) == 1:
        result = args[0]
    elif len(args) > 1:
        result = args
def run():
    global result
    result = None
    loop.run()
    if error:
        print
        print error
        print
    return result
async_args = {
    'reply_handler': done,
    'error_handler': failed,
    'timeout': 100000,   # very large, infinite doesn't seem to be supported by Python D-Bus bindings
}

manager.GetAllPeers(**async_args)
peers = strip_dbus(run())
print 'peers: %s' % peers
print 'available databases: %s' % ([''] + ['peer-' + uid for uid in peers.keys()])

if not error and options.configure:
    peer = {'protocol': 'PBAP',
            'address': options.mac}
    print 'adding peer config %s = %s' % (peername, peer)
    manager.SetPeer(peername, peer, **async_args)
    run()

def pull_progress():
    status = manager.GetPeerStatus(peername)
    print 'Poll status:', strip_dbus(status)
    return True

if not error and options.sync:
    # Do it once before starting the sync.
    if options.poll_progress is not None:
        pull_progress()

    print 'syncing peer %s' % peername
    flags = {}
    if options.progress_frequency != 0.0:
        flags['progress-frequency'] = options.progress_frequency
    if options.mode:
        flags['pbap-sync'] = options.mode
    if flags:
        manager.SyncPeerWithFlags(peername, flags, **async_args)
    else:
        manager.SyncPeer(peername, **async_args)

    # Start regular polling of status.
    timeout = None
    if options.poll_progress is not None:
        timeout = glib.Timeout(int(1 / options.poll_progress * 1000))
        timeout.set_callback(pull_progress)
        timeout.attach(loop.get_context())

    # Wait for completion of sync.
    run()

    # Stop polling, in case that we remove the peer.
    if timeout:
        timeout.destroy()

if not error and options.remove:
    print 'removing peer %s' % peername
    manager.RemovePeer(peername, **async_args)
    run()

if options.debug:
    print "waiting for further debug output, press CTRL-C to stop"
    loop.run()
