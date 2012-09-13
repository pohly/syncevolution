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

# PIM Manager specific tests, using Python unittest as framework.
#
# Run with "syncevolution", "synccompare" and "syncevo-dbus-server" in
# the PATH.
#
# Uses the normal testdbus.py infrastructure by including that file.
# Can be run directly from the SyncEvolution source code or after
# copying test/testdbus.py and src/dbus/server/pim/testpim.py into the
# same directory.

import os
import sys
import inspect
import unittest
import time
import copy
import subprocess
import dbus

# Update path so that testdbus.py can be found.
pimFolder = os.path.realpath(os.path.abspath(os.path.split(inspect.getfile(inspect.currentframe()))[0]))
if pimFolder not in sys.path:
     sys.path.insert(0, pimFolder)
testFolder = os.path.realpath(os.path.abspath(os.path.join(os.path.split(inspect.getfile(inspect.currentframe()))[0], "../../../../test")))
if testFolder not in sys.path:
    sys.path.insert(0, testFolder)

from testdbus import DBusUtil, timeout, usingValgrind, xdg_root, bus
import testdbus


class TestContacts(DBusUtil, unittest.TestCase):
    """Tests for org._01.pim.contacts API.

The tests use the system's EDS, which must be >= 3.6.
They create additional databases in EDS under the normal
location. This is necessary because the tests cannot
tell the EDS source registry daemon to run with a different
XDG root.
"""

    def setUp(self):
        self.manager = dbus.Interface(bus.get_object('org._01.pim.contacts',
                                                     '/org/01/pim/contacts'),
                                      'org._01.pim.contacts.Manager')

        # Determine location of EDS source configs.
        config = os.environ.get("XDG_CONFIG_HOME", None)
        if config:
            self.sourcedir = os.path.join(config, "evolution", "sources")
        else:
            self.sourcedir = os.path.expanduser("~/.config/evolution/sources")

        # SyncEvolution uses a local temp dir.
        self.configdir = os.path.join(xdg_root, "syncevolution")

        # Choose a very long timeout for method calls;
        # 'infinite' doesn't seem to be documented for Python.
        self.timeout = 100000

        # Common prefix for peer UIDs.
        self.uidPrefix = 'test-dbus-'

        # Prefix used by PIM Manager in EDS.
        self.managerPrefix = 'pim-manager-'

        # Remove all sources and configs which were created by us
        # before running the test.
        removed = False;
        for source in os.listdir(self.sourcedir):
            if source.startswith(self.managerPrefix + self.uidPrefix):
                os.unlink(os.path.join(self.sourcedir, source))
                removed = True
        if removed:
            # Give EDS time to notice the removal.
            time.sleep(5)


    def run(self, result):
        # Runtime varies a lot when using valgrind, because
        # of the need to check an additional process. Allow
        # a lot more time when running under valgrind.
        self.runTest(result, own_xdg=True, own_home=True,
                     defTimeout=usingValgrind() and 600 or 20)

    def currentSources(self):
        '''returns current set of EDS sources as set of UIDs, without the .source suffix'''
        return set([os.path.splitext(x)[0] for x in os.listdir(self.sourcedir)])

    def testConfig(self):
        '''TestContacts.testConfig - set and remove peers'''
        sources = self.currentSources()
        expected = sources.copy()
        peers = {}

        # add foo
        uid = self.uidPrefix + 'foo'
        peers[uid] = {'protocol': 'PBAP',
                      'address': 'xxx'}
        self.manager.SetPeer(uid,
                             peers[uid],
                             timeout=self.timeout)
        expected.add(self.managerPrefix + uid)
        self.assertEqual(peers, self.manager.GetAllPeers(timeout=self.timeout))
        self.assertEqual(expected, self.currentSources())

        # TODO: work around EDS bug: e_source_remove_sync() quickly after
        # e_source_registry_create_sources_sync() leads to an empty .source file:
        # [Data Source]
        # DisplayName=Unnamed
        # Enabled=true
        # Parent=
        #
        # That prevents reusing the same UID.
        time.sleep(2)

        # remove foo
        expected.remove(self.managerPrefix + uid)
        del peers[uid]
        self.manager.RemovePeer(uid,
                                timeout=self.timeout)
        self.assertEqual(peers, self.manager.GetAllPeers(timeout=self.timeout))
        self.assertEqual(expected, self.currentSources())

        # add and remove foo again
        uid = self.uidPrefix + 'foo4' # work around EDS bug with reusing UID
        peers[uid] = {'protocol': 'PBAP',
                      'address': 'xxx'}
        self.manager.SetPeer(uid,
                             peers[uid],
                             timeout=self.timeout)
        expected.add(self.managerPrefix + uid)
        self.assertEqual(peers, self.manager.GetAllPeers(timeout=self.timeout))
        self.assertEqual(expected, self.currentSources())
        time.sleep(2)
        expected.remove(self.managerPrefix + uid)
        del peers[uid]
        self.manager.RemovePeer(uid,
                                timeout=self.timeout)
        self.assertEqual(peers, self.manager.GetAllPeers(timeout=self.timeout))
        self.assertEqual(expected, self.currentSources())

        # add foo, bar, xyz
        uid = self.uidPrefix + 'foo2'
        peers[uid] = {'protocol': 'PBAP',
                      'address': 'xxx'}
        self.manager.SetPeer(uid,
                             peers[uid],
                             timeout=self.timeout)
        expected.add(self.managerPrefix + uid)
        self.assertEqual(peers, self.manager.GetAllPeers(timeout=self.timeout))
        self.assertEqual(expected, self.currentSources())

        uid = self.uidPrefix + 'bar'
        peers[uid] = {'protocol': 'PBAP',
                      'address': 'yyy'}
        self.manager.SetPeer(uid,
                             peers[uid],
                             timeout=self.timeout)
        expected.add(self.managerPrefix + uid)
        self.assertEqual(peers, self.manager.GetAllPeers(timeout=self.timeout))
        self.assertEqual(expected, self.currentSources())

        uid = self.uidPrefix + 'xyz'
        peers[uid] = {'protocol': 'PBAP',
                      'address': 'zzz'}
        self.manager.SetPeer(uid,
                             peers[uid],
                             timeout=self.timeout)
        expected.add(self.managerPrefix + uid)
        self.assertEqual(peers, self.manager.GetAllPeers(timeout=self.timeout))
        self.assertEqual(expected, self.currentSources())

        # EDS workaround
        time.sleep(2)

        # remove yxz, bar, foo
        expected.remove(self.managerPrefix + uid)
        del peers[uid]
        self.manager.RemovePeer(uid,
                                timeout=self.timeout)
        self.assertEqual(peers, self.manager.GetAllPeers(timeout=self.timeout))
        self.assertEqual(expected, self.currentSources())

        # EDS workaround
        time.sleep(2)

        uid = self.uidPrefix + 'bar'
        expected.remove(self.managerPrefix + uid)
        del peers[uid]
        self.manager.RemovePeer(uid,
                                timeout=self.timeout)
        self.assertEqual(peers, self.manager.GetAllPeers(timeout=self.timeout))
        self.assertEqual(expected, self.currentSources())

        # EDS workaround
        time.sleep(2)

        uid = self.uidPrefix + 'foo2'
        expected.remove(self.managerPrefix + uid)
        del peers[uid]
        self.manager.RemovePeer(uid,
                                timeout=self.timeout)
        self.assertEqual(peers, self.manager.GetAllPeers(timeout=self.timeout))
        self.assertEqual(expected, self.currentSources())

        # EDS workaround
        time.sleep(2)


if __name__ == '__main__':
    unittest.main()
