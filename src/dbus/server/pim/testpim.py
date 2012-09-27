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
import traceback
import re
import itertools

# Update path so that testdbus.py can be found.
pimFolder = os.path.realpath(os.path.abspath(os.path.split(inspect.getfile(inspect.currentframe()))[0]))
if pimFolder not in sys.path:
     sys.path.insert(0, pimFolder)
testFolder = os.path.realpath(os.path.abspath(os.path.join(os.path.split(inspect.getfile(inspect.currentframe()))[0], "../../../../test")))
if testFolder not in sys.path:
    sys.path.insert(0, testFolder)

from testdbus import DBusUtil, timeout, property, usingValgrind, xdg_root, bus, logging, loop
import testdbus

class ContactsView(dbus.service.Object, unittest.TestCase):
     '''Implements ViewAgent, starts a search and mirrors the remote state locally.'''

     counter = 1

     def __init__(self, manager):
          '''Create ViewAgent with the chosen path.'''
          self.manager = manager
          # Ensure unique path across different tests.
          self.path = '/org/syncevolution/testpim%d' % ContactsView.counter
          ContactsView.counter = ContactsView.counter + 1
          self.view = None
          # List of encountered errors in ViewAgent, should always be empty.
          self.errors = []
          # Currently known contact data, size matches view.
          self.contacts = []
          # Change events, as list of ("modified/added/removed", start, count).
          self.events = []

          dbus.service.Object.__init__(self, dbus.SessionBus(), self.path)

     def search(self, filter):
          '''Start a search.'''
          self.viewPath = self.manager.Search(filter, self.path)
          self.view = dbus.Interface(bus.get_object(self.manager.bus_name,
                                                    self.viewPath),
                                     'org._01.pim.contacts.ViewControl')

     # A function decorator for the boiler-plate code would be nice...
     # Or perhaps the D-Bus API should merge the three different callbacks
     # into one?

     @dbus.service.method(dbus_interface='org._01.pim.contacts.ViewAgent',
                          in_signature='oii', out_signature='')
     def ContactsModified(self, view, start, count):
          try:
               logging.log('contacts modified: %s, start %d, count %d' %
                           (view, start, count))
               self.events.append(('modified', start, count))
               assert view == self.viewPath
               assert start >= 0
               assert count >= 0
               assert start + count <= len(self.contacts)
               self.contacts[start:start + count] = itertools.repeat(None, count)
               logging.printf('contacts modified => %s', self.contacts)
          except:
               self.errors.append(traceback.format_exc())


     @dbus.service.method(dbus_interface='org._01.pim.contacts.ViewAgent',
                          in_signature='oii', out_signature='')
     def ContactsAdded(self, view, start, count):
          try:
               logging.log('contacts added: %s, start %d, count %d' %
                           (view, start, count))
               self.events.append(('added', start, count))
               assert view == self.viewPath
               assert start >= 0
               assert count >= 0
               assert start <= len(self.contacts)
               for i in range(0, count):
                    self.contacts.insert(start, None)
               logging.printf('contacts added => %s', self.contacts)
          except:
               self.errors.append(traceback.format_exc())

     @dbus.service.method(dbus_interface='org._01.pim.contacts.ViewAgent',
                          in_signature='oii', out_signature='')
     def ContactsRemoved(self, view, start, count):
          try:
               logging.log('contacts removed: %s, start %d, count %d' %
                           (view, start, count))
               self.events.append(('removed', start, count))
               assert view == self.viewPath
               assert start >= 0
               assert count >= 0
               assert start + count <= len(self.contacts)
               del self.contacts[start:start + count]
               logging.printf('contacts removed => %s', self.contacts)
          except:
               self.errors.append(traceback.format_exc())

     def read(self, index, count=1):
          '''Read the specified range of contact data.'''
          self.view.ReadContacts(index, count,
                                 timeout=100000,
                                 reply_handler=lambda x: self.contacts.__setslice__(index, index+len(x), x),
                                 error_handler=lambda x: self.errors.append(x))

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

    def setUpView(self):
        '''Set up a a 'foo' peer and create a view for it.'''
        # Ignore all currently existing EDS databases.
        self.sources = self.currentSources()
        self.expected = self.sources.copy()
        self.peers = {}

        # dummy peer directory
        self.contacts = os.path.abspath(os.path.join(xdg_root, 'contacts'))
        os.makedirs(self.contacts)

        # add foo
        self.uid = self.uidPrefix + 'foo'
        self.peers[self.uid] = {'protocol': 'PBAP',
                                'address': 'xxx'}
        self.manager.SetPeer(self.uid,
                             self.peers[self.uid],
                             timeout=self.timeout)
        self.expected.add(self.managerPrefix + self.uid)
        self.assertEqual(self.peers, self.manager.GetAllPeers(timeout=self.timeout))
        self.assertEqual(self.expected, self.currentSources())

        # Limit active databases to the one we just created.
        # TODO: self.manager.SetActiveAddressBooks(['peer-' + self.uid])

        # Start view. We don't know the current state, so give it some time to settle.
        self.view = ContactsView(self.manager)
        self.view.search([])
        time.sleep(5)

        # Delete all local data in 'foo' cache.
        logging.log('deleting all items')
        self.runCmdline(['--delete-items', '@' + self.managerPrefix + self.uid, 'local', '*'])

        # Run until view is empty.
        self.runUntil('empty view',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 0)

        # Clear unknown sequence of events.
        self.view.events = []


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

    def extractLUIDs(self, out):
         '''Extract the LUIDs from syncevolution --import/update output.'''
         r = re.compile(r'''#.*: (\S+)\n''')
         matches = r.split(out)
         # Even entry is text (empty here), odd entry is the match group.
         return matches[1::2]

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

    @timeout(60)
    def testView(self):
        '''TestContacts.testView - test making changes to the unified address book'''
        self.setUpView()

        # Insert new contact.
        john = '''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
END:VCARD'''
        item = os.path.join(self.contacts, 'john.vcf')
        output = open(item, "w")
        output.write(john)
        output.close()
        logging.log('inserting John')
        out, err, returncode = self.runCmdline(['--import', item, '@' + self.managerPrefix + self.uid, 'local'])
        luid = self.extractLUIDs(out)[0]

        # Run until the view has adapted.
        self.runUntil('view with one contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 1)

        # Check for the one expected event.
        self.assertEqual([('added', 0, 1)], self.view.events)
        self.view.events = []

        # Read contact.
        logging.log('reading contact')
        self.view.read(0)
        self.runUntil('contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[0] != None)
        self.assertEqual('John Doe', self.view.contacts[0]['full-name'])

        # Update contacts.
        johnBDay = '''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
BDAY:20120924
END:VCARD'''
        output = open(item, "w")
        output.write(johnBDay)
        output.close()
        logging.log('updating John')
        self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luid])
        self.runUntil('view with changed contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[0] == None)
        self.assertEqual([('modified', 0, 1)], self.view.events)
        self.view.events = []

        # Remove contact.
        self.runCmdline(['--delete-items', '@' + self.managerPrefix + self.uid, 'local', '*'])
        self.runUntil('view without contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 0)
        self.assertEqual([('removed', 0, 1)], self.view.events)
        self.view.events = []


    @timeout(60)
    def testViewSorting(self):
        '''TestContacts.testViewSorting - check that sorting works'''
        self.setUpView()

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate(['''BEGIN:VCARD
VERSION:3.0
FN:Abraham Zoo
N:Zoo;Abraham
END:VCARD''',

'''BEGIN:VCARD
VERSION:3.0
FN:Benjamin Yeah
N:Yeah;Benjamin
END:VCARD''',

'''BEGIN:VCARD
VERSION:3.0
FN:Charly Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = open(item, "w")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
        # Relies on importing contacts sorted ascending by file name.
        luids = self.extractLUIDs(out)
        logging.printf('created contacts with luids: %s' % luids)

        # Run until the view has adapted.
        self.runUntil('view with three contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 3)

        # Check for the one expected event.
        # TODO: self.assertEqual([('added', 0, 3)], view.events)
        self.view.events = []

        # Read contacts.
        logging.log('reading contacts')
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[0] != None)
        self.assertEqual('Charly Xing', self.view.contacts[0]['full-name'])
        self.assertEqual('Benjamin Yeah', self.view.contacts[1]['full-name'])
        self.assertEqual('Abraham Zoo', self.view.contacts[2]['full-name'])

        # No re-ordering necessary: criteria doesn't change.
        item = os.path.join(self.contacts, 'contact0.vcf')
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:Abraham Zoo
N:Zoo;Abraham
BDAY:20120924
END:VCARD''')
        output.close()
        self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('view with changed contact #2',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[2] == None)
        self.assertEqual([('modified', 2, 1)], self.view.events)
        self.view.events = []
        logging.log('reading updated contact')
        self.view.read(2, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[2] != None)
        self.assertEqual('Charly Xing', self.view.contacts[0]['full-name'])
        self.assertEqual('Benjamin Yeah', self.view.contacts[1]['full-name'])
        self.assertEqual('Abraham Zoo', self.view.contacts[2]['full-name'])

        # No re-ordering necessary: criteria changes, but not in a relevant way, at end.
        item = os.path.join(self.contacts, 'contact0.vcf')
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:Abraham Zoo
N:Zoo;Abraham;Middle
BDAY:20120924
END:VCARD''')
        output.close()
        self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('view with changed contact #2',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[2] == None)
        self.assertEqual([('modified', 2, 1)], self.view.events)
        self.view.events = []
        logging.log('reading updated contact')
        self.view.read(2, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[2] != None)
        self.assertEqual('Charly Xing', self.view.contacts[0]['full-name'])
        self.assertEqual('Benjamin Yeah', self.view.contacts[1]['full-name'])
        self.assertEqual('Abraham Zoo', self.view.contacts[2]['full-name'])

        # No re-ordering necessary: criteria changes, but not in a relevant way, in the middle.
        item = os.path.join(self.contacts, 'contact1.vcf')
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:Benjamin Yeah
N:Yeah;Benjamin;Middle
END:VCARD''')
        output.close()
        self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[1]])
        self.runUntil('view with changed contact #1',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[1] == None)
        self.assertEqual([('modified', 1, 1)], self.view.events)
        self.view.events = []
        logging.log('reading updated contact')
        self.view.read(1, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[1] != None)
        self.assertEqual('Charly Xing', self.view.contacts[0]['full-name'])
        self.assertEqual('Benjamin Yeah', self.view.contacts[1]['full-name'])
        self.assertEqual('Abraham Zoo', self.view.contacts[2]['full-name'])

        # No re-ordering necessary: criteria changes, but not in a relevant way, in the middle.
        item = os.path.join(self.contacts, 'contact2.vcf')
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:Charly Xing
N:Xing;Charly;Middle
END:VCARD''')
        output.close()
        self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[2]])
        self.runUntil('view with changed contact #0',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[0] == None)
        self.assertEqual([('modified', 0, 1)], self.view.events)
        self.view.events = []
        logging.log('reading updated contact')
        self.view.read(0, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[0] != None)
        self.assertEqual('Charly Xing', self.view.contacts[0]['full-name'])
        self.assertEqual('Benjamin Yeah', self.view.contacts[1]['full-name'])
        self.assertEqual('Abraham Zoo', self.view.contacts[2]['full-name'])

        # Re-ordering necessary: last item (Zoo;Abraham) becomes first (Ace;Abraham).
        item = os.path.join(self.contacts, 'contact0.vcf')
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:Abraham Ace
N:Ace;Abraham;Middle
BDAY:20120924
END:VCARD''')
        output.close()
        self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('view with changed contact #0',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[0] == None)
        self.assertEqual([('removed', 2, 1),
                          ('added', 0, 1)], self.view.events)
        self.view.events = []
        logging.log('reading updated contact')
        self.view.read(0, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[0] != None)
        self.assertEqual('Abraham Ace', self.view.contacts[0]['full-name'])
        self.assertEqual('Charly Xing', self.view.contacts[1]['full-name'])
        self.assertEqual('Benjamin Yeah', self.view.contacts[2]['full-name'])

        # Re-ordering necessary: first item (Ace;Abraham) becomes last (Zoo;Abraham).
        item = os.path.join(self.contacts, 'contact0.vcf')
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:Abraham Zoo
N:Zoo;Abraham;Middle
BDAY:20120924
END:VCARD''')
        output.close()
        self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('view with changed contact #2',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[2] == None)
        self.assertEqual([('removed', 0, 1),
                          ('added', 2, 1)], self.view.events)
        self.view.events = []
        logging.log('reading updated contact')
        self.view.read(2, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[2] != None)
        self.assertEqual('Charly Xing', self.view.contacts[0]['full-name'])
        self.assertEqual('Benjamin Yeah', self.view.contacts[1]['full-name'])
        self.assertEqual('Abraham Zoo', self.view.contacts[2]['full-name'])

    @timeout(60)
    def testRead(self):
        '''TestContacts.testRead - check that folks and PIM Manager deliver EDS data correctly'''
        self.setUpView()

        # Insert new contacts.
        #
        # Not all of the vCard properties need to be available via PIM Manager.
        testcases = ['''BEGIN:VCARD
