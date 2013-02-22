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
import codecs
import glib

# Update path so that testdbus.py can be found.
pimFolder = os.path.realpath(os.path.abspath(os.path.split(inspect.getfile(inspect.currentframe()))[0]))
if pimFolder not in sys.path:
     sys.path.insert(0, pimFolder)
testFolder = os.path.realpath(os.path.abspath(os.path.join(os.path.split(inspect.getfile(inspect.currentframe()))[0], "../../../../test")))
if testFolder not in sys.path:
    sys.path.insert(0, testFolder)

from testdbus import DBusUtil, timeout, property, usingValgrind, xdg_root, bus, logging, NullLogging, loop
import testdbus

@unittest.skip("not a real test")
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
          # Entry is a string (just the ID is known),
          # a tuple (ID + time when reading started), or
          # a dictionary (actual content known).
          self.contacts = []
          # Change events, as list of ("modified/added/removed", start, count).
          self.events = []
          # Number of times that ViewAgent.Quiescent() was called.
          self.quiescentCount = 0

          self.logging = logging

          dbus.service.Object.__init__(self, dbus.SessionBus(), self.path)
          unittest.TestCase.__init__(self)

     def runTest(self):
          pass

     def search(self, filter):
          '''Start a search.'''
          self.viewPath = self.manager.Search(filter, self.path,
                                              timeout=100000)
          self.view = dbus.Interface(bus.get_object(self.manager.bus_name,
                                                    self.viewPath),
                                     'org._01.pim.contacts.ViewControl')

     def close(self):
          self.view.Close(timeout=100000)

     def getIDs(self, start, count):
          '''Return just the IDs for a range of contacts in the current view.'''
          return [isinstance(x, dict) and x['id'] or \
                       isinstance(x, tuple) and x[0] or \
                       x \
                       for x in self.contacts[start:start + count]]

     def countData(self, start, count):
          '''Number of contacts with data in the given range.'''
          total = 0
          for contact in self.contacts[start:start + count]:
               if isinstance(contact, dict):
                    total = total + 1
          return total

     def haveData(self, start, count = 1):
          '''True if all contacts in the range have data.'''
          return count == self.countData(start, count)

     def haveNoData(self, start, count = 1):
          '''True if all contacts in the range have no data.'''
          return 0 == self.countData(start, count)

     def processEvent(self, message, event):
          self.logging.log(message)
          self.events.append(event)

     @dbus.service.method(dbus_interface='org._01.pim.contacts.ViewAgent',
                          in_signature='oias', out_signature='')
     def ContactsModified(self, view, start, ids):
          count = len(ids)
          self.processEvent('contacts modified: %s, start %d, ids %s' % (view, start, ids),
                            ('modified', start, count))
          try:
               self.assertEqual(view, self.viewPath)
               self.assertGreaterEqual(start, 0)
               self.assertGreater(count, 0)
               self.assertLessEqual(start + count, len(self.contacts))
               # Overwrite valid data with just the (possibly modified) ID.
               self.contacts[start:start + count] = [str(x) for x in ids]
               self.logging.printf('contacts modified => %s', self.contacts)
          except:
               error = traceback.format_exc()
               self.logging.printf('contacts modified: error: %s' % error)
               self.errors.append(error)


     @dbus.service.method(dbus_interface='org._01.pim.contacts.ViewAgent',
                          in_signature='oias', out_signature='')
     def ContactsAdded(self, view, start, ids):
          count = len(ids)
          self.processEvent('contacts added: %s, start %d, ids %s' % (view, start, ids),
                            ('added', start, count))
          try:
               self.assertEqual(view, self.viewPath)
               self.assertGreaterEqual(start, 0)
               self.assertGreater(count, 0)
               self.assertLessEqual(start, len(self.contacts))
               self.contacts[start:start] = [str(x) for x in ids]
               self.logging.printf('contacts added => %s', self.contacts)
          except:
               error = traceback.format_exc()
               self.logging.printf('contacts added: error: %s' % error)
               self.errors.append(error)

     @dbus.service.method(dbus_interface='org._01.pim.contacts.ViewAgent',
                          in_signature='oias', out_signature='')
     def ContactsRemoved(self, view, start, ids):
          count = len(ids)
          self.processEvent('contacts removed: %s, start %d, ids %s' % (view, start, ids),
                            ('removed', start, count))
          try:
               self.assertEqual(view, self.viewPath)
               self.assertGreaterEqual(start, 0)
               self.assertGreater(count, 0)
               self.assertLessEqual(start + count, len(self.contacts))
               self.assertEqual(self.getIDs(start, count), ids)
               del self.contacts[start:start + count]
               self.logging.printf('contacts removed => %s', self.contacts)
          except:
               error = traceback.format_exc()
               self.logging.printf('contacts removed: error: %s' % error)
               self.errors.append(error)

     @dbus.service.method(dbus_interface='org._01.pim.contacts.ViewAgent',
                          in_signature='o', out_signature='')
     def Quiescent(self, view):
          self.quiescentCount = self.quiescentCount + 1
          self.processEvent('quiescent: %s' % view,
                            ('quiescent',))

     def read(self, start, count=1):
          '''Read the specified range of contact data.'''
          starttime = time.time()
          ids = []
          for index, entry in enumerate(self.contacts[start:start+count]):
               if isinstance(entry, str):
                    ids.append(entry)
                    self.contacts[start + index] = (entry, starttime)
          # Avoid composing too large requests because they make the
          # server unresponsive and trigger our Watchdog. Instead chop up
          # into pieces and ask for more once we get the response.
          def step(contacts, start):
               for index, contact in contacts:
                    if index >= 0:
                         self.contacts[index] = contact
               if start < len(ids):
                    end = min(start + 50, len(ids))
                    self.view.ReadContacts(ids[start:end],
                                           timeout=100000,
                                           reply_handler=lambda contacts: step(contacts, end),
                                           error_handler=lambda error: self.errors.append(x))
          step([], 0)

class Watchdog():
     '''Send D-Bus queries regularly to the daemon and measure response time.'''
     def __init__(self, test, manager, threshold=0.1, interval=0.2):
          self.test = test
          self.manager = manager
          self.started = None
          self.results = [] # tuples of start time + duration
          self.threshold = threshold
          self.interval = interval
          self.timeout = None

     def start(self):
          self.timeout = glib.Timeout(int(self.interval * 1000))
          self.timeout.set_callback(self._ping)
          self.timeout.attach(loop.get_context())

     def stop(self):
          if self.timeout:
               self.timeout.destroy()
          self.started = None

     def check(self):
          '''Assert that all queries were served quickly enough.'''
          tooslow = [x for x in self.results if x[1] > self.threshold]
          self.test.assertEqual([], tooslow)
          if self.started:
               self.test.assertLess(time.time() - self.started, self.threshold)

     def reset(self):
          self.results = []
          self.started = None

     def checkpoint(self, name):
          self.check()
          logging.printf('ping results for %s: %s', name, self.results)
          self.reset()

     def _ping(self):
          if not self.started:
               # Run with a long timeout. We want to know how long it
               # takes to reply, even if it is too long.
               self.started = time.time()
               self.manager.GetAllPeers(timeout=1000,
                                        reply_handler=lambda peers: self._done(self.started, self.results, None),
                                        error_handler=lambda error: self._done(self.started, self.results, error))
          return True

     def _done(self, started, results, error):
          '''Record result. Intentionally uses the results array from the time when the call started,
          to handle intermittent checkpoints.'''
          duration = time.time() - started
          if duration > self.threshold or error:
               logging.printf('ping failure: duration %fs, error %s', duration, error)
          if error:
               results.append((started, duration, error))
          else:
               results.append((started, duration))
          if self.started == started:
               self.started = None

