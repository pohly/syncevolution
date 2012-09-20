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

from testdbus import DBusUtil, timeout, property, usingValgrind, xdg_root, bus, logging, loop
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

    def runCmdline(self, command, **args):
         '''use syncevolution command line without syncevo-dbus-server, for the sake of keeping code here minimal'''
         cmdline = testdbus.TestCmdline()
         cmdline.setUp()
         try:
              cmdline.running = True
              cmdline.session = None
              # Must use our own self.storedenv here, cmdline doesn't have it.
              c = [ '--daemon=no' ] + command
              logging.printf('running syncevolution command line: %s' % c)
              return cmdline.runCmdline(c,
                                        env=self.storedenv,
                                        sessionFlags=None,
                                        **args)
         finally:
              cmdline.running = False

    def exportCache(self, uid, filename):
        '''dump local cache content into file'''
        self.runCmdline(['--export', filename, '@' + self.managerPrefix + uid, 'local'])

    def compareDBs(self, expected, real, ignoreExtensions=True):
        '''ensure that two sets of items (file or directory) are identical at the semantic level'''
        env = copy.deepcopy(os.environ)
        if ignoreExtensions:
            # Allow the phone to add extensions like X-CLASS=private
            # (seen with Nokia N97 mini - FWIW, the phone should have
            # use CLASS=PRIVATE, because it was using vCard 3.0).
            env['CLIENT_TEST_STRIP_PROPERTIES'] = 'X-[-_a-zA-Z0-9]*'
        sub = subprocess.Popen(['synccompare', expected, real],
                               env=env,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
        stdout, stderr = sub.communicate()
        self.assertEqual(0, sub.returncode,
                         msg=stdout)

    def configurePhone(self, phone, uid, contacts):
        '''set up SyncML for copying all vCard 3.0 files in 'contacts' to the phone, if phone was set'''
        if phone:
             self.runCmdline(['--configure',
                              'syncURL=obex-bt://' + phone,
                              'backend=file',
                              'database=file://' + contacts,
                              'databaseFormat=text/vcard',
                              # Hard-coded Nokia config.
                              'remoteIdentifier=PC Suite',
                              'peerIsClient=1',
                              'uri=Contacts',
                              # Config name and source for syncPhone().
                              'phone@' + self.managerPrefix + uid,
                              'addressbook'])

    def syncPhone(self, phone, uid, syncMode='refresh-from-local'):
        '''use SyncML config for copying all vCard 3.0 files in 'contacts' to the phone, if phone was set'''
        if phone:
             self.runCmdline(['--sync', syncMode,
                              'phone@' + self.managerPrefix + uid,
                              'addressbook'])

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

    @timeout(100)
    def testSync(self):
        '''TestContacts.testSync - test caching of a dummy peer which uses a real phone or a local directory as fallback'''
        sources = self.currentSources()
        expected = sources.copy()
        peers = {}

        # Must be the Bluetooth MAC address (like A0:4E:04:1E:AD:30)
        # of a phone which is paired, currently connected, and
        # supports both PBAP and SyncML. SyncML is needed for putting
        # data onto the phone. Nokia phones like the N97 Mini are
        # known to work and easily available, therefore the test
        # hard-codes the Nokia SyncML settings (could be changed).
        #
        # If set, that phone will be used instead of local sync with
        # the file backend.
        phone = os.environ.get('TEST_DBUS_PBAP_PHONE', None)

        # dummy peer directory
        contacts = os.path.abspath(os.path.join(xdg_root, 'contacts'))
        os.makedirs(contacts)

        # add foo
        uid = self.uidPrefix + 'foo'
        if phone:
            peers[uid] = {'protocol': 'PBAP',
                          'address': phone}
        else:
            peers[uid] = {'protocol': 'files',
                          'address': contacts}
        self.manager.SetPeer(uid,
                             peers[uid],
                             timeout=self.timeout)
        expected.add(self.managerPrefix + uid)
        self.assertEqual(peers, self.manager.GetAllPeers(timeout=self.timeout))
        self.assertEqual(expected, self.currentSources())

        # Throw away data that might have been in the local database.
        self.configurePhone(phone, uid, contacts)
        self.syncPhone(phone, uid)
        self.manager.SyncPeer(uid)
        # TODO: check that syncPhone() really used PBAP - but how?

        # Export data from local database into a file via the --export
        # operation in the syncevo-dbus-server. Depends on (and tests)
        # that the SyncEvolution configuration was created as
        # expected. It does not actually check that EDS is used - the
        # test would also pass for any other storage.
        export = os.path.join(xdg_root, 'local.vcf')
        self.exportCache(uid, export)

        # Server item should be the simple one now, as in the client.
        self.compareDBs(contacts, export)

        # Add a contact.
        john = '''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
END:VCARD'''
        item = os.path.join(contacts, 'john.vcf')
        output = open(item, "w")
        output.write(john)
        output.close()
        self.syncPhone(phone, uid)
        self.manager.SyncPeer(uid)
        self.exportCache(uid, export)
        self.compareDBs(contacts, export)

    @timeout(100)
    @property("ENV", "SYNCEVOLUTION_SYNC_DELAY=200")
    def testSyncAbort(self):
        '''TestContacts.testSyncAbort - test StopSync()'''
        self.setUpServer()
        sources = self.currentSources()
        expected = sources.copy()
        peers = {}

        # dummy peer directory
        contacts = os.path.abspath(os.path.join(xdg_root, 'contacts'))
        os.makedirs(contacts)

        # add foo
        uid = self.uidPrefix + 'foo'
        peers[uid] = {'protocol': 'files',
                      'address': contacts}
        self.manager.SetPeer(uid,
                             peers[uid],
                             timeout=self.timeout)
        expected.add(self.managerPrefix + uid)
        self.assertEqual(peers, self.manager.GetAllPeers(timeout=self.timeout))
        self.assertEqual(expected, self.currentSources())

        # Start a sync. Because of SYNCEVOLUTION_SYNC_DELAY, this will block until
        # we kill it.
        syncCompleted = [ False, False ]
        self.aborted = False
        def result(index, res):
             syncCompleted[index] = res
        def output(path, level, text, procname):
            if self.running and not self.aborted and text == 'ready to sync':
                logging.printf('aborting sync')
                self.manager.StopSync(uid)
                self.aborted = True
        receiver = bus.add_signal_receiver(output,
                                           'LogOutput',
                                           'org.syncevolution.Server',
                                           self.server.bus_name,
                                           byte_arrays=True,
                                           utf8_strings=True)
        try:
             self.manager.SyncPeer(uid,
                                   reply_handler=lambda: result(0, True),
                                   error_handler=lambda x: result(0, x))
             self.manager.SyncPeer(uid,
                                   reply_handler=lambda: result(1, True),
                                   error_handler=lambda x: result(1, x))
             self.runUntil('both syncs done',
                           check=lambda: True,
                           until=lambda: not False in syncCompleted)
        finally:
            receiver.remove()

        # Check for specified error.
        self.assertIsInstance(syncCompleted[0], dbus.DBusException)
        self.assertEqual('org._01.pim.contacts.Manager.Aborted', syncCompleted[0].get_dbus_name())
        self.assertIsInstance(syncCompleted[1], dbus.DBusException)
        self.assertEqual('org._01.pim.contacts.Manager.Aborted', syncCompleted[1].get_dbus_name())

if __name__ == '__main__':
    unittest.main()