VERSION:3.0
URL:http://john.doe.com
TITLE:Senior Tester
ORG:Test Inc.;Testing;test#1
ROLE:professional test case
X-EVOLUTION-MANAGER:John Doe Senior
X-EVOLUTION-ASSISTANT:John Doe Junior
NICKNAME:user1
BDAY:2006-01-08
X-FOOBAR-EXTENSION;X-FOOBAR-PARAMETER=foobar:has to be stored internally by engine and preserved in testExtensions test\; never sent to a peer
X-TEST;PARAMETER1=nonquoted;PARAMETER2="quoted because of spaces":Content with\nMultiple\nText lines\nand national chars: äöü
X-EVOLUTION-ANNIVERSARY:2006-01-09
X-EVOLUTION-SPOUSE:Joan Doe
NOTE:This is a test case which uses almost all Evolution fields.
FN:John Doe
N:Doe;John;;;
X-EVOLUTION-FILE-AS:Doe\, John
CATEGORIES:TEST
X-EVOLUTION-BLOG-URL:web log
CALURI:calender
FBURL:free/busy
X-EVOLUTION-VIDEO-URL:chat
X-MOZILLA-HTML:TRUE
ADR;TYPE=WORK:Test Box #2;;Test Drive 2;Test Town;Upper Test County;12346;O
 ld Testovia