class TestContacts(DBusUtil, unittest.TestCase):
    """Tests for org._01.pim.contacts API.

The tests use the system's EDS, which must be >= 3.6.
They create additional databases in EDS under the normal
location. This is necessary because the tests cannot
tell the EDS source registry daemon to run with a different
XDG root.
"""

    def setUp(self):
        self.cleanup = []
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
        removed = False
        if os.path.exists(self.sourcedir):
             for source in os.listdir(self.sourcedir):
                  if source.startswith(self.managerPrefix + self.uidPrefix):
                       os.unlink(os.path.join(self.sourcedir, source))
                       removed = True
        if removed:
            # Give EDS time to notice the removal.
            time.sleep(5)

    def tearDown(self):
         for x in self.cleanup:
              x()

    def setUpView(self, peers=['foo'], withSystemAddressBook=False, search=[], withLogging=True):
        '''Set up peers and create a view for them.'''
        # Ignore all currently existing EDS databases.
        self.sources = self.currentSources()
        self.expected = self.sources.copy()
        self.peers = {}

        # dummy peer directory
        self.contacts = os.path.abspath(os.path.join(xdg_root, 'contacts'))
        os.makedirs(self.contacts)

        # add peers
        self.uid = None
        self.uids = []
        for peer in peers:
             uid = self.uidPrefix + peer
             if self.uid == None:
                  # Remember first uid for tests which only use one.
                  self.uid = uid
             self.uids.append(uid)
             self.peers[uid] = {'protocol': 'PBAP',
                                'address': 'xxx'}
             self.manager.SetPeer(uid,
                                  self.peers[uid],
                                  timeout=self.timeout)
             self.expected.add(self.managerPrefix + uid)
             self.assertEqual(self.peers, self.manager.GetAllPeers(timeout=self.timeout))
             self.assertEqual(self.expected, self.currentSources())

             # Delete local data in the cache.
             logging.log('deleting all items of ' + uid)
             self.runCmdline(['--delete-items', '@' + self.managerPrefix + uid, 'local', '*'])

        # Limit active databases to the one we just created.
        addressbooks = ['peer-' + uid for uid in self.uids]
        if withSystemAddressBook:
             addressbooks.append('')
             # Delete content of system address book. The check at the start of
             # the script ensures that this is not the real one of the user.
             logging.log('deleting all items of system address book')
             self.runCmdline(['--delete-items', 'backend=evolution-contacts', '--luids', '*'])

        self.manager.SetActiveAddressBooks(addressbooks, timeout=self.timeout)
        self.assertEqual(addressbooks, self.manager.GetActiveAddressBooks(timeout=self.timeout),
                         sortLists=True)

        # Start view.
        self.view = ContactsView(self.manager)
        if not withLogging:
             self.view.processEvent = lambda message, event: True
             self.view.logging = NullLogging()

        # Optional: search and wait for it to be stable.
        if search != None:
             self.view.search(search)
             self.runUntil('empty view',
                           check=lambda: self.assertEqual([], self.view.errors),
                           until=lambda: self.view.quiescentCount > 0)

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
        contacts = open(filename, 'r').read()
        # Ignore one empty vcard because the Nokia N97 always sends such a vcard,
        # despite having deleted everything via SyncML. The ordering of properties
        # comes from our own vcard profile (UID/PRODID/REV first).
        contacts = re.sub(r'''BEGIN:VCARD\r?
VERSION:3.0\r?
((UID|PRODID|REV):.*\r?
)*N:;;;;\r?
FN:\r?
END:VCARD(\r|\n)*''',
                          '',
                          contacts,
                          1)
        open(filename, 'w').write(contacts)

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
            # Also removes X-EVOLUTION-FILE-AS.
            env['CLIENT_TEST_STRIP_PROPERTIES'] = 'X-[-_a-zA-Z0-9]*'
        sub = subprocess.Popen(['synccompare', expected, real],
                               env=env,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
        stdout, stderr = sub.communicate()
        self.assertEqual(0, sub.returncode,
                         msg="env 'CLIENT_TEST_STRIP_PROPERTIES=%s' synccompare %s %s\n%s" %
                         (env.get('CLIENT_TEST_STRIP_PROPERTIES', ''),
                          expected,
                          real,
                          stdout))

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
        # No errors must be logged. During testRead, libphonenumber used to print
        #   [ERROR] Number too short to be viable: 8
        #   [ERROR] The string supplied did not seem to be a phone number.
        # to stdout until we reduced the log level.
        #
        # We check both D-Bus messages (which did not contain that
        # text, but some other error messages) and the servers stdout.
        self.runTestDBusCheck = lambda test, log: test.assertNotIn('ERROR', log)
        self.runTestOutputCheck = self.runTestDBusCheck

        # Runtime varies a lot when using valgrind, because
        # of the need to check an additional process. Allow
        # a lot more time when running under valgrind.
        self.runTest(result, own_xdg=True, own_home=True,
                     defTimeout=usingValgrind() and 600 or 20)

    def currentSources(self):
        '''returns current set of EDS sources as set of UIDs, without the .source suffix'''
        return set([os.path.splitext(x)[0] for x in (os.path.exists(self.sourcedir) and os.listdir(self.sourcedir) or [])])

    @property("snapshot", "simple-sort")
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

        # PIM Manager must not allow overwriting an existing config.
        # Uses the new name for SetPeer().
        with self.assertRaisesRegexp(dbus.DBusException,
                                     'org._01.pim.contacts.Manager.AlreadyExists: uid ' + uid + ' is already in use') as cm:
             self.manager.CreatePeer(uid,
                                     peers[uid],
                                     timeout=self.timeout)
        self.assertEqual('org._01.pim.contacts.Manager.AlreadyExists', cm.exception.get_dbus_name())

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

    @timeout(300)
    @property("snapshot", "simple-sort")
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

        # Clear data in the phone.
        self.configurePhone(phone, uid, contacts)
        self.syncPhone(phone, uid)

        def listall(dirs, exclude=[]):
             result = {}
             def append(dirname, entry):
                  fullname = os.path.join(dirname, entry)
                  for pattern in exclude:
                       if re.match(pattern, fullname):
                            return
                  result[fullname] = os.stat(fullname).st_mtime
             for dir in dirs:
                  for dirname, dirnames, filenames in os.walk(dir):
                       for subdirname in dirnames:
                            append(dirname, subdirname)
                       for filename in filenames:
                            append(dirname, filename)
             return result

        def listsyncevo(exclude=[]):
             '''find all files owned by SyncEvolution, excluding the logs for syncing with a real phone'''
             return listall([os.path.join(xdg_root, x) for x in ['config/syncevolution', 'cache/syncevolution']],
                            exclude + ['.*/phone@.*', '.*/peers/phone.*'])

        # Remember current list of files and modification time stamp.
        files = listsyncevo()

        # Remove all data locally.
        self.manager.SyncPeer(uid,
                              timeout=self.timeout)
        # TODO: check that syncPhone() really used PBAP - but how?

        # Should not have written files, except for specific exceptions:
        exclude = []
        # - directories in which we need to create files
        exclude.extend([xdg_root + '/cache/syncevolution/eds@[^/]*$',
                        xdg_root + '/cache/syncevolution/target_.config@[^/]*$'])
        # - some files which are allowed to be written
        exclude.extend([xdg_root + '/cache/syncevolution/[^/]*/(status.ini|syncevolution-log.html)$'])
        # - synthesis client files (should not be written at all, but that's harder - redirect into cache for now)
        exclude.extend([xdg_root + '/cache/syncevolution/[^/]*/synthesis(/|$)'])

        # Now compare files and their modification time stamp.
        self.assertEqual(files, listsyncevo(exclude=exclude))

        # Export data from local database into a file via the --export
        # operation in the syncevo-dbus-server. Depends on (and tests)
        # that the SyncEvolution configuration was created as
        # expected. It does not actually check that EDS is used - the
        # test would also pass for any other storage.
        export = os.path.join(xdg_root, 'local.vcf')
        self.exportCache(uid, export)

        # Must be empty now.
        self.compareDBs(contacts, export)

        # Add a contact.
        john = '''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
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
END:VCARD'''

        # Test all fields that PIM Manager supports in its D-Bus API
        # when using the file backend, because (in contrast to a phone)
        # we know that it supports them, too.
        if not phone:
             john = r'''BEGIN:VCARD
VERSION:3.0
URL:http://john.doe.com
TITLE:Senior Tester
ORG:Test Inc.;Testing;test#1
ROLE:professional test case
X-EVOLUTION-MANAGER:John Doe Senior
X-EVOLUTION-ASSISTANT:John Doe Junior
NICKNAME:user1
BDAY:2006-01-08
X-EVOLUTION-ANNIVERSARY:2006-01-09
X-EVOLUTION-SPOUSE:Joan Doe
NOTE:This is a test case which uses almost all Evolution fields.
FN:John Doe
N:Doe;John;;;
X-EVOLUTION-FILE-AS:Doe\, John
CATEGORIES:TEST
X-EVOLUTION-BLOG-URL:web log
GEO:30.12;-130.34
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
END:VCARD
'''

        item = os.path.join(contacts, 'john.vcf')
        output = open(item, "w")
        output.write(john)
        output.close()
        self.syncPhone(phone, uid)
        self.manager.SyncPeer(uid,
                              timeout=self.timeout)

        # Also exclude modified database files.
        self.assertEqual(files, listsyncevo(exclude=exclude))

        self.exportCache(uid, export)
        self.compareDBs(contacts, export)

        # Keep one session directory in a non-default location.
        logdir = xdg_root + '/pim-logdir'
        peers[uid]['logdir'] = logdir
        peers[uid]['maxsessions'] = '1'
        self.manager.SetPeer(uid,
                             peers[uid],
                             timeout=self.timeout)
        files = listsyncevo(exclude=exclude)
        self.manager.SyncPeer(uid,
                              timeout=self.timeout)
        exclude.append(logdir + '(/$)')
        self.assertEqual(files, listsyncevo(exclude=exclude))

        self.assertEqual(2, len(os.listdir(logdir)))

        # At most one!
        self.manager.SyncPeer(uid,
                              timeout=self.timeout)
        exclude.append(logdir + '(/$)')
        self.assertEqual(files, listsyncevo(exclude=exclude))
        self.assertEqual(2, len(os.listdir(logdir)))

        # And now prune none.
        peers[uid]['maxsessions'] = '0'
        self.manager.SetPeer(uid,
                             peers[uid],
                             timeout=self.timeout)
        files = listsyncevo(exclude=exclude)
        self.manager.SyncPeer(uid,
                              timeout=self.timeout)
        exclude.append(logdir + '(/$)')
        self.assertEqual(files, listsyncevo(exclude=exclude))
        self.assertEqual(4, len(os.listdir(logdir)))

        # Test invalid maxsession values.
        with self.assertRaisesRegexp(dbus.DBusException,
                                     "negative 'maxsessions' not allowed: -1"):
             self.manager.SetPeer(uid,
                                  {'protocol': 'PBAP',
                                   'address': 'foo',
                                   'maxsessions': '-1'},
                                  timeout=self.timeout)
        self.assertEqual(files, listsyncevo(exclude=exclude))

        with self.assertRaisesRegexp(dbus.DBusException,
                                     'bad lexical cast: source type value could not be interpreted as target'):
             self.manager.SetPeer(uid,
                                  {'protocol': 'PBAP',
                                   'address': 'foo',
                                   'maxsessions': '1000000000000000000000000000000000000000000000'},
                                  timeout=self.timeout)
        self.assertEqual(files, listsyncevo(exclude=exclude))

    @timeout(100)
    @property("ENV", "SYNCEVOLUTION_SYNC_DELAY=200")
    @property("snapshot", "simple-sort")
    def testSyncAbort(self):
        '''TestContacts.testSyncAbort - test StopSync()'''
        self.setUpServer()
        sources = self.currentSources()
        expected = sources.copy()
        peers = {}

        # Disable the default checking because
        # we trigger one ERROR message.
        self.runTestDBusCheck = None
        self.runTestOutputCheck = None

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
                                   error_handler=lambda x: result(0, x),
                                   timeout=self.timeout)
             self.manager.SyncPeer(uid,
                                   reply_handler=lambda: result(1, True),
                                   error_handler=lambda x: result(1, x),
                                   timeout=self.timeout)
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
    @property("snapshot", "simple-sort")
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
        self.assertEqual([('added', 0, 1), ('quiescent',)], self.view.events)
        self.view.events = []

        # Read contact.
        logging.log('reading contact')
        self.view.read(0)
        self.runUntil('contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
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
                      until=lambda: self.view.haveNoData(0))
        self.assertEqual([('modified', 0, 1), ('quiescent',)], self.view.events)
        self.view.events = []

        # Remove contact.
        self.runCmdline(['--delete-items', '@' + self.managerPrefix + self.uid, 'local', '*'])
        self.runUntil('view without contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 0)
        self.assertEqual([('removed', 0, 1), ('quiescent',)], self.view.events)
        self.view.events = []


    @timeout(60)
    @property("snapshot", "simple-sort")
    def testViewSorting(self):
        '''TestContacts.testViewSorting - check that sorting works when changing contacts'''
        self.setUpView()

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list. The default sort method is active, which is not
        # locale-aware. Therefore the test data only uses ASCII characters.
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
                      until=lambda: self.view.haveData(0))
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
                      until=lambda: self.view.haveNoData(2))
        self.assertEqual([('modified', 2, 1), ('quiescent',)], self.view.events)
        self.view.events = []
        logging.log('reading updated contact')
        self.view.read(2, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(2))
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
                      until=lambda: self.view.haveNoData(2))
        self.assertEqual([('modified', 2, 1), ('quiescent',)], self.view.events)
        self.view.events = []
        logging.log('reading updated contact')
        self.view.read(2, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(2))
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
                      until=lambda: self.view.haveNoData(1))
        self.assertEqual([('modified', 1, 1), ('quiescent',)], self.view.events)
        self.view.events = []
        logging.log('reading updated contact')
        self.view.read(1, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(1))
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
                      until=lambda: self.view.haveNoData(0))
        self.assertEqual([('modified', 0, 1), ('quiescent',)], self.view.events)
        self.view.events = []
        logging.log('reading updated contact')
        self.view.read(0, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
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
                      until=lambda: self.view.haveNoData(0))
        self.assertEqual([('removed', 2, 1),
                          ('added', 0, 1),
                          ('quiescent',)], self.view.events)
        self.view.events = []
        logging.log('reading updated contact')
        self.view.read(0, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
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
                      until=lambda: self.view.haveNoData(2))
        self.assertEqual([('removed', 0, 1),
                          ('added', 2, 1),
                          ('quiescent',)], self.view.events)
        self.view.events = []
        logging.log('reading updated contact')
        self.view.read(2, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(2))
        self.assertEqual('Charly Xing', self.view.contacts[0]['full-name'])
        self.assertEqual('Benjamin Yeah', self.view.contacts[1]['full-name'])
        self.assertEqual('Abraham Zoo', self.view.contacts[2]['full-name'])

    @timeout(60)
    # boost::locale checks LC_TYPE first, then LC_ALL, LANG. Set all, just
    # to be sure.
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    @property("snapshot", "first-last-sort")
    def testSortOrder(self):
        '''TestContacts.testSortOrder - check that sorting works when changing the comparison'''
        self.setUpView()

        # Locale-aware "first/last" sorting from "first-last-sort" config.
        self.assertEqual("first/last", self.manager.GetSortOrder(timeout=self.timeout))

        # Expect an error, no change to sort order.
        with self.assertRaisesRegexp(dbus.DBusException,
                                     'sort order.*not supported'):
             self.manager.SetSortOrder('no-such-order',
                                       timeout=self.timeout)
        self.assertEqual("first/last", self.manager.GetSortOrder(timeout=self.timeout))

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Äbraham
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Bénjamin
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
FN:Chàrly Xing
N:Xing;Chàrly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
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
                      until=lambda: self.view.haveData(0))
        self.assertEqual(u'Äbraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Bénjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Chàrly', self.view.contacts[2]['structured-name']['given'])

        # Invert sort order.
        self.manager.SetSortOrder("last/first",
                                  timeout=self.timeout)

        # Check that order was adapted and stored permanently.
        self.assertEqual("last/first", self.manager.GetSortOrder(timeout=self.timeout))
        self.assertIn("sort = last/first\n",
                      open(os.path.join(xdg_root, "config", "syncevolution", "pim-manager.ini"),
                           "r").readlines())

        # Contact in the middle may or may not become invalidated.
        self.runUntil('reordered',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 3 and \
                           self.view.haveNoData(0) and \
                           self.view.haveNoData(2))
        # Read contacts.
        logging.log('reading contacts')
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
        self.assertEqual('Xing', self.view.contacts[0]['structured-name']['family'])
        self.assertEqual('Yeah', self.view.contacts[1]['structured-name']['family'])
        self.assertEqual('Zoo', self.view.contacts[2]['structured-name']['family'])

        # Sort by FN or <first last> as fallback.
        self.manager.SetSortOrder("fullname",
                                  timeout=self.timeout)
        self.runUntil('reordered',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 3 and \
                           self.view.haveNoData(0) and \
                           self.view.haveNoData(2))
        logging.log('reading contacts')
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
        self.assertEqual(u'Äbraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Bénjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Chàrly', self.view.contacts[2]['structured-name']['given'])

    @timeout(60)
    @property("snapshot", "simple-sort")
    def testRead(self):
        '''TestContacts.testRead - check that folks and PIM Manager deliver EDS data correctly'''
        self.setUpView()

        # Insert new contacts.
        #
        # Not all of the vCard properties need to be available via PIM Manager.
        testcases = [r'''BEGIN:VCARD
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
CATEGORIES:TEST1,TEST2
GEO:30.12;-130.34
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
                      until=lambda: self.view.haveData(0))
        contact = copy.deepcopy(self.view.contacts[0])
        # Simplify the photo URI, if there was one. Avoid any assumptions
        # about the filename, except that it is a file:/// uri.
        if contact.has_key('photo'):
             contact['photo'] = re.sub('^file:///.*', 'file:///<stripped>', contact['photo'])
        if contact.has_key('id'):
             contact['id'] = '<stripped>'
        self.assertEqual({'full-name': 'John Doe',
                          'groups': ['TEST1', 'TEST2'],
                          'location': (30.12, -130.34),
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
                          'id': '<stripped>',
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

    @timeout(60)
    @property("snapshot", "simple-sort")
    def testStop(self):
        '''TestContacts.testStop - stop a started server'''
        # Auto-start.
        self.assertEqual(False, self.manager.IsRunning(timeout=self.timeout))
        self.setUpView(peers=['foo'])
        self.assertEqual(True, self.manager.IsRunning(timeout=self.timeout))

        # Must not stop now.
        self.manager.Stop(timeout=self.timeout)
        self.view.read(0, 0)

        # It may stop after closing the view.
        self.view.view.Close(timeout=self.timeout)
        self.manager.Stop(timeout=self.timeout)
        self.assertEqual(False, self.manager.IsRunning(timeout=self.timeout))

    @timeout(60)
    @property("snapshot", "simple-sort")
    def testEmpty(self):
        '''TestContacts.testEmpty - start with empty view without databases'''
        self.setUpView(peers=[])
        # Let it run for a bit longer, to catch further unintentional changes.
        now = time.time()
        self.runUntil('delay',
                      check=lambda: (self.assertEqual([], self.view.errors),
                                     self.assertEqual([], self.view.contacts)),
                      until=lambda: time.time() - now > 10)

    @timeout(60)
    @property("snapshot", "simple-sort")
    def testMerge(self):
        '''TestContacts.testMerge - merge identical contacts from two stores'''
        self.setUpView(peers=['foo', 'bar'])

        # folks merges this because a) X-JABBER (always) b) EMAIL (only
        # with patch for https://bugzilla.gnome.org/show_bug.cgi?id=685401).
        john = '''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
TEL:1234-5678
EMAIL:john.doe@example.com
URL:http://john.doe.com
X-JABBER:jd@example.com
END:VCARD'''
        item = os.path.join(self.contacts, 'john.vcf')
        output = open(item, "w")
        output.write(john)
        output.close()

        luids = {}
        for uid in self.uids:
             logging.log('inserting John into ' + uid)
             out, err, returncode = self.runCmdline(['--import', item, '@' + self.managerPrefix + uid, 'local'])
             luids[uid] = self.extractLUIDs(out)

        # Run until the view has adapted.
        self.runUntil('view with contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) != 0)
        self.assertEqual(1, len(self.view.contacts))

        # Let it run for a bit longer, to catch further unintentional changes
        # and ensure that changes from both stores where processed.
        now = time.time()
        self.runUntil('delay',
                      check=lambda: (self.assertEqual([], self.view.errors),
                                     self.assertEqual(1, len(self.view.contacts))),
                      until=lambda: time.time() - now > 10)

        # Read contact.
        logging.log('reading contact')
        self.view.read(0, 1)
        self.runUntil('contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
        contact = copy.deepcopy(self.view.contacts[0])
        self.assertEqual({'full-name': 'John Doe',
                          'structured-name': {'given': 'John', 'family': 'Doe'},
                          'emails': [('john.doe@example.com', [])],
                          'phones': [('1234-5678', [])],
                          'urls': [('http://john.doe.com', ['x-home-page'])],
                          'id': contact.get('id', '<???>'),
                          'source': [
                       ('test-dbus-bar', luids[self.uidPrefix + 'bar'][0]),
                       ('test-dbus-foo', luids[self.uidPrefix + 'foo'][0])

                       ],
                          },
                         # Order of list entries in the result is not specified.
                         # Must sort before comparing.
                         contact,
                         sortLists=True)

    @timeout(int(os.environ.get('TESTPIM_TEST_ACTIVE_NUM', 10)) * (usingValgrind() and 15 or 6))
    @property("snapshot", "db-active")
    def testActive(self):
        '''TestContacts.testActive - reconfigure active address books several times'''

        contactsPerPeer = int(os.environ.get('TESTPIM_TEST_ACTIVE_NUM', 10))

        self.assertEqual(['', 'peer-test-dbus-a', 'peer-test-dbus-c'],
                         self.manager.GetActiveAddressBooks(timeout=self.timeout),
                         sortLists=True)

        withLogging = (contactsPerPeer <= 10)
        checkPerformance = os.environ.get('TESTPIM_TEST_ACTIVE_RESPONSE', None)
        threshold = 0
        if checkPerformance:
             threshold = float(checkPerformance)

        # When starting the search right away and then add more
        # contact data later, we test the situation where new data
        # comes in because of a sync.
        #
        # When adding data first and then searching, we cover the
        # startup situation with already populated caches.
        #
        # Both are relevant scenarios. The first one stresses folks
        # more, because SyncEvolution adds contacts one at a time,
        # which then leads to many D-Bus messages containing a single
        # "contact added" notification that need to be processed by
        # folks. Testing this is the default.
        if os.environ.get('TESTPIM_TEST_ACTIVE_LOAD', False):
             # Delay starting the PIM Manager until data is ready to be read.
             # More realistic that way; otherwise folks must process new
             # contacts one-by-one.
             search=None
        else:
             # Start folks right away.
             search=''

        peers = ['a', 'b', 'c']
        self.setUpView(peers=peers, withSystemAddressBook=True,
                       search=search,
                       withLogging=withLogging)
        active = [''] + peers

        # Check that active databases were adapted and stored permanently.
        self.assertEqual(['', 'peer-test-dbus-a', 'peer-test-dbus-b', 'peer-test-dbus-c'],
                         self.manager.GetActiveAddressBooks(timeout=self.timeout),
                         sortLists=True)
        # Order mirrors the one of SetActiveAddressBooks() in setUpView(),
        # assuming that the PIM Manager preserves that order (not really guaranteed
        # by the API, but is how it is implemented).
        self.assertIn("active = pim-manager-test-dbus-a pim-manager-test-dbus-b pim-manager-test-dbus-c system-address-book\n",
                      open(os.path.join(xdg_root, "config", "syncevolution", "pim-manager.ini"),
                           "r").readlines())

        for peer in active:
             for index in range(0, contactsPerPeer):
                  item = os.path.join(self.contacts, 'john%d.vcf' % index)
                  output = open(item, "w")
                  output.write('''BEGIN:VCARD
VERSION:3.0
FN:John_%(peer)s%(index)03d Doe
N:Doe;John_%(peer)s%(index)03d
END:VCARD''' % {'peer': peer, 'index': index})
                  output.close()

             uid = self.uidPrefix + peer
             logging.log('inserting data into ' + uid)
             if peer != '':
                  out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + uid, 'local'])
             else:
                  out, err, returncode = self.runCmdline(['--import', self.contacts, 'database=', 'backend=evolution-contacts'])

        # Ping server regularly and check that it remains responsive.
        # Depends on processing all D-Bus replies with minimum
        # delay, because delays caused by us would lead to false negatives.
        w = Watchdog(self, self.manager, threshold=threshold)
        if checkPerformance:
             w.start()
             self.cleanup.append(w.stop)

        # Start the view if not done yet and run until the view has adapted.
        if search == None:
             self.view.search('')
        self.runUntil('view with contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == contactsPerPeer * len(active),
                      may_block=checkPerformance)
        w.checkpoint('full view')

        def checkContacts():
             contacts = copy.deepcopy(self.view.contacts)
             for contact in contacts:
                  del contact['id']
                  del contact['source']
             expected = [{'full-name': first + ' Doe',
                                'structured-name': {'given': first, 'family': 'Doe'}} for \
                                    first in ['John_%(peer)s%(index)03d' % {'peer': peer,
                                                                            'index': index} \
                                                   for peer in active \
                                                   for index in range(0, contactsPerPeer)] ]
             return (expected, contacts)

        def assertHaveContacts():
             expected, contacts = checkContacts()
             self.assertEqual(expected, contacts)

        # Read contacts.
        logging.log('reading contacts')
        self.view.read(0, contactsPerPeer * len(active))
        self.runUntil('contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, contactsPerPeer * len(active)),
                      may_block=not withLogging)
        assertHaveContacts()
        w.checkpoint('contact data')

        def haveExpectedView():
             current = time.time()
             logNow = withLogging or current - haveExpectedView.last > 1
             if len(self.view.contacts) == contactsPerPeer * len(active):
                  if self.view.haveData(0, contactsPerPeer * len(active)):
                       expected, contacts = checkContacts() # TODO: avoid calling this
                       if expected == contacts:
                            logging.log('got expected data')
                            return True
                       else:
                            if withLogging:
                                 logging.printf('data mismatch, keep waiting; currently have: %s', contacts)
                            elif logNow:
                                 logging.printf('data mismatch, keep waiting')
                  else:
                       self.view.read(0, len(self.view.contacts))
                       if logNow:
                            logging.log('still waiting for all data')
             elif logNow:
                  logging.printf('wrong contact count, keep waiting: have %d, want %d',
                                 len(self.view.contacts),
                                 contactsPerPeer * len(active))
             haveExpectedView.last = current
             return False
        haveExpectedView.last = time.time()

        # Now test all subsets until we are back at 'all active'.
        current = ['', 'a', 'b', 'c']
        for active in [filter(lambda x: x != None,
                              [s, a, b, c])
                       for s in [None, '']
                       for a in [None, 'a']
                       for b in [None, 'b']
                       for c in [None, 'c']]:
             logging.printf('changing address books %s -> %s', current, active)
             self.manager.SetActiveAddressBooks([x != '' and 'peer-' + self.uidPrefix + x or x for x in active],
                                                timeout=self.timeout)
             self.runUntil('contacts %s' % str(active),
                           check=lambda: self.assertEqual([], self.view.errors),
                           until=haveExpectedView,
                           may_block=not withLogging)
             w.checkpoint('%s -> %s' % (current, active))
             current = active


    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFilterExisting(self):
        '''TestContacts.testFilterExisting - check that filtering works when applied to static contacts'''
        self.setUpView()

        # Cannot refine full view.
        with self.assertRaisesRegexp(dbus.DBusException,
                                     r'.*: refining the search not supported by this view$'):
             self.view.view.RefineSearch([['any-contains', 'foo']],
                                         timeout=self.timeout)

        # Override default sorting.
        self.assertEqual("last/first", self.manager.GetSortOrder(timeout=self.timeout))
        self.manager.SetSortOrder("first/last",
                                  timeout=self.timeout)

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:Ace
TEL:1234
TEL:56/78
TEL:+1-800-FOOBAR
TEL:089/7888-99
EMAIL:az@example.com
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
TEL:+1-89-7888-99
END:VCARD''',

# Chárleß has chárless as representation after folding the case.
# This is different from lower case.
# See http://www.boost.org/doc/libs/1_51_0/libs/locale/doc/html/glossary.html#term_case_folding
u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
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
                      until=lambda: self.view.haveData(0, 3))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Find Charly by his FN (case insensitive by default).
        view = ContactsView(self.manager)
        view.search([['any-contains', 'chÁrles']])
        self.runUntil('charles search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('charles',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])

        # Cannot expand view.
        with self.assertRaisesRegexp(dbus.DBusException,
                                     r'.*: New filter is empty. It must be more restrictive than the old filter\.$'):
             view.view.RefineSearch([],
                                    timeout=self.timeout)

        # Find Charly by his FN (case insensitive explicitly).
        view = ContactsView(self.manager)
        view.search([['any-contains', 'chÁrless', 'case-insensitive']])
        self.runUntil('charles search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('charles',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])

        # Find Charly by his FN (case sensitive explicitly).
        view = ContactsView(self.manager)
        view.search([['any-contains', 'Chárleß', 'case-sensitive']])
        self.runUntil('charles search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('charles',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])

        # Do not find Charly by his FN (case sensitive explicitly).
        view = ContactsView(self.manager)
        view.search([['any-contains', 'charles', 'case-sensitive']])
        self.runUntil('charles search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

        # Find Abraham and Benjamin.
        view = ContactsView(self.manager)
        view.search([['any-contains', 'am']])
        self.runUntil('"am" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(2, len(view.contacts))
        view.read(0, 2)
        self.runUntil('two contacts',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', view.contacts[1]['structured-name']['given'])

        # Refine search without actually changing the result.
        view.quiescentCount = 0
        view.view.RefineSearch([['any-contains', 'am']],
                               timeout=self.timeout)
        self.runUntil('end of search refinement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', view.contacts[1]['structured-name']['given'])

        # Restrict search to Benjamin. The result is a view
        # which has different indices than the full view.
        view.quiescentCount = 0
        view.view.RefineSearch([['any-contains', 'Benjamin']],
                               timeout=self.timeout)
        self.runUntil('end of search refinement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        self.assertEqual(u'Benjamin', view.contacts[0]['structured-name']['given'])

        # Refine again, without changes.
        view.quiescentCount = 0
        view.view.RefineSearch([['any-contains', 'Benjamin']],
                               timeout=self.timeout)
        self.runUntil('end of search refinement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        self.assertEqual(u'Benjamin', view.contacts[0]['structured-name']['given'])

        # Refine to empty view.
        view.quiescentCount = 0
        view.view.RefineSearch([['any-contains', 'XXXBenjamin']],
                               timeout=self.timeout)
        self.runUntil('end of search refinement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

        # Find Abraham by his nickname.
        view = ContactsView(self.manager)
        view.search([['any-contains', 'ace']])
        self.runUntil('"ace" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('two contacts',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham by his email.
        view = ContactsView(self.manager)
        view.search([['any-contains', 'az@']])
        self.runUntil('"az@" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('two contacts',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham by his 1234 telephone number.
        view = ContactsView(self.manager)
        view.search([['any-contains', '1234']])
        self.runUntil('"1234" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('1234 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham by his 1234 telephone number, as sub-string.
        view = ContactsView(self.manager)
        view.search([['any-contains', '23']])
        self.runUntil('"23" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('23 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham by his 1234 telephone number, ignoring
        # formatting.
        view = ContactsView(self.manager)
        view.search([['any-contains', '12/34']])
        self.runUntil('"12/34" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('12/34 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham by his 56/78 telephone number, ignoring
        # slash in contact.
        view = ContactsView(self.manager)
        view.search([['any-contains', '5678']])
        self.runUntil('"5678" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('5678 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham via the +1-800-FOOBAR vanity number.
        view = ContactsView(self.manager)
        view.search([['any-contains', '+1-800-foobar']])
        self.runUntil('"+1-800-foobar" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('+1-800-foobar data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham via the +1-800-FOOBAR vanity number, with digits
        # instead of alpha characters.
        view = ContactsView(self.manager)
        view.search([['any-contains', '366227']])
        self.runUntil('"366227" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('366227 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham via caller ID for +1-800-FOOBAR.
        view = ContactsView(self.manager)
        view.search([['phone', '+1800366227']])
        self.runUntil('"+1800366227" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('+1800366227 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham via caller ID for 089/7888-99 (country is Germany).
        view = ContactsView(self.manager)
        view.search([['phone', '+49897888']])
        self.runUntil('"+49897888" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('+49897888 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham via 089/7888-99 (not a full caller ID, but at least a valid phone number).
        view = ContactsView(self.manager)
        view.search([['phone', '0897888']])
        self.runUntil('"0897888" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('0897888 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Don't find anyone.
        view = ContactsView(self.manager)
        view.search([['phone', '+49897888000']])
        self.runUntil('"+49897888000" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFilterQuiescence(self):
        '''TestContacts.testFilterQuiescence - check that starting server via filter leads to quiescence signal'''
        self.setUpView(peers=[], withSystemAddressBook=True, search=[('any-contains', 'foo')])
        self.assertEqual(1, self.view.quiescentCount)

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFullQuiescence(self):
        '''TestContacts.testFullQuiescence - check that starting server via filter leads to quiescence signal'''
        self.setUpView(peers=[], withSystemAddressBook=True, search=[])
        self.assertEqual(1, self.view.quiescentCount)

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFilterLive(self):
        '''TestContacts.testFilterLive - check that filtering works while adding contacts'''
        self.setUpView()

        self.manager.SetSortOrder("first/last",
                                  timeout=self.timeout)

        # Find Charly by his FN (case insensitive by default).
        view = ContactsView(self.manager)
        view.search([['any-contains', 'chÁrles']])
        self.runUntil('charles search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:Ace
TEL:1234
EMAIL:az@example.com
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
END:VCARD''',

# Chárleß has chárless as representation after folding the case.
# This is different from lower case.
# See http://www.boost.org/doc/libs/1_51_0/libs/locale/doc/html/glossary.html#term_case_folding
u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
        # Relies on importing contacts sorted ascending by file name.
        luids = self.extractLUIDs(out)
        logging.printf('created contacts with luids: %s' % luids)

        # Run until the view has adapted.
        self.runUntil('view with one contact',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: len(view.contacts) > 0)
        # Don't wait for more contacts here. They shouldn't come, and if
        # they do, we'll notice it below.
        self.assertEqual(1, len(view.contacts))

        # Search for telephone number.
        phone = ContactsView(self.manager)
        phone.search([['phone', '1234']])
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], phone.errors),
                      until=lambda: phone.quiescentCount > 0)
        self.assertEqual(1, len(phone.contacts))

        # Read contacts.
        logging.log('reading contacts')
        view.read(0, 1)
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: view.haveData(0) and \
                           self.view.haveData(0, 3))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Unmatched contact remains unmatched.
        # Modified phone number no longer matched.
        item = os.path.join(self.contacts, 'contact%d.vcf' % 0)
        output = codecs.open(item, "w", "utf-8")
        output.write(u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:King
TEL:123
EMAIL:az@example.com
END:VCARD''')
        output.close()
        logging.log('change nick of Abraham')
        out, err, returncode = self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('Abraham nickname changed',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual(1, len(view.contacts)),
                                     self.assertIsInstance(view.contacts[0], dict),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveNoData(0))
        self.view.read(0, 1)
        self.runUntil('Abraham nickname read',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual(1, len(view.contacts)),
                                     self.assertIsInstance(view.contacts[0], dict),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveData(0))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])

        # No longer part of the telephone search view.
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], phone.errors),
                      until=lambda: len(phone.contacts) == 0)
        phone.close()

        # Matched contact remains matched.
        item = os.path.join(self.contacts, 'contact%d.vcf' % 2)
        output = codecs.open(item, "w", "utf-8")
        output.write(u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
NICKNAME:Angel
N:Xing;Charly
END:VCARD''')
        output.close()
        logging.log('change nick of Charly')
        out, err, returncode = self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[2]])
        self.runUntil('Charly nickname changed',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual(1, len(view.contacts)),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveNoData(2) and \
                           view.haveNoData(0))
        view.read(0, 1)
        self.view.read(2, 1)
        self.runUntil('Charly nickname read',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual(1, len(view.contacts)),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveData(2) and \
                           view.haveData(0))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Unmatched contact gets matched.
        item = os.path.join(self.contacts, 'contact%d.vcf' % 0)
        output = codecs.open(item, "w", "utf-8")
        output.write(u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:Chárleß alter ego
TEL:1234
EMAIL:az@example.com
END:VCARD''')
        output.close()
        logging.log('change nick of Abraham, II')
        out, err, returncode = self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('Abraham nickname changed',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertLess(0, len(view.contacts)),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveNoData(0) and \
                           len(view.contacts) == 2)
        self.assertNotIsInstance(view.contacts[0], dict)
        self.assertIsInstance(view.contacts[1], dict)
        view.read(0, 1)
        self.view.read(0, 1)
        self.runUntil('Abraham nickname read, II',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual(2, len(view.contacts)),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveData(0) and \
                           view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Charly', view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])

        # Invert sort order.
        self.manager.SetSortOrder("last/first",
                                  timeout=self.timeout)
        self.runUntil('reordering',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveNoData(0) and \
                           self.view.haveNoData(2) and \
                           view.haveNoData(0, 2))
        view.read(0, 2)
        self.view.read(0, 3)
        self.runUntil('read reordered contacts',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveData(0, 3) and \
                           view.haveData(0, 2))
        self.assertEqual(2, len(view.contacts))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', view.contacts[1]['structured-name']['given'])
        self.assertEqual(3, len(self.view.contacts))
        self.assertEqual(u'Charly', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[2]['structured-name']['given'])

        # And back again.
        self.manager.SetSortOrder("first/last",
                                  timeout=self.timeout)
        self.runUntil('reordering, II',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveNoData(0) and \
                           self.view.haveNoData(2) and \
                           view.haveNoData(0, 2))
        view.read(0, 2)
        self.view.read(0, 3)
        self.runUntil('read reordered contacts, II',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveData(0, 3) and \
                           view.haveData(0, 2))
        self.assertEqual(2, len(view.contacts))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Charly', view.contacts[1]['structured-name']['given'])
        self.assertEqual(3, len(self.view.contacts))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Matched contact gets unmatched.
        item = os.path.join(self.contacts, 'contact%d.vcf' % 0)
        output = codecs.open(item, "w", "utf-8")
        output.write(u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:None
TEL:1234
EMAIL:az@example.com
END:VCARD''')
        output.close()
        logging.log('change nick of Abraham, II')
        out, err, returncode = self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('Abraham nickname changed to None',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertLess(0, len(view.contacts)),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveNoData(0) and \
                           len(view.contacts) == 1)
        self.assertIsInstance(view.contacts[0], dict)
        self.view.read(0, 1)
        self.runUntil('Abraham nickname read, None',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual(1, len(view.contacts)),
                                     self.assertIsInstance(view.contacts[0], dict),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveData(0))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])

        # Finally, remove everything.
        logging.log('remove contacts')
        out, err, returncode = self.runCmdline(['--delete-items', '@' + self.managerPrefix + self.uid, 'local', '*'])
        self.runUntil('all contacts removed',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: len(self.view.contacts) == 0 and \
                           len(view.contacts) == 0)

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFilterExistingLimit(self):
        '''TestContacts.testFilterExistingLimit - check that filtering works when applied to static contacts, with maximum number of results'''
        self.setUpView()

        # Override default sorting.
        self.assertEqual("last/first", self.manager.GetSortOrder(timeout=self.timeout))
        self.manager.SetSortOrder("first/last",
                                  timeout=self.timeout)

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:Ace
TEL:1234
TEL:56/78
TEL:+1-800-FOOBAR
TEL:089/7888-99
EMAIL:az@example.com
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
TEL:+1-89-7888-99
END:VCARD''',

# Chárleß has chárless as representation after folding the case.
# This is different from lower case.
# See http://www.boost.org/doc/libs/1_51_0/libs/locale/doc/html/glossary.html#term_case_folding
u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
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
                      until=lambda: self.view.haveData(0, 3))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Browse initial two contacts (= uses MatchAll filter with limit).
        view = ContactsView(self.manager)
        view.search([['limit', '2']])
        self.runUntil('browse results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(2, len(view.contacts))
        view.read(0, 2)
        self.runUntil('browse data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0, 2))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        view.close()

        # Find Abraham and Benjamin but stop at first contact.
        view = ContactsView(self.manager)
        view.search([['any-contains', 'am'], ['limit', '1']])
        self.runUntil('"am" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('one contact',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Changing the limit is not supported.
        with self.assertRaisesRegexp(dbus.DBusException,
                                     r'.*: refining the search must not change the maximum number of results$'):
             view.view.RefineSearch([['limit', '3'], ['any-contains', 'foo']],
                                    timeout=self.timeout)

        # Refine search without actually changing the result.
        view.quiescentCount = 0
        view.view.RefineSearch([['any-contains', 'am'], ['limit', '1']],
                               timeout=self.timeout)
        self.runUntil('end of search refinement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Restrict search to Benjamin. We can leave out the limit, the old
        # stays active automatically. Abraham drops out of the view
        # and Benjamin enters the result subset.
        view.quiescentCount = 0
        view.view.RefineSearch([['any-contains', 'Benjamin']],
                               timeout=self.timeout)
        self.runUntil('end of search refinement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        self.assertFalse(view.haveData(0))
        view.read(0, 1)
        self.runUntil('Benjamin',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Benjamin', view.contacts[0]['structured-name']['given'])

        # Refine again, without changes.
        view.quiescentCount = 0
        view.view.RefineSearch([['any-contains', 'Benjamin']],
                               timeout=self.timeout)
        self.runUntil('end of search refinement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        self.assertEqual(u'Benjamin', view.contacts[0]['structured-name']['given'])

        # Refine to empty view.
        view.quiescentCount = 0
        view.view.RefineSearch([['any-contains', 'XXXBenjamin']],
                               timeout=self.timeout)
        self.runUntil('end of search refinement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFilterLiveLimit(self):
        '''TestContacts.testFilterLiveLimit - check that filtering works while modifying contacts, with a maximum number of results'''
        self.setUpView()

        self.manager.SetSortOrder("first/last",
                                  timeout=self.timeout)

        # Find Abraham and Benjamin, but limit results to first one.
        view = ContactsView(self.manager)
        view.search([['any-contains', 'am'], ['limit', '1']])
        self.runUntil('am search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
FN:Abraham Zoo
NICKNAME:Ace
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
END:VCARD''',

# Chárleß has chárless as representation after folding the case.
# This is different from lower case.
# See http://www.boost.org/doc/libs/1_51_0/libs/locale/doc/html/glossary.html#term_case_folding
u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
        # Relies on importing contacts sorted ascending by file name.
        luids = self.extractLUIDs(out)
        logging.printf('created contacts with luids: %s' % luids)

        # Run until the view has adapted.
        self.runUntil('view with one contact',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: len(view.contacts) > 0 and len(self.view.contacts) == 3)
        # Don't wait for more contacts here. They shouldn't come, and if
        # they do, we'll notice it below.
        self.assertEqual(1, len(view.contacts))

        # Read contacts.
        logging.log('reading contacts')
        view.read(0, 1)
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: view.haveData(0) and \
                           self.view.haveData(0, 3))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Matched contact remains matched.
        item = os.path.join(self.contacts, 'contact%d.vcf' % 0)
        output = codecs.open(item, "w", "utf-8")
        output.write(u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
FN:Abraham Zoo
NICKNAME:King
END:VCARD''')
        output.close()
        logging.log('change nick of Abraham')
        out, err, returncode = self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('Abraham nickname changed',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual(1, len(view.contacts)),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveNoData(0) and view.haveNoData(0))
        view.read(0, 1)
        self.view.read(0, 1)
        self.runUntil('Abraham nickname read',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual(1, len(view.contacts)),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveData(0) and view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])

        # Unmatched contact gets matched, but stays out of view.
        item = os.path.join(self.contacts, 'contact%d.vcf' % 0)
        output = codecs.open(item, "w", "utf-8")
        output.write(u'''BEGIN:VCARD
VERSION:3.0
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
NICKNAME:mamam
END:VCARD''')
        output.close()
        logging.log('change nick of Charly')
        out, err, returncode = self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[2]])
        self.runUntil('Charly nickname changed',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertLess(0, len(view.contacts)),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveNoData(2))
        self.assertEqual(1, len(view.contacts))
        self.assertTrue(view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertFalse(self.view.haveData(2))
        self.view.read(2, 1)
        self.runUntil('Abraham nickname read, II',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual(1, len(view.contacts)),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveData(2) and \
                           view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Invert sort order.
        self.manager.SetSortOrder("last/first",
                                  timeout=self.timeout)
        self.runUntil('reordering',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveNoData(0) and \
                           self.view.haveNoData(2) and \
                           view.haveNoData(0, 1))
        view.read(0, 1)
        self.view.read(0, 3)
        self.runUntil('read reordered contacts',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveData(0, 3) and \
                           view.haveData(0, 1))
        self.assertEqual(1, len(view.contacts))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])
        self.assertEqual(3, len(self.view.contacts))
        self.assertEqual(u'Charly', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[2]['structured-name']['given'])

        # And back again.
        self.manager.SetSortOrder("first/last",
                                  timeout=self.timeout)
        self.runUntil('reordering, II',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveNoData(0) and \
                           self.view.haveNoData(2) and \
                           view.haveNoData(0, 1))
        view.read(0, 1)
        self.view.read(0, 3)
        self.runUntil('read reordered contacts, II',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveData(0, 3) and \
                           view.haveData(0, 1))
        self.assertEqual(1, len(view.contacts))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(3, len(self.view.contacts))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Matched contact gets unmatched.
        item = os.path.join(self.contacts, 'contact%d.vcf' % 0)
        output = codecs.open(item, "w", "utf-8")
        output.write(u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abrahan
FN:Abrahan Zoo
NICKNAME:None
END:VCARD''')
        output.close()
        logging.log('change name of Abraham')
        out, err, returncode = self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('Abraham -> Abrahan',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertLess(0, len(view.contacts)),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveNoData(0) and \
                           len(view.contacts) == 1 and \
                           view.haveNoData(0))
        view.read(0, 1)
        self.view.read(0, 1)
        self.runUntil('Abrahan read',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual(1, len(view.contacts)),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: self.view.haveData(0) and view.haveData(0))
        self.assertEqual(u'Benjamin', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abrahan', self.view.contacts[0]['structured-name']['given'])

        # Finally, remove everything.
        logging.log('remove contacts')
        out, err, returncode = self.runCmdline(['--delete-items', '@' + self.managerPrefix + self.uid, 'local', '*'])
        self.runUntil('all contacts removed',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: len(self.view.contacts) == 0 and \
                           len(view.contacts) == 0)

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFilterLiveLimitInverted(self):
        '''TestContacts.testFilterLiveLimitInverted - check that filtering works while modifying contacts, with a maximum number of results, and inverted sort order'''
        self.setUpView()

        self.manager.SetSortOrder("last/first",
                                  timeout=self.timeout)

        # Find Benjamin and Abraham, but limit results to first one.
        view = ContactsView(self.manager)
        view.search([['any-contains', 'am'], ['limit', '1']])
        self.runUntil('am search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
FN:Abraham Zoo
NICKNAME:Ace
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
FN:Benjamin Yeah
END:VCARD''',

# Chárleß has chárless as representation after folding the case.
# This is different from lower case.
# See http://www.boost.org/doc/libs/1_51_0/libs/locale/doc/html/glossary.html#term_case_folding
u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
        # Relies on importing contacts sorted ascending by file name.
        luids = self.extractLUIDs(out)
        logging.printf('created contacts with luids: %s' % luids)

        # Run until the view has adapted.
        self.runUntil('view with one contact',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: len(view.contacts) > 0 and len(self.view.contacts) == 3)
        # Don't wait for more contacts here. They shouldn't come, and if
        # they do, we'll notice it below.
        self.assertEqual(1, len(view.contacts))

        # Read contacts.
        logging.log('reading contacts')
        view.read(0, 1)
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: view.haveData(0) and \
                           self.view.haveData(0, 3))
        self.assertEqual(u'Benjamin', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[2]['structured-name']['given'])

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFilterLiveLimitRemove(self):
        '''TestContacts.testFilterLiveLimitRemove - check that filtering works while removing a contact, with a maximum number of results'''
        self.setUpView()

        self.manager.SetSortOrder("first/last",
                                  timeout=self.timeout)

        # Find Abraham and Benjamin, but limit results to first one.
        view = ContactsView(self.manager)
        view.search([['any-contains', 'am'], ['limit', '1']])
        self.runUntil('am search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
FN:Abraham Zoo
NICKNAME:Ace
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
END:VCARD''',

# Chárleß has chárless as representation after folding the case.
# This is different from lower case.
# See http://www.boost.org/doc/libs/1_51_0/libs/locale/doc/html/glossary.html#term_case_folding
u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
        # Relies on importing contacts sorted ascending by file name.
        luids = self.extractLUIDs(out)
        logging.printf('created contacts with luids: %s' % luids)

        # Run until the view has adapted.
        self.runUntil('view with one contact',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: len(view.contacts) > 0 and len(self.view.contacts) == 3)
        # Don't wait for more contacts here. They shouldn't come, and if
        # they do, we'll notice it below.
        self.assertEqual(1, len(view.contacts))

        # Read contacts.
        logging.log('reading contacts')
        view.read(0, 1)
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: view.haveData(0) and \
                           self.view.haveData(0, 3))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Remove Abraham. Gets replaced by Benjamin in the view.
        self.runCmdline(['--delete-items', '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('view with Benjamin',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: len(view.contacts) == 1 and view.haveNoData(0) and len(self.view.contacts) == 2)
        view.read(0, 1)
        self.runUntil('Benjamin',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: view.haveData(0) and \
                           self.view.haveData(0, 2))
        self.assertEqual(u'Benjamin', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[1]['structured-name']['given'])

        # Remove Benjamin.
        self.runCmdline(['--delete-items', '@' + self.managerPrefix + self.uid, 'local', luids[1]])
        self.runUntil('view without Benjamin',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: len(view.contacts) == 0 and len(self.view.contacts) == 1)
        self.assertEqual(u'Charly', self.view.contacts[0]['structured-name']['given'])

    @timeout(60)
    @property("snapshot", "simple-sort")
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testContactWrite(self):
        '''TestContacts.testContactWrite - add, update and remove contact'''
        self.setUpView(peers=[], withSystemAddressBook=True)

        # Use unicode strings to make the assertEqual output nicer in case
        # of a mismatch. It's not necessary otherwise.
        #
        # This covers all fields which can be written by the folks EDS
        # backend.
        john = {
             'full-name': 'John Doe',
             'groups': ['Foo', 'Bar'],
             'location': (30.12, -130.34),
             'structured-name': {
                  'family': 'Doe',
                  'given': 'John',
                  'additional': 'D.',
                  'prefixes': 'Mr.',
                  'suffixes': 'Sr.'
                  },
             # 'nickname': 'Johnny', TODO: should be stored by folks, currently not supported
             'birthday': (2011, 12, 1),
             # 'photo': 'file:///tmp/photo.png', TODO: test with real file, folks will store the content of it
             # 'gender', 'male', not exposed via D-Bus
             # 'im': ...
             # 'is-favorite': ...
             'emails': [
                  ( 'john.doe@work', [ 'work' ] ),
                  ( 'john@home', [ 'home' ] ),
                  ],
             'phones': [
                  ( '1234', ['fax']),
                  ( '5678', ['cell', 'work'] ),
                  ( 'foobar', dbus.Array(signature="s")), # empty string list
                  ],
             'addresses': [
                  (
                       {
                            'country': 'United States of America',
                            'extension': 'ext',
                            'locality': 'New York',
                            'po-box': 'box',
                            'region': 'NY',
                            'street': 'Lower East Side',
                            },
                       ['work']
                       ),
                  (
                       {
                            'locality': 'Boston',
                            'street': 'Main Street',
                            },
                       dbus.Array(signature="s") # empty string list
                       ),
                  ],
             # 'web-services'
             'roles': [
                  {
                       'organisation': 'ACME',
                       'title': 'president',
                       'role': 'decision maker',
                       },
                  {
                       'organisation': 'BAR',
                       'title': 'CEO',
                       'role': 'spokesperson',
                       },
                  ],
             'notes': [
                  'note\n\ntext',
                  # TODO: notes -> note (EDS only supports one NOTE)
                  ],
             'urls': [
                  ('chat', ['x-video']),
                  ('free/busy', ['x-free-busy']),
                  ('http://john.doe.com', ['x-home-page']),
                  ('web log', ['x-blog']),
                  ],
             }

        with self.assertRaisesRegexp(dbus.DBusException,
                                     r'.*: only the system address book is writable'):
             self.manager.AddContact('no-such-address-book',
                                     john,
                                     timeout=self.timeout)

        # Add new contact.
        localID = self.manager.AddContact('', john,
                                          timeout=self.timeout)
        john['source'] = [('', unicode(localID))]

        self.runUntil('view with one contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) > 0)
        self.assertEqual(1, len(self.view.contacts))
        self.view.read(0, 1)
        self.runUntil('contact data',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
        contact = self.view.contacts[0]
        john['id'] = contact.get('id', '<???>')
        self.assertEqual(john, contact, sortLists=True)

        with self.assertRaisesRegexp(dbus.DBusException,
                                     r'''.*: contact with local ID 'no-such-local-id' not found in system address book'''):
             self.manager.ModifyContact('',
                                        'no-such-local-id',
                                        john,
                                        timeout=self.timeout)

        # Update the contact.
        john = {
             'source': [('', localID)],
             'id': contact.get('id', '<???>'),

             'full-name': 'John A. Doe',
             'groups': ['Foo', 'Bar'],
             'location': (30.12, -130.34),
             'structured-name': {
                  'family': 'Doe',
                  'given': 'John',
                  'additional': 'A.',
                  'prefixes': 'Mr.',
                  'suffixes': 'Sr.'
                  },
             # 'nickname': 'Johnny', TODO: should be stored by folks, currently not supported - https://bugzilla.gnome.org/show_bug.cgi?id=686695
             'birthday': (2011, 12, 24),
             # 'photo': 'file:///tmp/photo.png', TODO: test with real file, folks will store the content of it
             # 'gender', 'male', not exposed via D-Bus
             # 'im': ...
             # 'is-favorite': ...
             'emails': [
                  ( 'john2@home', [ 'home' ] ),
                  ],
             'phones': [
                  ( '1234', ['fax']),
                  ( '56789', ['work'] ),
                  ],
             'addresses': [
                  (
                       {
                            'country': 'United States of America',
                            'extension': 'ext',
                            'locality': 'New York',
                            'po-box': 'box',
                            'region': 'NY',
                            'street': 'Upper East Side',
                            },
                       ['work']
                       ),
                  (
                       {
                            'country': 'United States of America',
                            'locality': 'Boston',
                            'street': 'Main Street',
                            },
                       dbus.Array(signature="s") # empty string list
                       ),
                  ],
             # 'web-services'
             'roles': [
                  {
                       'organisation': 'ACME',
                       'title': 'senior president',
                       'role': 'scapegoat',
                       },
                  ],
             'notes': [
                  'note\n\ntext modified',
                  # TODO: notes -> note (EDS only supports one NOTE)
                  ],
             'urls': [
                  ('http://john.A.doe.com', ['x-home-page']),
                  ('web log 2', ['x-blog']),
                  ],
             }
        self.manager.ModifyContact('', localID, john,
                                   timeout=self.timeout)
        self.runUntil('modified contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 1 and self.view.haveNoData(0))
        # Keep asking for data for a while: we may get "modified" signals multiple times,
        # which invalidates data that we just read until the unified address book
        # is stable again.
        start = time.time()
        self.runUntil('modified contact data',
                      check=lambda: (self.assertEqual([], self.view.errors),
                                     self.view.haveData(0) or self.view.read(0, 1) or True),
                      until=lambda: self.view.haveData(0) and time.time() - start > 5)
        self.assertEqual(john, self.view.contacts[0], sortLists=True)

        # Search for modified telephone number.
        # Depends on having a valid country set via env variables.
        view = ContactsView(self.manager)
        view.search([['phone', '56789']])
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))

        # Remove all properties, except for a minimal name.
        # Entirely emtpy contacts make no sense.
        john = {
             'source': [('', localID)],
             'id': contact.get('id', '<???>'),

             'full-name': 'nobody',
             'structured-name': {
                  'given': 'nobody',
                  },
             }
        self.manager.ModifyContact('', localID, john,
                                   timeout=self.timeout)
        self.runUntil('modified contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 1 and self.view.haveNoData(0))
        start = time.time()
        self.runUntil('modified contact data',
                      check=lambda: (self.assertEqual([], self.view.errors),
                                     self.view.haveData(0) or self.view.read(0, 1) or True),
                      until=lambda: self.view.haveData(0) and time.time() - start > 5)
        self.assertEqual(john, self.view.contacts[0], sortLists=True)

        # No longer part of the telephone search view.
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: len(view.contacts) == 0)

        # Remove the contact.
        self.manager.RemoveContact('', localID,
                                   timeout=self.timeout)
        self.runUntil('empty view',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 0)

    # TODO: check that deleting or modifying a contact works directly
    # after starting the PIM manager. The problem is that FolksPersonaStore
    # might still be loading the contacts, in which case looking up the
    # contact would fail.

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8 SYNCEVOLUTION_PIM_DELAY_FOLKS=5")
    def testFilterStartup(self):
        '''TestContacts.testFilterStartup - phone number lookup while folks still loads'''
        self.setUpView(search=None)

        # Override default sorting.
        self.assertEqual("last/first", self.manager.GetSortOrder(timeout=self.timeout))
        self.manager.SetSortOrder("first/last",
                                  timeout=self.timeout)

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:Ace
TEL:1234
TEL:56/78
TEL:+1-800-FOOBAR
TEL:089/788899
EMAIL:az@example.com
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
TEL:+49-89-788899
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])

        # Now start with a phone number search which must look
        # directly in EDS because the unified address book is not
        # ready (delayed via env variable).
        self.view.search([['phone', '089/788899']])
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        self.assertEqual(2, len(self.view.contacts))
        self.view.read(0, 2)
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, 2))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])

        # Wait for final results from folks. The same in this case.
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 1)
        self.assertEqual(2, len(self.view.contacts))
        self.view.read(0, 2)
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, 2))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])

        # Nothing changed when folks became active.
        self.assertEqual([
                  ('added', 0, 2),
                  ('quiescent',),
                  ('quiescent',),
                  ],
                         self.view.events)

    def doFilterStartupRefine(self, simpleSearch=True):
        '''TestContacts.testFilterStartupRefine - phone number lookup while folks still loads, with folks finding more contacts (simple search in EDS) or the same contacts (intelligent search)'''
        self.setUpView(search=None)

        # Override default sorting.
        self.assertEqual("last/first", self.manager.GetSortOrder(timeout=self.timeout))
        self.manager.SetSortOrder("first/last",
                                  timeout=self.timeout)

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:Ace
TEL:1234
TEL:56/78
TEL:+1-800-FOOBAR
TEL:089/788899
EMAIL:az@example.com
END:VCARD''',

# Extra space, breaks suffix match in EDS.
# A more intelligent phone number search in EDS
# will find this again.
u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
TEL:+49-89-7888 99
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])

        # Now start with a phone number search which must look
        # directly in EDS because the unified address book is not
        # ready (delayed via env variable).
        self.view.search([['phone', '089/788899']])
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        if simpleSearch:
             self.assertEqual(1, len(self.view.contacts))
             self.view.read(0, 1)
        else:
             self.assertEqual(2, len(self.view.contacts))
             self.view.read(0, 2)

        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, simpleSearch and 1 or 2))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        if not simpleSearch:
             self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])

        # Wait for final results from folks. Also finds Benjamin.
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 1)
        self.assertEqual(2, len(self.view.contacts))
        self.view.read(0, 2)
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, 2))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])

        if simpleSearch:
             # One contact added by folks.
             self.assertEqual([
                       ('added', 0, 1),
                       ('quiescent',),
                       ('added', 1, 1),
                       ('quiescent',),
                       ],
                              self.view.events)
        else:
             # Two contacts added initially, not updated by folks.
             self.assertEqual([
                       ('added', 0, 2),
                       ('quiescent',),
                       ('quiescent',),
                       ],
                              self.view.events)


    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8 SYNCEVOLUTION_PIM_DELAY_FOLKS=5 SYNCEVOLUTION_PIM_EDS_SUBSTRING=1")
    def testFilterStartupRefine(self):
        '''TestContacts.testFilterStartupRefine - phone number lookup while folks still loads, with folks finding more contacts because we use substring search in EDS'''
        self.doFilterStartupRefine()

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8 SYNCEVOLUTION_PIM_DELAY_FOLKS=5")
    def testFilterStartupRefineSmart(self):
        '''TestContacts.testFilterStartupRefine - phone number lookup while folks still loads, with folks finding the same contacts because we use smart search in EDS'''
        # This test depends on libphonenumber support in EDS!
        self.doFilterStartupRefine(simpleSearch=False)

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8 SYNCEVOLUTION_PIM_DELAY_FOLKS=5")
    def testFilterStartupMany(self):
        '''TestContacts.testFilterStartupMany - phone number lookup in many address books'''
        self.setUpView(search=None, peers=['0', '1', '2'])

        # Override default sorting.
        self.assertEqual("last/first", self.manager.GetSortOrder(timeout=self.timeout))
        self.manager.SetSortOrder("first/last",
                                  timeout=self.timeout)

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:Ace
TEL:1234
TEL:56/78
TEL:+1-800-FOOBAR
TEL:089/788899
EMAIL:az@example.com
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
TEL:+49-89-788899
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact.vcf')
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
             logging.printf('inserting contact %d', i)
             uid = self.uidPrefix + str(i)
             out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + uid, 'local'])

        # Now start with a phone number search which must look
        # directly in EDS because the unified address book is not
        # ready (delayed via env variable).
        self.view.search([['phone', '089/788899']])
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        self.assertEqual(2, len(self.view.contacts))
        self.view.read(0, 2)
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, 2))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])

        # Wait for final results from folks. The same in this case.
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 1)
        self.assertEqual(2, len(self.view.contacts))
        self.view.read(0, 2)
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, 2))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])

        # Nothing changed when folks became active.
        self.assertEqual([
                  ('added', 0, 2),
                  ('quiescent',),
                  ('quiescent',),
                  ],
                         self.view.events)

    @timeout(60)
    def testDeadAgent(self):
        '''TestContacts.testDeadAgent - an error from the agent kills the view'''
        self.setUpView(search=None, peers=[], withSystemAddressBook=True)

        # Insert new contact.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
END:VCARD''',
]):
             item = os.path.join(self.contacts, 'contact.vcf')
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
             logging.printf('inserting contact %d', i)

        out, err, returncode = self.runCmdline(['--import', self.contacts, 'backend=evolution-contacts'])

        # Plug into "ContactsAdded" method so that it throws an error.
        original = self.view.processEvent
        def intercept(message, event):
             original(message, event)
             if event[0] == 'added':
                  raise Exception('fake error')
        self.view.processEvent = intercept
        self.view.search([])
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        self.assertEqual([('added', 0, 1),
                          ('quiescent',)],
                         self.view.events)

        # Expect an error, view should have been closed already.
        with self.assertRaisesRegexp(dbus.DBusException,
                                     "org.freedesktop.DBus.Error.UnknownMethod: .*"):
             self.view.close()

    @timeout(60)
    def testQuiescentOptional(self):
        '''TestContacts.testQuiescentOptional - the Quiescent() method is allowed to fail'''
        self.setUpView(search=None, peers=[], withSystemAddressBook=True)

        # Plug into "Quiescent" method so that it throws an error.
        original = self.view.processEvent
        def intercept(message, event):
             original(message, event)
             if event[0] == 'quiescent':
                  raise Exception('fake error')
        self.view.processEvent = intercept
        self.view.search([])
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        self.assertEqual([('quiescent',)],
                         self.view.events)
        self.view.close()


if __name__ == '__main__':
    xdg = (os.path.join(os.path.abspath('.'), 'temp-testpim', 'config'),
           os.path.join(os.path.abspath('.'), 'temp-testpim', 'local', 'cache'))
    error = ''
    if (os.environ.get('XDG_CONFIG_HOME', None), os.environ.get('XDG_DATA_HOME', None)) != xdg:
         # Don't allow user of the script to erase his normal EDS data.
         error = error + 'testpim.py must be started in a D-Bus session with XDG_CONFIG_HOME=%s XDG_DATA_HOME=%s because it will modify system EDS databases there.\n' % xdg
    if os.environ.get('LANG', '') != 'de_DE.utf-8':
         error = error + 'EDS daemon must use the same LANG=de_DE.utf-8 as tests to get phone number normalization right.\n'
    if error:
         sys.exit(error)
    unittest.main()