LABEL;TYPE=WORK:Test Drive 2\nTest Town\, Upper Test County\n12346\nTest Bo
 x #2\nOld Testovia
ADR;TYPE=HOME:Test Box #1;;Test Drive 1;Test Village;Lower Test County;1234
 5;Testovia
LABEL;TYPE=HOME:Test Drive 1\nTest Village\, Lower Test County\n12345\nTest
  Box #1\nTestovia
ADR:Test Box #3;;Test Drive 3;Test Megacity;Test County;12347;New Testonia
LABEL;TYPE=OTHER:Test Drive 3\nTest Megacity\, Test County\n12347\nTest Box
  #3\nNew Testonia
UID:pas-id-43C0ED3900000001
EMAIL;TYPE=WORK;X-EVOLUTION-UI-SLOT=1:john.doe@work.com
EMAIL;TYPE=HOME;X-EVOLUTION-UI-SLOT=2:john.doe@home.priv
EMAIL;TYPE=OTHER;X-EVOLUTION-UI-SLOT=3:john.doe@other.world
EMAIL;TYPE=OTHER;X-EVOLUTION-UI-SLOT=4:john.doe@yet.another.world
TEL;TYPE=work;TYPE=Voice;X-EVOLUTION-UI-SLOT=1:business 1
TEL;TYPE=homE;TYPE=VOICE;X-EVOLUTION-UI-SLOT=2:home 2
TEL;TYPE=CELL;X-EVOLUTION-UI-SLOT=3:mobile 3
TEL;TYPE=WORK;TYPE=FAX;X-EVOLUTION-UI-SLOT=4:businessfax 4
TEL;TYPE=HOME;TYPE=FAX;X-EVOLUTION-UI-SLOT=5:homefax 5
TEL;TYPE=PAGER;X-EVOLUTION-UI-SLOT=6:pager 6
TEL;TYPE=CAR;X-EVOLUTION-UI-SLOT=7:car 7
TEL;TYPE=PREF;X-EVOLUTION-UI-SLOT=8:primary 8
X-AIM;X-EVOLUTION-UI-SLOT=1:AIM JOHN
X-YAHOO;X-EVOLUTION-UI-SLOT=2:YAHOO JDOE
X-ICQ;X-EVOLUTION-UI-SLOT=3:ICQ JD
X-GROUPWISE;X-EVOLUTION-UI-SLOT=4:GROUPWISE DOE
X-GADUGADU:GADUGADU DOE
X-JABBER:JABBER DOE
X-MSN:MSN DOE
X-SKYPE:SKYPE DOE
X-SIP:SIP DOE
PHOTO;ENCODING=b;TYPE=JPEG:/9j/4AAQSkZJRgABAQEASABIAAD/4QAWRXhpZgAATU0AKgAA
 AAgAAAAAAAD//gAXQ3JlYXRlZCB3aXRoIFRoZSBHSU1Q/9sAQwAFAwQEBAMFBAQEBQUFBgcM
 CAcHBwcPCwsJDBEPEhIRDxERExYcFxMUGhURERghGBodHR8fHxMXIiQiHiQcHh8e/9sAQwEF
 BQUHBgcOCAgOHhQRFB4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4e
 Hh4eHh4eHh4e/8AAEQgAFwAkAwEiAAIRAQMRAf/EABkAAQADAQEAAAAAAAAAAAAAAAAGBwgE
 Bf/EADIQAAECBQMCAwQLAAAAAAAAAAECBAADBQYRBxIhEzEUFSIIFjNBGCRHUVZ3lqXD0+P/
 xAAUAQEAAAAAAAAAAAAAAAAAAAAA/8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAwDAQACEQMR
 AD8AuX6UehP45/aXv9MTPTLVKxNSvMPcqu+a+XdLxf1SfJ6fU37PioTnOxfbOMc/KIZ7U/2V
 fmTR/wCaKlu6+blu/Ui72zxWtUmmUOrTaWwkWDT09FPR4K587OVrUfVsIwElPPPAbAjxr2um
 hWXbDu5rmfeApLPZ4hx0lzNm9aUJ9KAVHKlJHAPf7ozPLqWt9y6Z0EPGmoLNjTq48a1iaybJ
 YV52yEtCms5KJmAT61JXtJyUdyQTEc1WlMql7N1/oZ6jagVZVFfUyZPpFy5lvWcxU7Z03BUk
 GZLWJqVhPYLkIIPBEBtSEUyNAsjI1q1m/VP+UICwL/sqlXp7v+aOHsnyGttq218MtKd8+Ru2
 JXuScoO45Awe2CIi96aKW1cVyubkYVy6rTqz0J8a5t2qqZl0UjAMwYKScfPAJ+cIQHHP0Dth
 VFaMWt0XwxetnM50Ks2rsxL6ZMnJlJmb5hBBBEiVxjA28dznqo+hdksbQuS3Hs6tVtNzdM1Z
 /VH5nO3Bl/CJmYHKDynjv3zCEB5rLQNo0bIbydWNWxKljbLQLoWkISOAkBKAABCEID//2Q==
END:VCARD
''']
        for i, contact in enumerate(testcases):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = open(item, "w")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
        # Relies on importing contacts sorted ascending by file name.
        luids = self.extractLUIDs(out)
        logging.printf('created contacts with luids: %s' % luids)

        # Run until the view has adapted.
        self.runUntil('view with contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == len(testcases))

        # Check for the one expected event.
        # TODO: self.assertEqual([('added', 0, len(testcases))], view.events)
        self.view.events = []

        # Read contacts.
        logging.log('reading contacts')
        self.view.read(0, len(testcases))
        self.runUntil('contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.contacts[0] != None)
        contact = copy.deepcopy(self.view.contacts[0])
        # Simplify the photo URI, if there was one. Avoid any assumptions
        # about the filename, except that it is a file:/// uri.
        if contact.has_key('photo'):
             contact['photo'] = re.sub('^file:///.*', 'file:///<stripped>', contact['photo'])
        self.assertEqual({'full-name': 'John Doe',
                          'nickname': 'user1',
                          'structured-name': {'given': 'John', 'family': 'Doe'},
                          'birthday': (2006, 1, 8),
                          'photo': 'file:///<stripped>',
                          'roles': [
                       {
                        'organisation': 'Test Inc.',
                        'role': 'professional test case',
                        'title': 'Senior Tester',
                        },
                       ],
                          'source': [
                       ('test-dbus-foo', luids[0])
                       ],
                          'notes': [
                       'This is a test case which uses almost all Evolution fields.',
                       ],
                          'emails': [
                       ('john.doe@home.priv', ['home']),
                       ('john.doe@other.world', ['other']),
                       ('john.doe@work.com', ['work']),
                       ('john.doe@yet.another.world', ['other']),
                       ],
                          'phones': [
                       ('business 1', ['voice', 'work']),
                       ('businessfax 4', ['fax', 'work']),
                       ('car 7', ['car']),
                       ('home 2', ['home', 'voice']),
                       ('homefax 5', ['fax', 'home']),
                       ('mobile 3', ['cell']),
                       ('pager 6', ['pager']),
                       ('primary 8', ['pref']),
                       ],
                          'addresses': [
                       ({'country': 'New Testonia',
                         'locality': 'Test Megacity',
                         'po-box': 'Test Box #3',
                         'postal-code': '12347',
                         'region': 'Test County',
                         'street': 'Test Drive 3'},
                        []),
                       ({'country': 'Old Testovia',
                         'locality': 'Test Town',
                         'po-box': 'Test Box #2',
                         'postal-code': '12346',
                         'region': 'Upper Test County',
                         'street': 'Test Drive 2'},
                        ['work']),
                       ({'country': 'Testovia',
                         'locality': 'Test Village',
                         'po-box': 'Test Box #1',
                         'postal-code': '12345',
                         'region': 'Lower Test County',
                         'street': 'Test Drive 1'},
                        ['home']),
                       ],
                          'urls': [
                       ('chat', ['x-video']),
                       ('free/busy', ['x-free-busy']),
                       ('http://john.doe.com', ['x-home-page']),
                       ('web log', ['x-blog']),
                       ],
                          },
                         # Order of list entries in the result is not specified.
                         # Must sort before comparing.
                         contact,
                         sortLists=True)


if __name__ == '__main__':
    unittest.main()
