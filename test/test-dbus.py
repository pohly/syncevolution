#! /usr/bin/python -u
#
# Copyright (C) 2009 Intel Corporation
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

import random
import unittest
import subprocess
import time
import os
import errno
import signal
import shutil
import copy
import heapq
import string
import difflib
import traceback
import ConfigParser
import io

import dbus
from dbus.mainloop.glib import DBusGMainLoop
import dbus.service
import gobject
import sys
import traceback
import re
import atexit
import base64

# introduced in python-gobject 2.16, not available
# on all Linux distros => make it optional
try:
    import glib
    have_glib = True
except ImportError:
    have_glib = False

DBusGMainLoop(set_as_default=True)

debugger = os.environ.get("TEST_DBUS_GDB", None) and "gdb" or ""
server = ["syncevo-dbus-server"]
monitor = ["dbus-monitor"]
# primarily for XDG files, but also other temporary files
xdg_root = "temp-test-dbus"
configName = "dbus_unittest"

def usingValgrind():
    return 'valgrind' in os.environ.get("TEST_DBUS_PREFIX", "")

def GrepNotifications(dbuslog):
    '''finds all Notify calls and returns their parameters as list of line lists'''
    return re.findall(r'^method call .* dest=.* .*interface=org.freedesktop.Notifications; member=Notify\n((?:^   .*\n)*)',
                      dbuslog,
                      re.MULTILINE)

# See notification-daemon.py for a stand-alone version.
#
# Embedded here to avoid issues with setting up the environment
# in such a way that the stand-alone version can be run
# properly.
class Notifications (dbus.service.Object):
    '''fake org.freedesktop.Notifications implementation,'''
    '''used when there is none already registered on the session bus'''

    @dbus.service.method(dbus_interface='org.freedesktop.Notifications', in_signature='', out_signature='ssss')
    def GetServerInformation(self):
        return ('test-dbus', 'SyncEvolution', '0.1', '1.1')

    @dbus.service.method(dbus_interface='org.freedesktop.Notifications', in_signature='', out_signature='as')
    def GetCapabilities(self):
        return ['actions', 'body', 'body-hyperlinks', 'body-markup', 'icon-static', 'sound']

    @dbus.service.method(dbus_interface='org.freedesktop.Notifications', in_signature='susssasa{sv}i', out_signature='u')
    def Notify(self, app, replaces, icon, summary, body, actions, hints, expire):
        return random.randint(1,100)

# fork before connecting to the D-Bus daemon
child = os.fork()
if child == 0:
    bus = dbus.SessionBus()
    loop = gobject.MainLoop()
    name = dbus.service.BusName("org.freedesktop.Notifications", bus)
    # start dummy notification daemon, if possible;
    # if it fails, ignore (probably already one running)
    notifications = Notifications(bus, "/org/freedesktop/Notifications")
    loop.run()
    sys.exit(0)

# testing continues in parent process
atexit.register(os.kill, child, 9)
bus = dbus.SessionBus()
loop = gobject.MainLoop()

# log to .dbus.log of a test
class Logging(dbus.service.Object):
    def __init__(self):
        dbus.service.Object.__init__(self, bus, '/test/dbus/py')

    @dbus.service.signal(dbus_interface='t.d.p',
                         signature='s')
    def log(self, str):
        pass

    def printf(self, format, *args):
        self.log(format % args)
logging = Logging()

# Bluez default adapter
bt_adaptor = "/org/bluez/1036/hci0"

# handles D-Bus messages for '/' object in both net.connman and
# org.bluez; must be the same object because D-Bus cannot switch based
# on the bus name (not necessarily included in message)
class RootObject (dbus.service.Object):
    # ConnMan state
    state = "online"
    getPropertiesCalled = False
    waitingForGetProperties = False

    def __init__(self):
        self.bluez_name = dbus.service.BusName('org.bluez', bus)
        self.conn_name = dbus.service.BusName("net.connman", bus)
        dbus.service.Object.__init__(self, bus, '/')

    @dbus.service.method(dbus_interface='org.bluez.Manager', in_signature='', out_signature='o')
    def DefaultAdapter(self):
        return bt_adaptor

    @dbus.service.signal(dbus_interface='org.bluez.Manager', signature='o')
    def DefaultAdapterChanged(self, obj):
        return bt_adaptor

    @dbus.service.method(dbus_interface='net.connman.Manager', in_signature='', out_signature='a{sv}')
    def GetProperties(self):
        # notify TestDBusServerPresence.setUp()?
        if self.waitingForGetProperties:
            loop.quit()
        self.getPropertiesCalled = True
        return { "State" : self.state }

    @dbus.service.signal(dbus_interface='net.connman.Manager', signature='sv')
    def PropertyChanged(self, key, value):
        pass

    def setState(self, state):
        if self.state != state:
            self.state = state
            self.PropertyChanged("State", state)
            # race condition: it happened that method calls
            # reached syncevo-dbus-server before the state change,
            # thus breaking the test
            time.sleep(1)

    def reset(self):
        self.state = "online"
        self.getPropertiesCalled = False
        self.waitingForGetProperties = False

root = RootObject()

def property(key, value):
    """Function decorator which sets an arbitrary property of a test.
    Use like this:
         @property("foo", "bar")
         def testMyTest:
             ...

             print self.getTestProperty("foo", "default")
    """
    def __setProperty(func):
        if not "properties" in dir(func):
            func.properties = {}
        func.properties[key] = value
        return func
    return __setProperty

def timeout(seconds):
    """Function decorator which sets a non-default timeout for a test.
    The default timeout, enforced by DBusTest.runTest(), are 20 seconds.
    Use like this:
        @timeout(60)
        def testMyTest:
            ...
    """
    return property("timeout", seconds)

class Timeout:
    """Implements global time-delayed callbacks."""
    alarms = []
    next_alarm = None
    previous_handler = None
    debugTimeout = False

    @classmethod
    def addTimeout(cls, delay_seconds, callback, use_glib=True):
        """Call function after a certain delay, specified in seconds.
        If possible and use_glib=True, then it will only fire inside
        glib event loop. Otherwise it uses signals. When signals are
        used it is a bit uncertain what kind of Python code can
        be executed. It was observed that trying to append to
        DBusUtil.quit_events before calling loop.quit() caused
        a KeyboardInterrupt"""
        if have_glib and use_glib:
            glib.timeout_add(delay_seconds, callback)
            # TODO: implement removal of glib timeouts
            return None
        else:
            now = time.time()
            if cls.debugTimeout:
                print "addTimeout", now, delay_seconds, callback, use_glib
            timeout = (now + delay_seconds, callback)
            heapq.heappush(cls.alarms, timeout)
            cls.__check_alarms()
            return timeout

    @classmethod
    def removeTimeout(cls, timeout):
        """Remove a timeout returned by a previous addTimeout call.
        None and timeouts which have already fired are acceptable."""
        try:
            cls.alarms.remove(timeout)
        except ValueError:
            pass
        else:
            heapq.heapify(cls.alarms)
            cls.__check_alarms()

    @classmethod
    def __handler(cls, signum, stack):
        """next_alarm has fired, check for expired timeouts and reinstall"""
        if cls.debugTimeout:
            print "fired", time.time()
        cls.next_alarm = None
        cls.__check_alarms()

    @classmethod
    def __check_alarms(cls):
        now = time.time()
        while cls.alarms and cls.alarms[0][0] <= now:
            timeout = heapq.heappop(cls.alarms)
            if cls.debugTimeout:
                print "invoking", timeout
            timeout[1]()

        if cls.alarms:
            if not cls.next_alarm or \
                    cls.next_alarm > cls.alarms[0][0]:
                if cls.previous_handler == None:
                    cls.previous_handler = signal.signal(signal.SIGALRM, cls.__handler)
                cls.next_alarm = cls.alarms[0][0]
                delay = int(cls.next_alarm - now + 0.5)
                if not delay:
                    delay = 1
                if cls.debugTimeout:
                    print "next alarm", cls.next_alarm, delay
                signal.alarm(delay)
        elif cls.next_alarm:
            if cls.debugTimeout:
                print "disarming alarm"
            signal.alarm(0)
            cls.next_alarm = None

# commented out because running it takes time
#class TestTimeout(unittest.TestCase):
class TimeoutTest:
    """unit test for Timeout mechanism"""

    def testOneTimeout(self):
        """testOneTimeout - OneTimeout"""
        self.called = False
        start = time.time()
        def callback():
            self.called = True
        Timeout.addTimeout(2, callback, use_glib=False)
        time.sleep(10)
        end = time.time()
        self.assertTrue(self.called)
        self.assertFalse(end - start < 2)
        self.assertFalse(end - start >= 3)

    def testEmptyTimeout(self):
        """testEmptyTimeout - EmptyTimeout"""
        self.called = False
        start = time.time()
        def callback():
            self.called = True
        Timeout.addTimeout(0, callback, use_glib=False)
        if not self.called:
            time.sleep(10)
        end = time.time()
        self.assertTrue(self.called)
        self.assertFalse(end - start < 0)
        self.assertFalse(end - start >= 1)

    def testTwoTimeouts(self):
        """testTwoTimeouts - TwoTimeouts"""
        self.called = False
        start = time.time()
        def callback():
            self.called = True
        Timeout.addTimeout(2, callback, use_glib=False)
        Timeout.addTimeout(5, callback, use_glib=False)
        time.sleep(10)
        end = time.time()
        self.assertTrue(self.called)
        self.assertFalse(end - start < 2)
        self.assertFalse(end - start >= 3)
        time.sleep(10)
        end = time.time()
        self.assertTrue(self.called)
        self.assertFalse(end - start < 5)
        self.assertFalse(end - start >= 6)

    def testTwoReversedTimeouts(self):
        """testTwoReversedTimeouts - TwoReversedTimeouts"""
        self.called = False
        start = time.time()
        def callback():
            self.called = True
        Timeout.addTimeout(5, callback, use_glib=False)
        Timeout.addTimeout(2, callback, use_glib=False)
        time.sleep(10)
        end = time.time()
        self.assertTrue(self.called)
        self.assertFalse(end - start < 2)
        self.assertFalse(end - start >= 3)
        time.sleep(10)
        end = time.time()
        self.assertTrue(self.called)
        self.assertFalse(end - start < 5)
        self.assertFalse(end - start >= 6)

def TryKill(pid, signal):
    try:
        os.kill(pid, signal)
        return True
    except OSError, ex:
        # might have quit in the meantime, deal with the race
        # condition
        if ex.errno != 3:
            raise ex
    return False

def ShutdownSubprocess(popen, timeout):
    start = time.time()
    if popen.poll() == None:
        # kill process and process group, in case that process has
        # forked children (valgrindcheck.sh + syncevo-dbus-server
        # case or syncevo-dbus-server + local sync)
        TryKill(popen.pid, signal.SIGTERM)
        TryKill(-popen.pid, signal.SIGTERM)
    while popen.poll() == None and start + timeout >= time.time():
        time.sleep(0.01)
    if popen.poll() == None:
        TryKill(popen.pid, signal.SIGKILL)
        TryKill(-popen.pid, signal.SIGKILL)
        while popen.poll() == None and start + timeout + 1 >= time.time():
            time.sleep(0.01)
        return False
    else:
        # there shouldn't be any processes in the process group left now
        # because the parent process has quit normally, but make sure anyway
        TryKill(-popen.pid, signal.SIGKILL)
        return True

class DBusUtil(Timeout):
    """Contains the common run() method for all D-Bus test suites
    and some utility functions."""

    # Use class variables because that way it is ensured that there is
    # only one set of them. Previously instance variables were used,
    # which had the effect that D-Bus signal handlers from test A
    # wrote into variables which weren't the ones used by test B.
    # Unfortunately it is impossible to remove handlers when
    # completing test A.
    events = []
    quit_events = []
    reply = None
    pserver = None
    pmonitor = None
    storedenv = {}

    def getTestProperty(self, key, default):
        """retrieve values set with @property()"""
        test = eval(self.id().replace("__main__.", ""))
        if "properties" in dir(test):
            return test.properties.get(key, default)
        else:
            return default

    def runTest(self, result, own_xdg=True, serverArgs=[], own_home=False, defTimeout=20):
        """Starts the D-Bus server and dbus-monitor before the test
        itself. After the test run, the output of these two commands
        are added to the test's failure, if any. Otherwise the output
        is ignored. A non-zero return code of the D-Bus server is
        logged as separate failure.

        The D-Bus server must print at least one line of output
        before the test is allowed to start.
        
        The commands are run with XDG_DATA_HOME, XDG_CONFIG_HOME,
        XDG_CACHE_HOME pointing towards local dirs
        test-dbus/[data|config|cache] which are removed before each
        test."""

        DBusUtil.events = []
        DBusUtil.quit_events = []
        DBusUtil.reply = None
        self.pserverpid = None

        # allow arbitrarily long diffs in Python unittest
        self.maxDiff = None

        self.killChildren(0)
        # collect zombies
        try:
            while os.waitpid(-1, os.WNOHANG)[0]:
                pass
        except OSError, ex:
            if ex.errno != errno.ECHILD:
                raise ex

        """own_xdg is saved in self for we use this flag to check whether
        to copy the reference directory tree."""
        self.own_xdg = own_xdg
        env = copy.deepcopy(os.environ)
        if own_xdg or own_home:
            shutil.rmtree(xdg_root, True)
        if own_xdg:
            env["XDG_DATA_HOME"] = os.path.abspath(os.path.join(xdg_root, "data"))
            env["XDG_CONFIG_HOME"] = os.path.abspath(os.path.join(xdg_root, "config"))
            env["XDG_CACHE_HOME"] = os.path.abspath(os.path.join(xdg_root, "cache"))
        if own_home:
            env["HOME"] = os.path.abspath(xdg_root)

        # populate xdg_root
        snapshot = self.getTestProperty("snapshot", None)
        if snapshot:
            self.setUpFiles(snapshot)

        # set additional environment variables for the test run,
        # as defined by @property("ENV", "foo=bar x=y")
        for assignment in self.getTestProperty("ENV", "").split():
            var, value = assignment.split("=")
            env[var] = value

        # always print all debug output directly (no output redirection),
        # and increase log level
        if self.getTestProperty("debug", True):
            env["SYNCEVOLUTION_DEBUG"] = "1"

        self.storedenv = env

        # can be set by a test to run additional tests on the content
        # of the D-Bus log
        self.runTestDBusCheck = None

        # testAutoSyncFailure (__main__.TestSessionAPIsDummy) => testAutoSyncFailure_TestSessionAPIsDummy
        testname = str(self).replace(" ", "_").replace("__main__.", "").replace("(", "").replace(")", "")
        dbuslog = testname + ".dbus.log"
        syncevolog = testname + ".syncevo.log"

        self.pmonitor = subprocess.Popen(monitor,
                                         stdout=open(dbuslog, "w"),
                                         stderr=subprocess.STDOUT)
        
        if debugger:
            print "\n%s: %s\n" % (self.id(), self.shortDescription())
            DBusUtil.pserver = subprocess.Popen([debugger] + server,
                                                env=env)

            while True:
                check = subprocess.Popen("ps x | grep %s | grep -w -v -e %s -e grep -e ps" % \
                                             (server[0], debugger),
                                         env=env,
                                         shell=True,
                                         stdout=subprocess.PIPE)
                out, err = check.communicate()
                if out:
                    # process exists, but might still be loading,
                    # so give it some more time
                    print "found syncevo-dbus-server, starting test in two seconds:\n", out
                    time.sleep(2)
                    break
        else:
            logfile = open(syncevolog, "w")
            prefix = os.environ.get("TEST_DBUS_PREFIX", "")
            args = []
            if prefix:
                args.append(prefix)
            args.extend(server)
            args.extend(serverArgs)
            logfile.write("env:\n%s\n\nargs:\n%s\n\n" % (env, args))
            logfile.flush()
            size = os.path.getsize(syncevolog)
            DBusUtil.pserver = subprocess.Popen(args,
                                                preexec_fn=lambda: os.setpgid(0, 0),
                                                env=env,
                                                stdout=logfile,
                                                stderr=subprocess.STDOUT)
            while self.isServerRunning():
                newsize = os.path.getsize(syncevolog)
                if newsize != size:
                    if "] ready to run\n" in open(syncevolog).read():
                        break
                size = newsize
                time.sleep(1)

        # pserver.pid is not necessarily the pid of syncevo-dbus-server.
        # It might be the child of the pserver process.
        self.pserverpid = self.serverPid()

        numerrors = len(result.errors)
        numfailures = len(result.failures)
        if debugger:
            print "\nrunning\n"

        # Find out what test function we run and look into
        # the function definition to see whether it comes
        # with a non-default timeout, otherwise use a 20 second
        # timeout.
        timeout = self.getTestProperty("timeout", defTimeout)
        timeout_handle = None
        if timeout and not debugger:
            def timedout():
                error = "%s timed out after %d seconds, current quit events: %s" % (self.id(), timeout, self.quit_events)
                if Timeout.debugTimeout:
                    print error
                raise Exception(error)
            timeout_handle = self.addTimeout(timeout, timedout, use_glib=False)
        try:
            self.running = True
            unittest.TestCase.run(self, result)
        except KeyboardInterrupt, ex:
            # somehow this happens when timedout() above raises the exception
            # while inside glib main loop
            result.errors.append((self,
                                  "interrupted by timeout (%d seconds, current quit events: %s) or CTRL-C or Python signal handler problem, exception is: %s" % (timeout, self.quit_events, traceback.format_exc())))
        self.running = False
        self.removeTimeout(timeout_handle)
        if debugger:
            print "\ndone, quit gdb now\n"
        hasfailed = numerrors + numfailures != len(result.errors) + len(result.failures)

        if debugger:
            # allow debugger to run as long as it is needed
            DBusUtil.pserver.communicate()

        # Find and kill all sub-processes except for dbus-monitor:
        # first try SIGTERM, then if they don't respond, SIGKILL. The
        # latter is an error. How much time we allow them between
        # SIGTERM and SIGTERM depends on how much work still needs to
        # be done after being asked to quit. valgrind leak checking
        # can take a while.
        unresponsive = self.killChildren(usingValgrind() and 60 or 20)
        if unresponsive:
            error = "/".join(unresponsive) + " had to be killed with SIGKILL"
            print "   ", error
            result.errors.append((self, error))

        serverout = open(syncevolog).read()
        if DBusUtil.pserver is not None and DBusUtil.pserver.returncode != -15:
            hasfailed = True
        if hasfailed:
            # give D-Bus time to settle down
            time.sleep(1)
        if not ShutdownSubprocess(self.pmonitor, 5):
            print "   dbus-monitor had to be killed with SIGKILL"
            result.errors.append((self,
                                  "dbus-monitor had to be killed with SIGKILL"))
        monitorout = open(dbuslog).read()
        report = "\n\nD-Bus traffic:\n%s\n\nserver output:\n%s\n" % \
            (monitorout, serverout)
        if self.runTestDBusCheck:
            try:
                self.runTestDBusCheck(self, monitorout)
            except:
                # only append report if not part of some other error below
                result.errors.append((self,
                                      "D-Bus log failed check: %s\n%s" % (sys.exc_info()[1], (not hasfailed and report) or "")))
        # detect the expected "killed by signal TERM" both when
        # running syncevo-dbus-server directly (negative value) and
        # when valgrindcheck.sh returns the error code 128 + 15 = 143
        if DBusUtil.pserver and \
           DBusUtil.pserver.returncode and \
           DBusUtil.pserver.returncode != 128 + 15 and \
           DBusUtil.pserver.returncode != -15:
            # create a new failure specifically for the server
            result.errors.append((self,
                                  "server terminated with error code %d%s" % (DBusUtil.pserver.returncode, report)))
        elif numerrors != len(result.errors):
            # append report to last error
            result.errors[-1] = (result.errors[-1][0], result.errors[-1][1] + report)
        elif numfailures != len(result.failures):
            # same for failure
            result.failures[-1] = (result.failures[-1][0], result.failures[-1][1] + report)

    def isServerRunning(self):
        """True while the syncevo-dbus-server executable is still running"""
        return DBusUtil.pserver and DBusUtil.pserver.poll() == None

    def getChildren(self):
        '''Find all children of the current process (ppid = out pid,
        in our process group, or in process group of known
        children). Return mapping from pid to name.'''

        # Any of the known children might have its own process group.
        # syncevo-dbus-server definitely does (we create it like that);
        # that allows us to find its forked processes.
        pgids = []
        if self.pmonitor:
            pgids.append(self.pmonitor.pid)
        if self.pserverpid:
            pgids.append(self.pserverpid)

        # Maps from pid to name, process ID, process group ID.
        procs = {}
        statre = re.compile(r'^\d+ \((?P<name>.*?)\) \S (?P<ppid>\d+) (?P<pgid>\d+)')
        for process in os.listdir('/proc'):
            try:
                pid = int(process)
                stat = open('/proc/%d/stat' % pid).read()
                m = statre.search(stat)
                if m:
                    procs[pid] = m.groupdict()
                    for i in ('ppid', 'pgid'):
                        procs[pid][i] = int(procs[pid][i])
            except:
                # ignore all errors
                pass
        logging.printf("found processes: %s", procs)
        # Now determine direct or indirect children.
        children = {}
        mypid = os.getpid()
        def isChild(pid):
            if pid == mypid:
                return False
            if not procs.get(pid, None):
                return False
            return procs[pid]['ppid'] == mypid or \
                procs[pid]['pgid'] in pgids or \
                isChild(procs[pid]['ppid'])
        for pid, info in procs.iteritems():
            if isChild(pid):
                children[pid] = info['name']
        # Exclude dbus-monitor and forked test-dbus.py, they are handled separately.
        if self.pmonitor:
            del children[self.pmonitor.pid]
        del children[child]
        logging.printf("found children: %s", children)
        return children

    def killChildren(self, delay):
        '''Find all children of the current process and kill them. First send SIGTERM,
        then after a grace period SIGKILL.'''
        children = self.getChildren()
        # First pass with SIGTERM?
        if delay:
            for pid, name in children.iteritems():
                logging.printf("sending SIGTERM to %d %s", pid, name)
                TryKill(pid, signal.SIGTERM)
        start = time.time()
        logging.printf("starting to wait for process termination at %s", time.asctime(time.localtime(start)))
        while delay and start + delay >= time.time():
            # Check if any process has quit, remove from list.
            pids = copy.deepcopy(children)
            def checkKnown(p):
                if (p):
                    if p.pid in pids:
                        del pids[p.pid]
                    if p.poll() != None and p.pid in children:
                        logging.printf("known process %d %s has quit at %s",
                                       p.pid, children[p.pid], time.asctime())
                        del children[p.pid]
            checkKnown(self.pserver)
            for pid, name in pids.iteritems():
                try:
                    res = os.waitpid(pid, os.WNOHANG)
                    if res[0]:
                        # got result, remove process
                        logging.printf("got status for process %d %s at %s",
                                       pid, name, time.asctime())
                        del children[pid]
                except OSError, ex:
                    if ex.errno == errno.ECHILD:
                        # someone else must have been faster, also okay
                        logging.printf("process %d %s gone at %s",
                                       pid, name, time.asctime())
                        del children[pid]
                    else:
                        raise ex
            if not children:
                # All children quit normally.
                logging.printf("all process gone at %s",
                               time.asctime())
                return []
        # Force killing of remaining children. It's still possible
        # that one of them quits before we get around to sending the
        # signal.
        logging.printf("starting to kill unresponsive processes at %s", time.asctime())
        killed = []
        for pid, name in children.iteritems():
            if TryKill(pid, signal.SIGKILL):
                logging.printf("killed %d %s", pid, name)
                killed.append("%d %s" % (pid, name))
        return killed

    def serverExecutableHelper(self, pid):
        self.assertTrue(self.isServerRunning())
        maps = open("/proc/%d/maps" % pid, "r")
        regex = re.compile(r'[0-9a-f]*-[0-9a-f]* r-xp [0-9a-f]* [^ ]* \d* *(.*)\n')
        parentre = re.compile(r'^PPid:\s+(\d+)', re.MULTILINE)
        for line in maps:
            match = regex.match(line)
            if match:
                # must be syncevo-dbus-server
                res = match.group(1)
                if 'syncevo-dbus-server' in res:
                    return (res, pid)
                # not found, try children
                for process in os.listdir('/proc'):
                    try:
                        status = open('/proc/%s/status' % process).read()
                        parent = parentre.search(status)
                        if parent and int(parent.group(1)) == pid:
                            res = self.serverExecutableHelper(int(process))
                            if res:
                                return res
                    except:
                        # ignore all errors
                        pass
        # no result
        return None

    def serverExecutable(self):
        """returns full path of currently running syncevo-dbus-server binary"""
        res = self.serverExecutableHelper(DBusUtil.pserver.pid)
        self.assertTrue(res)
        return res[0]

    def serverPid(self):
        """PID of syncevo-dbus-server, None if not running. Works regardless whether it is
        started directly or with a wrapper script like valgrindcheck.sh."""
        res = self.serverExecutableHelper(DBusUtil.pserver.pid)
        self.assertTrue(res)
        return res[1]

    def setUpServer(self):
        self.server = dbus.Interface(bus.get_object('org.syncevolution',
                                                    '/org/syncevolution/Server'),
                                     'org.syncevolution.Server')

    def createSession(self, config, wait, flags=[]):
        """Return sessionpath and session object for session using 'config'.
        A signal handler calls loop.quit() when this session becomes ready.
        If wait=True, then this call blocks until the session is ready.
        """
        if flags:
            sessionpath = self.server.StartSessionWithFlags(config, flags)
        else:
            sessionpath = self.server.StartSession(config)

        def session_ready(object, ready):
            if self.running and ready and object == sessionpath:
                DBusUtil.quit_events.append("session " + object + " ready")
                loop.quit()

        signal = bus.add_signal_receiver(session_ready,
                                         'SessionChanged',
                                         'org.syncevolution.Server',
                                         self.server.bus_name,
                                         None,
                                         byte_arrays=True,
                                         utf8_strings=True)
        session = dbus.Interface(bus.get_object(self.server.bus_name,
                                                sessionpath),
                                 'org.syncevolution.Session')
        status, error, sources = session.GetStatus(utf8_strings=True)
        if wait and status == "queueing":
            # wait for signal
            loop.run()
            self.assertEqual(DBusUtil.quit_events, ["session " + sessionpath + " ready"])
        elif DBusUtil.quit_events:
            # signal was processed inside D-Bus call?
            self.assertEqual(DBusUtil.quit_events, ["session " + sessionpath + " ready"])
        if wait:
            # signal no longer needed, remove it because otherwise it
            # might record unexpected "session ready" events
            signal.remove()
        DBusUtil.quit_events = []
        return (sessionpath, session)

    def setUpSession(self, config, flags=[]):
        """stores ready session in self.sessionpath and self.session"""
        self.sessionpath, self.session = self.createSession(config, True, flags)

    def progressChanged(self, *args, **keywords):
        '''subclasses override this method to write specified callbacks for ProgressChanged signals
           It is called by progress signal receivers in setUpListeners'''
        pass

    def statusChanged(self, *args, **keywords):
        '''subclasses override this method to write specified callbacks for StatusChanged signals
           It is called by status signal receivers in setUpListeners'''
        pass

    def setUpListeners(self, sessionpath):
        """records progress and status changes in DBusUtil.events and
        quits the main loop when the session is done"""

        def progress(*args, **keywords):
            if self.running:
                DBusUtil.events.append(("progress", args, keywords['path']))
                self.progressChanged(*args, **keywords)

        def status(*args, **keywords):
            if self.running:
                DBusUtil.events.append(("status", args, keywords['path']))
                self.statusChanged(*args, **keywords)
                if args[0] == "done":
                    if sessionpath:
                        DBusUtil.quit_events.append("session " + sessionpath + " done")
                    else:
                        DBusUtil.quit_events.append("session done")
                    loop.quit()

        bus.add_signal_receiver(progress,
                                'ProgressChanged',
                                'org.syncevolution.Session',
                                self.server.bus_name,
                                sessionpath,
                                path_keyword='path',
                                byte_arrays=True,
                                utf8_strings=True)
        bus.add_signal_receiver(status,
                                'StatusChanged',
                                'org.syncevolution.Session',
                                self.server.bus_name,
                                sessionpath,
                                path_keyword='path',
                                byte_arrays=True,
                                utf8_strings=True)

    def setUpConfigListeners(self):
        """records ConfigChanged signal and records it in DBusUtil.events, then quits the loop"""

        def config():
            if self.running:
                DBusUtil.events.append("ConfigChanged")
                DBusUtil.quit_events.append("ConfigChanged")
                loop.quit()

        bus.add_signal_receiver(config,
                                'ConfigChanged',
                                'org.syncevolution.Server',
                                self.server.bus_name,
                                byte_arrays=True, 
                                utf8_strings=True)

    def setUpConnectionListeners(self, conpath):
        """records connection signals (abort and reply), quits when
        getting an abort"""

        def abort():
            if self.running:
                DBusUtil.events.append(("abort",))
                DBusUtil.quit_events.append("connection " + conpath + " aborted")
                loop.quit()

        def reply(*args):
            if self.running:
                DBusUtil.reply = args
                if args[3]:
                    DBusUtil.quit_events.append("connection " + conpath + " got final reply")
                else:
                    DBusUtil.quit_events.append("connection " + conpath + " got reply")
                loop.quit()

        bus.add_signal_receiver(abort,
                                'Abort',
                                'org.syncevolution.Connection',
                                self.server.bus_name,
                                conpath,
                                byte_arrays=True, 
                                utf8_strings=True)
        bus.add_signal_receiver(reply,
                                'Reply',
                                'org.syncevolution.Connection',
                                self.server.bus_name,
                                conpath,
                                byte_arrays=True, 
                                utf8_strings=True)

    def setUpLocalSyncConfigs(self, childPassword=None, enableCalendar=False):
        # create file<->file configs
        self.setUpSession("target-config@client")
        addressbook = { "sync": "two-way",
                        "backend": "file",
                        "databaseFormat": "text/vcard",
                        "database": "file://" + xdg_root + "/client" }
        calendar = { "sync": "two-way",
                     "backend": "file",
                     "databaseFormat": "text/calendar",
                     "database": "file://" + xdg_root + "/client-calendar" }
        if childPassword:
            addressbook["databaseUser"] = "foo-user"
            addressbook["databasePassword"] = childPassword
        config = {"" : { "loglevel": "4" } }
        config["source/addressbook"] = addressbook
        if enableCalendar:
            config["source/calendar"] = calendar
        self.session.SetConfig(False, False, config)
        self.session.Detach()
        self.setUpSession("server")
        config = {"" : { "loglevel": "4",
                         "syncURL": "local://@client",
                         "RetryDuration": self.getTestProperty("resendDuration", "60"),
                         "peerIsClient": "1" },
                  "source/addressbook": { "sync": "two-way",
                                          "uri": "addressbook",
                                          "backend": "file",
                                          "databaseFormat": "text/vcard",
                                          "database": "file://" + xdg_root + "/server" } }
        if enableCalendar:
            config["source/calendar"] = { "sync": "two-way",
                                          "uri": "calendar",
                                          "backend": "file",
                                          "databaseFormat": "text/calendar",
                                          "database": "file://" + xdg_root + "/server-calendar" }
        self.session.SetConfig(False, False, config)

    def setUpFiles(self, snapshot):
        """ Copy reference directory trees from
        test/test-dbus/<snapshot> to own xdg_root (=./test-dbus). To
        be used only in tests which called runTest() with
        own_xdg=True."""
        self.assertTrue(self.own_xdg)
        # Get the absolute path of the current python file.
        scriptpath = os.path.abspath(os.path.expanduser(os.path.expandvars(sys.argv[0])))
        # reference directory 'test-dbus' is in the same directory as the current python file
        sourcedir = os.path.join(os.path.dirname(scriptpath), 'test-dbus', snapshot)
        """ Directories in test/test-dbus are copied to xdg_root, but
        maybe with different names, mappings are:
                  'test/test-dbus/<snapshot>'   './test-dbus'"""
        pairs = { 'sync4j'                    : '.sync4j'  ,
                  'config'                    : 'config'   ,
                  'cache'                     : 'cache'    ,
                  'data'                      : 'data'     ,
                  'templates'                 : 'templates'}
        for src, dest in pairs.items():
            destpath = os.path.join(xdg_root, dest)
            # make sure the dest directory does not exist, which is required by shutil.copytree
            shutil.rmtree(destpath, True)
            sourcepath = os.path.join(sourcedir, src)
            # if source exists and could be accessed, then copy them
            if os.access(sourcepath, os.F_OK):
                shutil.copytree(sourcepath, destpath)

    def prettyPrintEvents(self, events=None):
        '''Format events as lines without full class specifiers, like this:
status: idle, 0, {}
'''
        if events == None:
            events = DBusUtil.events
        lines = []
        def prettyPrintArg(arg):
            if isinstance(arg, type(())):
                res = []
                for i in arg:
                    res.append(prettyPrintArg(i))
                return '(' + ', '.join(res) + ')'
            elif isinstance(arg, type([])):
                res = []
                for i in arg:
                    res.append(prettyPrintArg(i))
                return '[' + ', '.join(res) + ']'
            elif isinstance(arg, type({})):
                res = []
                items = arg.items()
                items.sort()
                for i,e in items:
                    res.append('%s: %s' % (prettyPrintArg(i), prettyPrintArg(e)))
                return '{' + ', '.join(res) + '}'
            else:
                return str(arg)
        def prettyPrintArgs(args):
            res = []
            for arg in args:
                res.append(prettyPrintArg(arg))
            return ', '.join(res)
        for event in events:
            self.assertTrue(len(event) >= 1,
                            'Unexpected number of entries in event: %s' % str(event))
            if len(event) >= 2:
                lines.append('%s: %s' % (event[0], prettyPrintArgs(event[1])))
            else:
                lines.append(event[0])
        return '\n'.join(lines)

    def assertSyncStatus(self, config, status, error):
        cache = self.own_xdg and os.path.join(xdg_root, 'cache', 'syncevolution') or \
            os.path.join(os.environ['HOME'], '.cache', 'syncevolution')
        entries = [x for x in os.listdir(cache) if x.startswith(config + '-')]
        entries.sort()
        self.assertEqual(1, len(entries))
        config = ConfigParser.ConfigParser()
        content = '[fake]\n' + open(os.path.join(cache, entries[0], 'status.ini')).read()
        config.readfp(io.BytesIO(content))
        self.assertEqual(status, config.getint('fake', 'status'))
        if error == None:
            self.assertFalse(config.has_option('fake', 'error'))
        else:
            self.assertEqual(error, config.get('fake', 'error'))

    def doCheckSync(self, expectedError=0, expectedResult=0, reportOptional=False, numReports=1):
        # check recorded events in DBusUtil.events, first filter them
        statuses = []
        progresses = []
        # Dict is used to check status order.  
        statusPairs = {"": 0, "idle": 1, "running" : 2, "aborting" : 3, "done" : 4}
        for item in DBusUtil.events:
            if item[0] == "status":
                statuses.append(item[1])
            elif item[0] == "progress":
                progresses.append(item[1])

        # check statuses
        lastStatus = ""
        lastSources = {}
        lastError = 0
        for status, error, sources in statuses:
            # consecutive entries should not be equal
            self.assertNotEqual((lastStatus, lastError, lastSources), (status, error, sources))
            # no error, unless expected
            if expectedError:
                if error:
                    self.assertEqual(expectedError, error)
            else:
                self.assertEqual(error, 0)
            # keep order: session status must be unchanged or the next status 
            seps = status.split(';')
            lastSeps = lastStatus.split(';')
            self.assertTrue(statusPairs.has_key(seps[0]))
            self.assertTrue(statusPairs[seps[0]] >= statusPairs[lastSeps[0]])
            # check specifiers
            if len(seps) > 1:
                self.assertEqual(seps[1], "waiting")
            for sourcename, value in sources.items():
                # no error
                self.assertEqual(value[2], 0)
                # keep order: source status must also be unchanged or the next status
                if lastSources.has_key(sourcename):
                    lastValue = lastSources[sourcename]
                    self.assertTrue(statusPairs[value[1]] >= statusPairs[lastValue[1]])

            lastStatus = status
            lastSources = sources
            lastError = error

        # check increasing progress percentage
        lastPercent = 0
        for percent, sources in progresses:
            self.assertFalse(percent < lastPercent)
            lastPercent = percent

        status, error, sources = self.session.GetStatus(utf8_strings=True)
        self.assertEqual(status, "done")
        self.assertEqual(error, expectedError)

        # now check that report is sane
        reports = self.session.GetReports(0, 100, utf8_strings=True)
        if reportOptional and len(reports) == 0:
            # no report was written
            return None
        self.assertEqual(len(reports), numReports)
        if expectedResult:
            self.assertEqual(int(reports[0]["status"]), expectedResult)
        else:
            self.assertEqual(int(reports[0]["status"]), 200)
            self.assertNotIn("error", reports[0])
        return reports[0]

    def checkSync(self, *args, **keywords):
        '''augment any assertion in doCheckSync() with text dump of events'''
        events = self.prettyPrintEvents()
        try:
            return self.doCheckSync(*args, **keywords)
        except AssertionError, ex:
            raise self.failureException('Assertion about the following events failed:\n%s\n%s' %
                                        (events, traceback.format_exc()))

    def assertEqualDiff(self, expected, res):
        '''Like assertEqual(), but raises an error which contains a
        diff of the two parameters. Useful when they are long strings
        (will be split at newlines automatically) or lists (compared
        as-is). Very similar to Python's 2.7 unittest, but also works
        for older Python releases and allows comparing strings against lists.'''
        def splitlines(str):
            '''split any object which looks like a string == has splitlines'''
            if 'splitlines' in dir(str):
                return str.splitlines(True)
            else:
                return str
        expected = splitlines(expected)
        res = splitlines(res)
        if expected != res:
            diff = ''.join(difflib.Differ().compare(expected, res))
            self.fail('differences between expected and actual text\n\n' + diff)

    def assertRegexpMatchesCustom(self, text, regex, msg=None):
        if isinstance(regex, str):
            regex = re.compile(regex)
        if not regex.search(text):
            if msg != None:
                self.fail(msg)
            else:
                self.fail('text does not match regex\n\nText:\n%s\n\nRegex:\n%s' % \
                              (text, regex.pattern))


    def assertInCustom(self, needle, haystack, msg=None):
        if not needle in haystack:
            if msg != None:
                self.fail(msg)
            else:
                self.fail("'" + str(needle) + "' not found in '" + str(haystack) + "'")

    def assertNotInCustom(self, needle, haystack, msg=None):
        if needle in haystack:
            if msg != None:
                self.fail(msg)
            else:
                self.fail("'" + str(needle) + "' found in '" + str(haystack) + "'")

    # reimplement Python 2.7 assertions only in older Python
    if True or not 'assertRegexpMatches' in dir(unittest.TestCase):
        assertRegexpMatches = assertRegexpMatchesCustom

    if not 'assertIn' in dir(unittest.TestCase):
        assertIn = assertInCustom

    if not 'assertNotIn' in dir(unittest.TestCase):
        assertNotIn = assertNotInCustom

class TestDBusServer(DBusUtil, unittest.TestCase):
    """Tests for the read-only Server API."""

    def setUp(self):
        self.setUpServer()

    def run(self, result):
        self.runTest(result)

    def testCapabilities(self):
        """TestDBusServer.testCapabilities - Server.Capabilities()"""
        capabilities = self.server.GetCapabilities()
        capabilities.sort()
        self.assertEqual(capabilities, ['ConfigChanged', 'DatabaseProperties', 'GetConfigName', 'NamedConfig', 'Notifications', 'SessionAttach', 'SessionFlags', 'Version'])

    def testVersions(self):
        """TestDBusServer.testVersions - Server.GetVersions()"""
        versions = self.server.GetVersions()
        self.assertNotEqual(versions["version"], "")
        self.assertNotEqual(versions["system"], None)
        self.assertNotEqual(versions["backends"], None)

    def testGetConfigsEmpty(self):
        """TestDBusServer.testGetConfigsEmpty - Server.GetConfigsEmpty()"""
        configs = self.server.GetConfigs(False, utf8_strings=True)
        self.assertEqual(configs, [])

    @property("ENV", "DBUS_TEST_BLUETOOTH=none")
    def testGetConfigsTemplates(self):
        """TestDBusServer.testGetConfigsTemplates - Server.GetConfigsTemplates()"""
        configs = self.server.GetConfigs(True, utf8_strings=True)
        configs.sort()
        self.assertEqual(configs, ["Funambol",
                                   "Google_Calendar",
                                   "Google_Contacts",
                                   "Goosync",
                                   "Memotoo",
                                   "Mobical",
                                   "Oracle",
                                   "Ovi",
                                   "ScheduleWorld",
                                   "SyncEvolution",
                                   "Synthesis",
                                   "WebDAV",
                                   "Yahoo",
                                   "eGroupware"])

    def testGetConfigScheduleWorld(self):
        """TestDBusServer.testGetConfigScheduleWorld - Server.GetConfigScheduleWorld()"""
        config1 = self.server.GetConfig("scheduleworld", True, utf8_strings=True)
        config2 = self.server.GetConfig("ScheduleWorld", True, utf8_strings=True)
        self.assertNotEqual(config1[""]["deviceId"], config2[""]["deviceId"])
        config1[""]["deviceId"] = "foo"
        config2[""]["deviceId"] = "foo"
        self.assertEqual(config1, config2)

    def testInvalidConfig(self):
        """TestDBusServer.testInvalidConfig - Server.NoSuchConfig exception"""
        try:
            config1 = self.server.GetConfig("no-such-config", False, utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.NoSuchConfig: No configuration 'no-such-config' found")
        else:
            self.fail("no exception thrown")

class TestDBusServerTerm(DBusUtil, unittest.TestCase):
    def setUp(self):
        self.setUpServer()

    def run(self, result):
        self.runTest(result, True, ["-d", "10"])

    @timeout(100)
    def testNoTerm(self):
        """TestDBusServerTerm.testNoTerm - D-Bus server must stay around during calls"""

        """The server should stay alive because we have dbus call within
        the duration. The loop is to make sure the total time is longer 
        than duration and the dbus server still stays alive for dbus calls.""" 
        for i in range(0, 4):
            time.sleep(4)
            try:
                self.server.GetConfigs(True, utf8_strings=True)
            except dbus.DBusException:
                self.fail("dbus server should work correctly")

    @timeout(100)
    def testTerm(self):
        """TestDBusServerTerm.testTerm - D-Bus server must auto-terminate"""
        #sleep a duration and wait for syncevo-dbus-server termination
        time.sleep(16)
        try:
            self.server.GetConfigs(True, utf8_strings=True)
        except dbus.DBusException:
            pass
        else:
            self.fail("no exception thrown")

    @timeout(100)
    def testTermConnection(self):
        """TestDBusServerTerm.testTermConnection - D-Bus server must terminate after closing connection and not sooner"""
        conpath = self.server.Connect({'description': 'test-dbus.py',
                                       'transport': 'dummy'},
                                      False,
                                      "")
        time.sleep(16)
        try:
            self.server.GetConfigs(True, utf8_strings=True)
        except dbus.DBusException:
            self.fail("dbus server should not terminate")

        connection = dbus.Interface(bus.get_object(self.server.bus_name,
                                                   conpath),
                                    'org.syncevolution.Connection')
        connection.Close(False, "good bye", utf8_strings=True)
        time.sleep(16)
        try:
            self.server.GetConfigs(True, utf8_strings=True)
        except dbus.DBusException:
            pass
        else:
            self.fail("no exception thrown")

    @timeout(100)
    def testTermAttachedClients(self):
        """TestDBusServerTerm.testTermAttachedClients - D-Bus server must not terminate while clients are attached"""

        """Also it tries to test the dbus server's behavior when a client 
        attaches the server many times"""
        self.server.Attach()
        self.server.Attach()
        time.sleep(16)
        try:
            self.server.GetConfigs(True, utf8_strings=True)
        except dbus.DBusException:
            self.fail("dbus server should not terminate")
        self.server.Detach()
        time.sleep(16)

        try:
            self.server.GetConfigs(True, utf8_strings=True)
        except dbus.DBusException:
            self.fail("dbus server should not terminate")

        self.server.Detach()
        time.sleep(16)
        try:
            self.server.GetConfigs(True, utf8_strings=True)
        except dbus.DBusException:
            pass
        else:
            self.fail("no exception thrown")

    @timeout(100)
    def testAutoSyncOn(self):
        """TestDBusServerTerm.testAutoSyncOn - D-Bus server must not terminate while auto syncing is enabled"""
        self.setUpSession("scheduleworld")
        # enable auto syncing with a very long delay to prevent accidentally running it
        config = self.session.GetConfig(True, utf8_strings=True)
        config[""]["autoSync"] = "1"
        config[""]["autoSyncInterval"] = "60m"
        self.session.SetConfig(False, False, config)
        self.session.Detach()

        time.sleep(16)

        try:
            self.server.GetConfigs(True, utf8_strings=True)
        except dbus.DBusException:
            self.fail("dbus server should not terminate")

    @timeout(100)
    def testAutoSyncOff(self):
        """TestDBusServerTerm.testAutoSyncOff - D-Bus server must terminate after auto syncing was disabled"""
        self.setUpSession("scheduleworld")
        # enable auto syncing with a very long delay to prevent accidentally running it
        config = self.session.GetConfig(True, utf8_strings=True)
        config[""]["autoSync"] = "1"
        config[""]["autoSyncInterval"] = "60m"
        self.session.SetConfig(False, False, config)
        self.session.Detach()

        self.setUpSession("scheduleworld")
        config[""]["autoSync"] = "0"
        self.session.SetConfig(True, False, config)
        self.session.Detach()

        time.sleep(16)
        try:
            self.server.GetConfigs(True, utf8_strings=True)
        except dbus.DBusException:
            pass
        else:
            self.fail("no exception thrown")

    @timeout(100)
    def testAutoSyncOff2(self):
        """TestDBusServerTerm.testAutoSyncOff2 - D-Bus server must terminate after auto syncing was disabled after a while"""
        self.setUpSession("scheduleworld")
        # enable auto syncing with a very long delay to prevent accidentally running it
        config = self.session.GetConfig(True, utf8_strings=True)
        config[""]["autoSync"] = "1"
        config[""]["autoSyncInterval"] = "60m"
        self.session.SetConfig(False, False, config)
        self.session.Detach()

        # wait until -d 10 second timeout has triggered in syncevo-dbus-server
        time.sleep(11)

        self.setUpSession("scheduleworld")
        config[""]["autoSync"] = "0"
        self.session.SetConfig(True, False, config)
        self.session.Detach()

        # should shut down after the 10 second idle period
        time.sleep(16)

        try:
            self.server.GetConfigs(True, utf8_strings=True)
        except dbus.DBusException:
            pass
        else:
            self.fail("no exception thrown")

    # configure server with files in test/test-dbus/auto-sync before starting it
    @property("snapshot", "auto-sync")
    @timeout(100)
    def testAutoSyncOff3(self):
        """TestDBusServerTerm.testAutoSyncOff3 - start with auto-syncing on, D-Bus server must terminate after disabling auto syncing"""
        # wait until -d 10 second timeout has triggered in syncevo-dbus-server,
        # should not shut down because of auto sync
        time.sleep(11)
        self.assertTrue(self.isServerRunning())

        self.setUpSession("scheduleworld")
        config = self.session.GetConfig(False)
        config[""]["autoSync"] = "0"
        self.session.SetConfig(False, False, config)
        self.session.Detach()

        # should shut down after the 10 second idle period
        time.sleep(16)
        self.assertFalse(self.isServerRunning())


class TestNamedConfig(DBusUtil, unittest.TestCase):
    """Tests for Set/GetNamedConfig"""

    def setUp(self):
        self.setUpServer()

    def run(self, result):
        self.runTest(result)

    def testSetNamedConfigError(self):
        """TestNamedConfig.testSetNamedConfigError - SetNamedConfig() only allowed in 'all-configs' sessions"""
        self.setUpSession("")
        try:
            self.session.SetNamedConfig("foobar", False, False, {})
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                             "org.syncevolution.InvalidCall: SetNameConfig() only allowed in 'all-configs' sessions")
        else:
            self.fail("no exception thrown")

    def testSetNamedConfigErrorTemporary(self):
        """TestNamedConfig.testSetNamedConfigErrorTemporary - SetNamedConfig() only implemented for session config"""
        self.setUpSession("foo", [ "all-configs" ])
        try:
            self.session.SetNamedConfig("foobar", False, True, {})
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                             "org.syncevolution.InvalidCall: SetNameConfig() with temporary config change only supported for config named when starting the session")
        else:
            self.fail("no exception thrown")
        self.session.Detach()

        self.setUpSession("")
        self.session.SetNamedConfig("", False, True, {})

    def testSetNamedConfig(self):
        """TestNamedConfig.testSetNamedConfig - create two different configs in one session"""
        self.setUpSession("", [ "all-configs" ])

        fooConfig = {"": {"username": "foo", "configName": "foo"}}
        barConfig = {"": {"username": "bar", "configName": "bar"}}

        self.session.SetNamedConfig("foo", False, False, fooConfig)
        self.session.SetNamedConfig("bar", False, False, barConfig)

        self.assertEqual(fooConfig, self.server.GetConfig("foo", False))
        self.assertEqual(barConfig, self.server.GetConfig("bar", False))

        self.assertEqual(fooConfig, self.session.GetNamedConfig("foo", False))
        self.assertEqual(barConfig, self.session.GetNamedConfig("bar", False))

class TestDBusServerPresence(DBusUtil, unittest.TestCase):
    """Tests Presence signal and checkPresence API"""

    # TODO: check auto sync + presence combination

    # Our fake ConnMan implementation must be present on the
    # bus also outside of tests, because syncevo-dbus-server
    # will try to call it before setUp(). The implementation's
    # initialization and tearDown() below ensures that the state
    # is "online" outside of tests.
    conn = root

    def setUp(self):
        self.setUpServer()
        self.cbFailure = None
        # we don't know if the GetProperties() call was already
        # processed; if not, wait for it here
        if not self.conn.getPropertiesCalled:
            self.conn.waitingForGetProperties = True
            loop.run()

    def tearDown(self):
        self.conn.reset()
        self.conf = None

    def presenceCB(self,
                   server, status, transport,
                   expected):
        try:
            state = expected.pop(server, None)
            if not state:
                self.fail("unexpected presence signal for config " + server)
            self.failUnlessEqual(status, state[0])
            if not status:
                self.failUnlessEqual(transport, state[1])
        except Exception, ex:
            # tell test method about the problem
            loop.quit()
            self.cbFailure = ex
            # log exception
            raise ex

        if not expected:
            # got all expected signals
            loop.quit()

    def expect(self, expected):
        '''expected: hash from server config name to state+transport'''
        match = bus.add_signal_receiver(lambda x,y,z:
                                            self.presenceCB(x,y,z, expected), \
                                            'Presence',
                                            'org.syncevolution.Server',
                                            self.server.bus_name,
                                            None,
                                            byte_arrays=True,
                                            utf8_strings=True)
        return match

    @property("ENV", "DBUS_TEST_CONNMAN=session DBUS_TEST_NETWORK_MANAGER=none")
    @timeout(100)
    def testPresenceSignal(self):
        """TestDBusServerPresence.testPresenceSignal - check Server.Presence signal"""

        # creating a config does not trigger a Presence signal
        self.setUpSession("foo")
        self.session.SetConfig(False, False, {"" : {"syncURL": "http://http-only-1"}})
        self.session.Detach()

        # go offline
        match = self.expect({"foo" : ("no transport", "")})
        self.conn.setState("idle")
        loop.run()
        self.failIf(self.cbFailure)
        match.remove()

        # Changing the properties temporarily does change
        # the presence of the config although strictly speaking,
        # the presence of the config on disk hasn't changed.
        # Not sure whether we really want that behavior.
        match = self.expect({"foo" : ("", "obex-bt://temp-bluetooth-peer-changed-from-http")})
        self.setUpSession("foo")
        self.session.SetConfig(True, False, {"" : {"syncURL":
        "obex-bt://temp-bluetooth-peer-changed-from-http"}})
        # A ConnMan state change is needed to trigger the presence signal.
        # Definitely not the behavior that we want :-/
        self.conn.setState("failure")
        loop.run()
        self.failIf(self.cbFailure)
        match.remove()
        # remove temporary config change, back to using HTTP
        # BUG BMC #24648 in syncevo-dbus-server: after discarding the temporary
        # config change it keeps using the obex-bt syncURL.
        # Work around that bug in thus test here temporarily
        # by explicitly restoring the previous URL.
        self.session.SetConfig(True, False, {"" : {"syncURL": "http://http-only-1"}})
        self.session.Detach()

        # create second session
        self.setUpSession("bar")
        self.session.SetConfig(False, False, {"" : {"syncURL":
            "http://http-client-2"}})
        self.session.Detach()

        # go back to online mode
        match = self.expect({"foo" : ("", "http://http-only-1"),
                             "bar" : ("", "http://http-client-2")})
        self.conn.setState("online")
        loop.run()
        self.failIf(self.cbFailure)
        match.remove()

        # and offline
        match = self.expect({"foo" : ("no transport", ""),
                             "bar" : ("no transport", "")})
        self.conn.setState("idle")
        loop.run()
        self.failIf(self.cbFailure)
        match.remove()

    @property("ENV", "DBUS_TEST_CONNMAN=session DBUS_TEST_NETWORK_MANAGER=none")
    @timeout(100)
    def testServerCheckPresence(self):
        """TestDBusServerPresence.testServerCheckPresence - check Server.CheckPresence()"""
        self.setUpSession("foo")
        self.session.SetConfig(False, False, {"" : {"syncURL":
        "http://http-client"}})
        self.session.Detach()
        self.setUpSession("bar")
        self.session.SetConfig(False, False, {"" : {"syncURL":
            "obex-bt://bt-client"}})
        self.session.Detach()
        self.setUpSession("foobar")
        self.session.SetConfig(False, False, {"" : {"syncURL":
            "obex-bt://bt-client-mixed http://http-client-mixed"}})
        self.session.Detach()

        # online initially
        (status, transports) = self.server.CheckPresence ("foo")
        self.assertEqual (status, "")
        self.assertEqual (transports, ["http://http-client"])
        (status, transports) = self.server.CheckPresence ("bar")
        self.assertEqual (status, "")
        self.assertEqual (transports, ["obex-bt://bt-client"])
        (status, transports) = self.server.CheckPresence ("foobar")
        self.assertEqual (status, "")
        self.assertEqual (transports, ["obex-bt://bt-client-mixed",
        "http://http-client-mixed"])

        # go offline; Bluetooth remains on
        self.conn.setState("idle")
        # wait until server has seen the state change
        match = bus.add_signal_receiver(lambda x,y,z: loop.quit(),
                                        'Presence',
                                        'org.syncevolution.Server',
                                        self.server.bus_name,
                                        None,
                                        byte_arrays=True,
                                        utf8_strings=True)
        match.remove()
        (status, transports) = self.server.CheckPresence ("foo")
        self.assertEqual (status, "no transport")
        (status, transports) = self.server.CheckPresence ("bar")
        self.assertEqual (status, "")
        self.assertEqual (transports, ["obex-bt://bt-client"])
        (status, transports) = self.server.CheckPresence ("foobar")
        self.assertEqual (status, "")
        self.assertEqual (transports, ["obex-bt://bt-client-mixed"])

    @property("ENV", "DBUS_TEST_CONNMAN=session DBUS_TEST_NETWORK_MANAGER=none")
    @timeout(100)
    def testSessionCheckPresence(self):
        """TestDBusServerPresence.testSessionCheckPresence - check Session.CheckPresence()"""
        self.setUpSession("foobar")
        self.session.SetConfig(False, False, {"" : {"syncURL":
            "obex-bt://bt-client-mixed http://http-client-mixed"}})
        status = self.session.CheckPresence()
        self.failUnlessEqual (status, "")

        # go offline; Bluetooth remains on
        self.conn.setState("idle")
        # wait until server has seen the state change
        match = bus.add_signal_receiver(lambda x,y,z: loop.quit(),
                                        'Presence',
                                        'org.syncevolution.Server',
                                        self.server.bus_name,
                                        None,
                                        byte_arrays=True,
                                        utf8_strings=True)
        match.remove()

        # config uses Bluetooth, so syncing still possible
        status = self.session.CheckPresence()
        self.failUnlessEqual (status, "")

        # now the same without Bluetooth, while offline
        self.session.Detach()
        self.setUpSession("foo")
        self.session.SetConfig(False, False, {"" : {"syncURL": "http://http-only"}})
        status = self.session.CheckPresence()
        self.assertEqual (status, "no transport")

        # go online
        self.conn.setState("online")
        # wait until server has seen the state change
        match = bus.add_signal_receiver(lambda x,y,z: loop.quit(),
                                        'Presence',
                                        'org.syncevolution.Server',
                                        self.server.bus_name,
                                        None,
                                        byte_arrays=True,
                                        utf8_strings=True)
        match.remove()
        status = self.session.CheckPresence()
        self.failUnlessEqual (status, "")

        # temporary config change shall always affect the
        # Session.CheckPresence() result: go offline,
        # then switch to Bluetooth (still present)
        self.conn.setState("idle")
        match = bus.add_signal_receiver(lambda x,y,z: loop.quit(),
                                        'Presence',
                                        'org.syncevolution.Server',
                                        self.server.bus_name,
                                        None,
                                        byte_arrays=True,
                                        utf8_strings=True)
        match.remove()
        status = self.session.CheckPresence()
        self.failUnlessEqual (status, "no transport")
        self.session.SetConfig(True, False, {"" : {"syncURL": "obex-bt://bt-client-mixed"}})
        status = self.session.CheckPresence()
        self.failUnlessEqual (status, "")

    def run(self, result):
        self.runTest(result, True)

class TestDBusSession(DBusUtil, unittest.TestCase):
    """Tests that work with an active session."""

    def setUp(self):
        self.setUpServer()
        self.setUpSession("")

    def run(self, result):
        self.runTest(result)

    def testCreateSession(self):
        """TestDBusSession.testCreateSession - ask for session"""
        self.assertEqual(self.session.GetFlags(), [])
        self.assertEqual(self.session.GetConfigName(), "@default")

    def testAttachSession(self):
        """TestDBusSession.testAttachSession - attach to running session"""
        self.session.Attach()
        self.session.Detach()
        self.assertEqual(self.session.GetFlags(), [])
        self.assertEqual(self.session.GetConfigName(), "@default")

    @timeout(70)
    def testAttachOldSession(self):
        """TestDBusSession.testAttachOldSession - attach to session which no longer has clients"""
        self.session.Detach()
        time.sleep(5)
        # This used to be impossible with SyncEvolution 1.0 because it
        # removed the session right after the previous client
        # left. SyncEvolution 1.1 makes it possible by keeping
        # sessions around for a minute. However, the session is
        # no longer listed because it really should only be used
        # by clients which heard about it before.
        self.assertEqual(self.server.GetSessions(), [])
        self.session.Attach()
        self.assertEqual(self.session.GetFlags(), [])
        self.assertEqual(self.session.GetConfigName(), "@default")
        time.sleep(60)
        self.assertEqual(self.session.GetFlags(), [])

    @timeout(70)
    def testExpireSession(self):
        """TestDBusSession.testExpireSession - ensure that session stays around for a minute"""
        self.session.Detach()
        time.sleep(5)
        self.assertEqual(self.session.GetFlags(), [])
        self.assertEqual(self.session.GetConfigName(), "@default")
        time.sleep(60)
        try:
            self.session.GetFlags()
        except:
            pass
        else:
            self.fail("Session.GetFlags() should have failed")

    def testCreateSessionWithFlags(self):
        """TestDBusSession.testCreateSessionWithFlags - ask for session with some specific flags and config"""
        self.session.Detach()
        self.sessionpath, self.session = self.createSession("FooBar@no-such-context", True, ["foo", "bar"])
        self.assertEqual(self.session.GetFlags(), ["foo", "bar"])
        self.assertEqual(self.session.GetConfigName(), "foobar@no-such-context")

    def testSecondSession(self):
        """TestDBusSession.testSecondSession - a second session should not run unless the first one stops"""
        sessions = self.server.GetSessions()
        self.assertEqual(sessions, [self.sessionpath])
        sessionpath = self.server.StartSession("")
        sessions = self.server.GetSessions()
        self.assertEqual(sessions, [self.sessionpath, sessionpath])

        def session_ready(object, ready):
            if self.running:
                DBusUtil.quit_events.append("session " + object + (ready and " ready" or " done"))
                loop.quit()

        bus.add_signal_receiver(session_ready,
                                'SessionChanged',
                                'org.syncevolution.Server',
                                self.server.bus_name,
                                None,
                                byte_arrays=True,
                                utf8_strings=True)

        def status(*args):
            if self.running:
                DBusUtil.events.append(("status", args))
                if args[0] == "idle":
                    DBusUtil.quit_events.append("session " + sessionpath + " idle")
                    loop.quit()

        bus.add_signal_receiver(status,
                                'StatusChanged',
                                'org.syncevolution.Session',
                                self.server.bus_name,
                                sessionpath,
                                byte_arrays=True, 
                                utf8_strings=True)

        session = dbus.Interface(bus.get_object(self.server.bus_name,
                                                sessionpath),
                                 'org.syncevolution.Session')
        status, error, sources = session.GetStatus(utf8_strings=True)
        self.assertEqual(status, "queueing")
        # use hash so that we can write into it in callback()
        callback_called = {}
        def callback():
            callback_called[1] = "callback()"
            self.session.Detach()
        try:
            t1 = self.addTimeout(2, callback)
            # session 1 done
            loop.run()
            self.assertTrue(callback_called)
            # session 2 ready and idle
            loop.run()
            loop.run()
            expected = ["session " + self.sessionpath + " done",
                        "session " + sessionpath + " idle",
                        "session " + sessionpath + " ready"]
            expected.sort()
            DBusUtil.quit_events.sort()
            self.assertEqual(DBusUtil.quit_events, expected)
            status, error, sources = session.GetStatus(utf8_strings=True)
            self.assertEqual(status, "idle")
        finally:
            self.removeTimeout(t1)

class TestSessionAPIsEmptyName(DBusUtil, unittest.TestCase):
    """Test session APIs that work with an empty server name. Thus, all of session APIs which
       need this kind of checking are put in this class. """

    def setUp(self):
        self.setUpServer()
        self.setUpSession("")

    def run(self, result):
        self.runTest(result)

    def testGetConfigEmptyName(self):
        """TestSessionAPIsEmptyName.testGetConfigEmptyName - reading empty default config"""
        config = self.session.GetConfig(False, utf8_strings=True)

    def testGetTemplateEmptyName(self):
        """TestSessionAPIsEmptyName.testGetTemplateEmptyName - trigger error by getting template for empty server name"""
        try:
            config = self.session.GetConfig(True, utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.NoSuchConfig: No template '' found")
        else:
            self.fail("no exception thrown")

    def testCheckSourceEmptyName(self):
        """TestSessionAPIsEmptyName.testCheckSourceEmptyName - Test the error is reported when the server name is empty for CheckSource"""
        try:
            self.session.CheckSource("", utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.NoSuchSource: '' has no '' source")
        else:
            self.fail("no exception thrown")

    def testGetDatabasesEmptyName(self):
        """TestSessionAPIsEmptyName.testGetDatabasesEmptyName - Test the error is reported when the server name is empty for GetDatabases"""
        try:
            self.session.GetDatabases("", utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.NoSuchSource: '' has no '' source")
        else:
            self.fail("no exception thrown")

    def testGetReportsEmptyName(self):
        """TestSessionAPIsEmptyName.testGetReportsEmptyName - Test reports from all peers are returned in order when the peer name is empty for GetReports"""
        self.setUpFiles('reports')
        reports = self.session.GetReports(0, 0xFFFFFFFF, utf8_strings=True)
        self.assertEqual(len(reports), 7)
        refPeers = ["dummy-test", "dummy", "dummy-test", "dummy-test",
                    "dummy-test", "dummy_test", "dummy-test"]
        for i in range(0, len(refPeers)):
            self.assertEqual(reports[i]["peer"], refPeers[i])

    def testGetReportsContext(self):
        """TestSessionAPIsEmptyName.testGetReportsContext - Test reports from a context are returned when the peer name is empty for GetReports"""
        self.setUpFiles('reports')
        self.session.Detach()
        self.setUpSession("@context")
        reports = self.session.GetReports(0, 0xFFFFFFFF, utf8_strings=True)
        self.assertEqual(len(reports), 1)
        self.assertTrue(reports[0]["dir"].endswith("dummy_+test@context-2010-01-20-10-10"))


class TestSessionAPIsDummy(DBusUtil, unittest.TestCase):
    """Tests that work for GetConfig/SetConfig/CheckSource/GetDatabases/GetReports in Session.
       This class is only working in a dummy config. Thus it can't do sync correctly. The purpose
       is to test some cleanup cases and expected errors. Also, some unit tests for some APIs 
       depend on a clean configuration so they are included here. For those unit tests depending
       on sync, another class is used """

    def setUp(self):
        self.setUpServer()
        # use 'dummy-test' as the server name
        self.setUpSession("dummy-test")
        # default config
        self.config = { 
                         "" : { "syncURL" : "http://impossible-syncurl-just-for-testing-to-avoid-conflict",
                                "username" : "unknown",
                                # the password request tests depend on not having a real password here
                                "password" : "-",
                                "deviceId" : "foo",
                                "RetryInterval" : "10",
                                "RetryDuration" : "20",
                                "ConsumerReady" : "1",
                                "configName" : "dummy-test"
                              },
                         "source/addressbook" : { "sync" : "slow",
                                                  "backend" : "file",
                                                  "database" : "file://" + xdg_root + "/addressbook",
                                                  "databaseFormat" : "text/vcard",
                                                  "uri" : "card"
                                                },
                         "source/calendar"    : { "sync" : "disabled",
                                                  "backend" : "file",
                                                  "database" : "file://" + xdg_root + "/calendar",
                                                  "databaseFormat" : "text/calendar",
                                                  "uri" : "cal"
                                                },
                         "source/todo"        : { "sync" : "disabled",
                                                  "backend" : "file",
                                                  "database" : "file://" + xdg_root + "/todo",
                                                  "databaseFormat" : "text/calendar",
                                                  "uri" : "task"
                                                },
                         "source/memo"        : { "sync" : "disabled",
                                                  "backend" : "file",
                                                  "database" : "file://" + xdg_root + "/memo",
                                                  "databaseFormat" : "text/calendar",
                                                  "uri" : "text"
                                                }
                       }
        # update config
        self.updateConfig = { 
                               "" : { "username" : "doe"},
                               "source/addressbook" : { "sync" : "slow"}
                            }
        self.sources = ['addressbook', 'calendar', 'todo', 'memo']

        # set by SessionReady signal handlers in some tests
        self.auto_sync_session_path = None

    def run(self, result):
        self.runTest(result)

    def clearAllConfig(self):
        """ clear a server config. All should be removed. Used internally. """
        emptyConfig = {}
        self.session.SetConfig(False, False, emptyConfig, utf8_strings=True)

    def setupConfig(self):
        """ create a server with full config. Used internally. """
        self.session.SetConfig(False, False, self.config, utf8_strings=True)

    def testTemporaryConfig(self):
        """TestSessionAPIsDummy.testTemporaryConfig - various temporary config changes"""
        ref = { "": { "loglevel": "2", "configName": "dummy-test" } }
        config = copy.deepcopy(ref)
        self.session.SetConfig(False, False, config, utf8_strings=True)
        # reset
        self.session.SetConfig(False, True, {}, utf8_strings=True)
        self.assertEqual(config, self.session.GetConfig(False, utf8_strings=True))
        # add sync prop
        self.session.SetConfig(True, True, { "": { "loglevel": "100" } }, utf8_strings=True)
        config[""]["loglevel"] = "100"
        self.assertEqual(config, self.session.GetConfig(False, utf8_strings=True))
        # add source
        self.session.SetConfig(True, True, { "source/foobar": { "sync": "two-way" } }, utf8_strings=True)
        config["source/foobar"] = { "sync": "two-way" }
        self.session.SetConfig(True, True, { "": { "loglevel": "100" } }, utf8_strings=True)
        # add source prop
        self.session.SetConfig(True, True, { "source/foobar": { "database": "xyz" } }, utf8_strings=True)
        config["source/foobar"]["database"] = "xyz"
        self.assertEqual(config, self.session.GetConfig(False, utf8_strings=True))
        # reset temporary settings
        self.session.SetConfig(False, True, { }, utf8_strings=True)
        config = copy.deepcopy(ref)
        self.assertEqual(config, self.session.GetConfig(False, utf8_strings=True))

    def testCreateGetConfig(self):
        """TestSessionAPIsDummy.testCreateGetConfig -  test the config is created successfully. """
        self.setUpConfigListeners()
        self.config[""]["username"] = "creategetconfig"
        self.config[""]["password"] = "112233445566778"
        self.setupConfig()
        """ get config and compare """
        config = self.session.GetConfig(False, utf8_strings=True)
        self.assertEqual(config, self.config)
        # terminate session and check whether a "config changed" signal
        # was sent as required
        self.session.Detach()
        loop.run()
        self.assertEqual(DBusUtil.events, ["ConfigChanged"])

    def testUpdateConfig(self):
        """TestSessionAPIsDummy.testUpdateConfig -  test the config is permenantly updated correctly. """
        self.setupConfig()
        """ update the given config """
        self.session.SetConfig(True, False, self.updateConfig, utf8_strings=True)
        config = self.session.GetConfig(False, utf8_strings=True)
        self.assertEqual(config[""]["username"], "doe")
        self.assertEqual(config["source/addressbook"]["sync"], "slow")

    def testUpdateConfigTemp(self):
        """TestSessionAPIsDummy.testUpdateConfigTemp -  test the config is just temporary updated but no effect in storage. """
        self.setupConfig()
        """ set config temporary """
        self.session.SetConfig(True, True, self.updateConfig, utf8_strings=True)
        self.session.Detach()
        """ creat a new session to lose the temporary configs """
        self.setUpSession("dummy-test")
        config = self.session.GetConfig(False, utf8_strings=True)
        """ no change of any properties """
        self.assertEqual(config, self.config)

    def testGetConfigUpdateConfigTemp(self):
        """TestSessionAPIsDummy.testGetConfigUpdateConfigTemp -  test the config is temporary updated and in effect for GetConfig in the current session. """
        self.setupConfig()
        """ set config temporary """
        self.session.SetConfig(True, True, self.updateConfig, utf8_strings=True)
        """ GetConfig is affected """
        config = self.session.GetConfig(False, utf8_strings=True)
        """ no change of any properties """
        self.assertEqual(config[""]["username"], "doe")
        self.assertEqual(config["source/addressbook"]["sync"], "slow")

    def testGetConfigWithTempConfig(self):
        """TestSessionAPIsDummy.testGetConfigWithTempConfig -  test the config is gotten for a new temporary config. """
        """ The given config doesn't exist on disk and it's set temporarily. Then GetConfig should
            return the configs temporarily set. """
        self.session.SetConfig(True, True, self.config, utf8_strings=True)
        config = self.session.GetConfig(False, utf8_strings=True)
        self.assertEqual(config, self.config)

    def testUpdateConfigError(self):
        """TestSessionAPIsDummy.testUpdateConfigError -  test the right error is reported when an invalid property value is set """
        self.setupConfig()
        config = { 
                     "source/addressbook" : { "sync" : "invalid-value"}
                  }
        try:
            self.session.SetConfig(True, False, config, utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.InvalidCall: invalid value 'invalid-value' for "
                                 "property 'sync': 'not one of the valid values (two-way, slow, "
                                 "refresh-from-local, refresh-from-remote = refresh, one-way-from-local, "
                                 "one-way-from-remote = one-way, refresh-from-client = refresh-client, "
                                 "refresh-from-server = refresh-server, one-way-from-client = one-way-client, "
                                 "one-way-from-server = one-way-server, disabled = none)'")
        else:
            self.fail("no exception thrown")

    def testUpdateNoConfig(self):
        """TestSessionAPIsDummy.testUpdateNoConfig -  test the right error is reported when updating properties for a non-existing server """
        try:
            self.session.SetConfig(True, False, self.updateConfig, utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.NoSuchConfig: The configuration 'dummy-test' doesn't exist")
        else:
            self.fail("no exception thrown")

    def testUnknownConfigContent(self):
        """TestSessionAPIsDummy.testUnknownConfigContent - config with unkown must be rejected"""
        self.setupConfig()

        try:
            config1 = copy.deepcopy(self.config)
            config1[""]["no-such-sync-property"] = "foo"
            self.session.SetConfig(False, False, config1, utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.InvalidCall: unknown property 'no-such-sync-property'")
        else:
            self.fail("no exception thrown")

        try:
            config1 = copy.deepcopy(self.config)
            config1["source/addressbook"]["no-such-source-property"] = "foo"
            self.session.SetConfig(False, False, config1, utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.InvalidCall: unknown property 'no-such-source-property'")
        else:
            self.fail("no exception thrown")

        try:
            config1 = copy.deepcopy(self.config)
            config1["no-such-key"] = { "foo": "bar" }
            self.session.SetConfig(False, False, config1, utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.InvalidCall: invalid config entry 'no-such-key'")
        else:
            self.fail("no exception thrown")

    def testClearAllConfig(self):
        """TestSessionAPIsDummy.testClearAllConfig -  test all configs of a server are cleared correctly. """
        """ first set up config and then clear all configs and also check a non-existing config """
        self.setupConfig()
        self.clearAllConfig()
        try:
            config = self.session.GetConfig(False, utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                "org.syncevolution.NoSuchConfig: No configuration 'dummy-test' found")
        else:
            self.fail("no exception thrown")

    def testCheckSourceNoConfig(self):
        """TestSessionAPIsDummy.testCheckSourceNoConfig -  test the right error is reported when the server doesn't exist """
        try:
            self.session.CheckSource("", utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.NoSuchSource: 'dummy-test' has no '' source")
        else:
            self.fail("no exception thrown")

    def testCheckSourceNoSourceName(self):
        """TestSessionAPIsDummy.testCheckSourceNoSourceName -  test the right error is reported when the source doesn't exist """
        self.setupConfig()
        try:
            self.session.CheckSource("dummy", utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.NoSuchSource: 'dummy-test' "
                                 "has no 'dummy' source")
        else:
            self.fail("no exception thrown")

    def testCheckSourceInvalidDatabase(self):
        """TestSessionAPIsDummy.testCheckSourceInvalidEvolutionSource -  test the right error is reported when the evolutionsource is invalid """
        self.setupConfig()
        config = { "source/memo" : { "database" : "impossible-source"} }
        self.session.SetConfig(True, False, config, utf8_strings=True)
        try:
            self.session.CheckSource("memo", utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.SourceUnusable: The source 'memo' is not usable")
        else:
            self.fail("no exception thrown")

    def testCheckSourceInvalidBackend(self):
        """TestSessionAPIsDummy.testCheckSourceInvalidBackend -  test the right error is reported when the type is invalid """
        self.setupConfig()
        config = { "source/memo" : { "backend" : "no-such-backend"} }
        try:
            self.session.SetConfig(True, False, config, utf8_strings=True)
        except dbus.DBusException, ex:
            expected = "org.syncevolution.InvalidCall: invalid value 'no-such-backend' for property 'backend': "
            self.assertEqual(str(ex)[0:len(expected)], expected)
        else:
            self.fail("no exception thrown")

    def testCheckSourceNoBackend(self):
        """TestSessionAPIsDummy.testCheckSourceNoBackend -  test the right error is reported when the source is unusable"""
        self.setupConfig()
        config = { "source/memo" : { "backend" : "file",
                                     "databaseFormat" : "text/calendar",
                                     "database" : "file:///no/such/path" } }
        self.session.SetConfig(True, False, config, utf8_strings=True)
        try:
            self.session.CheckSource("memo", utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.SourceUnusable: The source 'memo' is not usable")
        else:
            self.fail("no exception thrown")

    def testCheckSource(self):
        """TestSessionAPIsDummy.testCheckSource - testCheckSource - test all sources are okay"""
        self.setupConfig()
        try:
            for source in self.sources:
                self.session.CheckSource(source, utf8_strings=True)
        except dbus.DBusException, ex:
            self.fail(ex)

    def testCheckSourceUpdateConfigTemp(self):
        """TestSessionAPIsDummy.testCheckSourceUpdateConfigTemp -  test the config is temporary updated and in effect for GetDatabases in the current session. """
        self.setupConfig()
        tempConfig = {"source/temp" : { "backend" : "calendar"}}
        self.session.SetConfig(True, True, tempConfig, utf8_strings=True)
        databases2 = self.session.CheckSource("temp", utf8_strings=True)

    def testGetDatabasesNoConfig(self):
        """TestSessionAPIsDummy.testGetDatabasesNoConfig -  test the right error is reported when the server doesn't exist """
        # make sure the config doesn't exist """
        try:
            self.session.GetDatabases("", utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.NoSuchSource: 'dummy-test' has no '' source")
        else:
            self.fail("no exception thrown")

    def testGetDatabasesEmpty(self):
        """TestSessionAPIsDummy.testGetDatabasesEmpty -  test the right error is reported for non-existing source"""
        self.setupConfig()
        try:
            databases = self.session.GetDatabases("never_use_this_source_name", utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.NoSuchSource: 'dummy-test' has no 'never_use_this_source_name' source")
        else:
            self.fail("no exception thrown")

    def testGetDatabases(self):
        """TestSessionAPIsDummy.testGetDatabases -  test the right way to get databases """
        self.setupConfig()

        # don't know actual databases, so compare results of two different times
        sources = ['addressbook', 'calendar', 'todo', 'memo']
        databases1 = []
        for source in sources:
            databases1.append(self.session.GetDatabases(source, utf8_strings=True))
        # reverse the list of sources and get databases again
        sources.reverse()
        databases2 = []
        for source in sources:
            databases2.append(self.session.GetDatabases(source, utf8_strings=True))
        # sort two arrays
        databases1.sort()
        databases2.sort()
        self.assertEqual(databases1, databases2)

    def testGetDatabasesUpdateConfigTemp(self):
        """TestSessionAPIsDummy.testGetDatabasesUpdateConfigTemp -  test the config is temporary updated and in effect for GetDatabases in the current session. """
        self.setupConfig()
        # file backend: reports a short help text instead of a real database list
        databases1 = self.session.GetDatabases("calendar", utf8_strings=True)
        # databaseFormat is required for file backend, otherwise it
        # cannot be instantiated and even simple operations as reading
        # the (in this case fixed) list of databases fail
        tempConfig = {"source/temp" : { "backend" : "file", "databaseFormat" : "text/calendar" }}
        self.session.SetConfig(True, True, tempConfig, utf8_strings=True)
        databases2 = self.session.GetDatabases("temp", utf8_strings=True)
        self.assertEqual(databases2, databases1)

    def testGetReportsNoConfig(self):
        """TestSessionAPIsDummy.testGetReportsNoConfig -  Test nothing is gotten when the given server doesn't exist. Also covers boundaries """
        reports = self.session.GetReports(0, 0, utf8_strings=True)
        self.assertEqual(reports, [])
        reports = self.session.GetReports(0, 1, utf8_strings=True)
        self.assertEqual(reports, [])
        reports = self.session.GetReports(0, 0xFFFFFFFF, utf8_strings=True)
        self.assertEqual(reports, [])
        reports = self.session.GetReports(0xFFFFFFFF, 0xFFFFFFFF, utf8_strings=True)
        self.assertEqual(reports, [])

    def testGetReportsNoReports(self):
        """TestSessionAPIsDummy.testGetReportsNoReports -  Test when the given server has no reports. Also covers boundaries """
        self.setupConfig()
        reports = self.session.GetReports(0, 0, utf8_strings=True)
        self.assertEqual(reports, [])
        reports = self.session.GetReports(0, 1, utf8_strings=True)
        self.assertEqual(reports, [])
        reports = self.session.GetReports(0, 0xFFFFFFFF, utf8_strings=True)
        self.assertEqual(reports, [])
        reports = self.session.GetReports(0xFFFFFFFF, 0xFFFFFFFF, utf8_strings=True)
        self.assertEqual(reports, [])

    def testGetReportsByRef(self):
        """TestSessionAPIsDummy.testGetReportsByRef -  Test the reports are gotten correctly from reference files. Also covers boundaries """
        """ This could be extractly compared since the reference files are known """
        self.setUpFiles('reports')
        report0 = { "peer" : "dummy-test",
                    "start" : "1258520955",
                    "end" : "1258520964",
                    "status" : "200",
                    "source-addressbook-mode" : "slow",
                    "source-addressbook-first" : "true",
                    "source-addressbook-resume" : "false",
                    "source-addressbook-status" : "0",
                    "source-addressbook-backup-before" : "0",
                    "source-addressbook-backup-after" : "0",
                    "source-addressbook-stat-local-any-sent" : "9168",
                    "source-addressbook-stat-remote-added-total" : "71",
                    "source-addressbook-stat-remote-updated-total" : "100",
                    "source-addressbook-stat-local-updated-total" : "632",
                    "source-addressbook-stat-remote-any-reject" : "100",
                    "source-addressbook-stat-remote-any-conflict_duplicated" : "5293487",
                    "source-addressbook-stat-remote-any-conflict_client_won" : "33",
                    "source-addressbook-stat-local-any-received" : "2",
                    "source-addressbook-stat-local-removed-total" : "4",
                    "source-addressbook-stat-remote-any-conflict_server_won" : "38",
                    "source-addressbook-stat-local-any-reject" : "77",
                    "source-addressbook-stat-local-added-total" : "84",
                    "source-addressbook-stat-remote-removed-total" : "66",
                    "source-calendar-mode" : "slow",
                    "source-calendar-first" : "true",
                    "source-calendar-resume" : "false",
                    "source-calendar-status" : "0",
                    "source-calendar-backup-before" : "17",
                    "source-calendar-backup-after" : "17",
                    "source-calendar-stat-local-any-sent" : "8619",
                    "source-calendar-stat-remote-added-total": "17",
                    "source-calendar-stat-remote-updated-total" : "10",
                    "source-calendar-stat-local-updated-total" : "6",
                    "source-calendar-stat-remote-any-reject" : "1",
                    "source-calendar-stat-remote-any-conflict_duplicated" : "5",
                    "source-calendar-stat-remote-any-conflict_client_won" : "3",
                    "source-calendar-stat-local-any-received" : "24",
                    "source-calendar-stat-local-removed-total" : "54",
                    "source-calendar-stat-remote-any-conflict_server_won" : "38",
                    "source-calendar-stat-local-any-reject" : "7",
                    "source-calendar-stat-local-added-total" : "42",
                    "source-calendar-stat-remote-removed-total" : "6",
                    "source-memo-mode" : "slow",
                    "source-memo-first" : "true",
                    "source-memo-resume" : "false",
                    "source-memo-status" : "0",
                    "source-memo-backup-before" : "3",
                    "source-memo-backup-after" : "4",
                    "source-memo-stat-local-any-sent" : "8123",
                    "source-memo-stat-remote-added-total" : "15",
                    "source-memo-stat-remote-updated-total" : "6",
                    "source-memo-stat-local-updated-total" : "8",
                    "source-memo-stat-remote-any-reject" : "16",
                    "source-memo-stat-remote-any-conflict_duplicated" : "27",
                    "source-memo-stat-remote-any-conflict_client_won" : "2",
                    "source-memo-stat-local-any-received" : "3",
                    "source-memo-stat-local-removed-total" : "4",
                    "source-memo-stat-remote-any-conflict_server_won" : "8",
                    "source-memo-stat-local-any-reject" : "40",
                    "source-memo-stat-local-added-total" : "34",
                    "source-memo-stat-remote-removed-total" : "5",
                    "source-todo-mode" : "slow",
                    "source-todo-first" : "true",
                    "source-todo-resume" : "false",
                    "source-todo-status" : "0",
                    "source-todo-backup-before" : "2",
                    "source-todo-backup-after" : "2",
                    "source-todo-stat-local-any-sent" : "619",
                    "source-todo-stat-remote-added-total" : "71",
                    "source-todo-stat-remote-updated-total" : "1",
                    "source-todo-stat-local-updated-total" : "9",
                    "source-todo-stat-remote-any-reject" : "10",
                    "source-todo-stat-remote-any-conflict_duplicated" : "15",
                    "source-todo-stat-remote-any-conflict_client_won" : "7",
                    "source-todo-stat-local-any-received" : "2",
                    "source-todo-stat-local-removed-total" : "4",
                    "source-todo-stat-remote-any-conflict_server_won" : "8",
                    "source-todo-stat-local-any-reject" : "3",
                    "source-todo-stat-local-added-total" : "24",
                    "source-todo-stat-remote-removed-total" : "80" }
        reports = self.session.GetReports(0, 0, utf8_strings=True)
        self.assertEqual(reports, [])
        # get only one report
        reports = self.session.GetReports(0, 1, utf8_strings=True)
        self.assertTrue(len(reports) == 1)
        del reports[0]["dir"]

        self.assertEqual(reports[0], report0)
        """ the number of reference sessions is totally 5. Check the returned count
        when parameter is bigger than 5 """
        reports = self.session.GetReports(0, 0xFFFFFFFF, utf8_strings=True)
        self.assertTrue(len(reports) == 5)
        # start from 2, this could check integer overflow
        reports2 = self.session.GetReports(2, 0xFFFFFFFF, utf8_strings=True)
        self.assertTrue(len(reports2) == 3)
        # the first element of reports2 should be the same as the third element of reports
        self.assertEqual(reports[2], reports2[0])
        # indexed from 5, nothing could be gotten
        reports = self.session.GetReports(5, 0xFFFFFFFF, utf8_strings=True)
        self.assertEqual(reports, [])

    def testRestoreByRef(self):
        """TestSessionAPIsDummy.testRestoreByRef - restore data before or after a given session"""
        self.setUpFiles('restore')
        self.setupConfig()
        self.setUpListeners(self.sessionpath)
        reports = self.session.GetReports(0, 1, utf8_strings=True)
        dir = reports[0]["dir"]
        sessionpath, session = self.createSession("dummy-test", False)
        #TODO: check restore result, how?
        #restore data before this session
        self.session.Restore(dir, True, [], utf8_strings=True)
        loop.run()
        self.session.Detach()

        # check recorded events in DBusUtil.events, first filter them
        statuses = []
        progresses = []
        for item in DBusUtil.events:
            if item[0] == "status":
                statuses.append(item[1])
            elif item[0] == "progress":
                progresses.append(item[1])

        lastStatus = ""
        lastSources = {}
        statusPairs = {"": 0, "idle": 1, "running" : 2, "done" : 3}
        for status, error, sources in statuses:
            self.assertFalse(status == lastStatus and lastSources == sources)
            # no error
            self.assertEqual(error, 0)
            for sourcename, value in sources.items():
                # no error
                self.assertEqual(value[2], 0)
                # keep order: source status must also be unchanged or the next status
                if lastSources.has_key(sourcename):
                    lastValue = lastSources[sourcename]
                    self.assertTrue(statusPairs[value[1]] >= statusPairs[lastValue[1]])

            lastStatus = status
            lastSources = sources

        # check increasing progress percentage
        lastPercent = 0
        for percent, sources in progresses:
            self.assertFalse(percent < lastPercent)
            lastPercent = percent

        session.SetConfig(False, False, self.config, utf8_strings=True)
        #restore data after this session
        session.Restore(dir, False, ["addressbook", "calendar"], utf8_strings=True)
        loop.run()

    def testSecondRestore(self):
        """TestSessionAPIsDummy.testSecondRestore - right error thrown when session is not active?"""
        self.setUpFiles('restore')
        self.setupConfig()
        self.setUpListeners(self.sessionpath)
        reports = self.session.GetReports(0, 1, utf8_strings=True)
        dir = reports[0]["dir"]
        sessionpath, session = self.createSession("dummy-test", False)
        try:
            session.Restore(dir, False, [], utf8_strings=True)
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                    "org.syncevolution.InvalidCall: session is not active, call not allowed at this time")
        else:
            self.fail("no exception thrown")

        self.session.Detach()
        session.SetConfig(False, False, self.config, utf8_strings=True)
        session.Restore(dir, False, [], utf8_strings=True)
        loop.run()

    @timeout(300)
    def testInteractivePassword(self):
        """TestSessionAPIsDummy.testInteractivePassword -  test the info request is correctly working for password """
        self.setupConfig()
        self.setUpListeners(self.sessionpath)
        self.lastState = "unknown"
        # define callback for InfoRequest signals and send corresponds response
        # to dbus server
        def infoRequest(id, session, state, handler, type, params):
            if state == "request":
                self.assertEqual(self.lastState, "unknown")
                self.lastState = "request"
                self.server.InfoResponse(id, "working", {}, utf8_strings=True)
            elif state == "waiting":
                self.assertEqual(self.lastState, "request")
                self.lastState = "waiting"
                self.server.InfoResponse(id, "response", {"password" : "123456"}, utf8_strings=True)
            elif state == "done":
                self.assertEqual(self.lastState, "waiting")
                self.lastState = "done"
            else:
                self.fail("state should not be '" + state + "'")

        signal = bus.add_signal_receiver(infoRequest,
                                         'InfoRequest',
                                         'org.syncevolution.Server',
                                         self.server.bus_name,
                                         None,
                                         byte_arrays=True,
                                         utf8_strings=True)

        # dbus server will be blocked by gnome-keyring-ask dialog, so we kill it, and then 
        # it can't get the password from gnome keyring and send info request for password
        def callback():
            kill = subprocess.Popen("sh -c 'killall -9 gnome-keyring-ask >/dev/null 2>&1'", shell=True)
            kill.communicate()
            return True

        timeout_handler = Timeout.addTimeout(1, callback)

        # try to sync and invoke password request
        self.session.Sync("", {})
        loop.run()
        Timeout.removeTimeout(timeout_handler)
        self.assertEqual(self.lastState, "done")

    @timeout(60)
    def testAutoSyncNetworkFailure(self):
        """TestSessionAPIsDummy.testAutoSyncNetworkFailure - test that auto-sync is triggered, fails due to (temporary?!) network error here"""
        self.setupConfig()
        # enable auto-sync
        config = copy.deepcopy(self.config)
        # Note that writing this config will modify the host's keyring!
        # Use a syncURL that is unlikely to conflict with the host
        # or any other D-Bus test.
        config[""]["syncURL"] = "http://no-such-domain.foobar"
        config[""]["autoSync"] = "1"
        config[""]["autoSyncDelay"] = "0"
        config[""]["autoSyncInterval"] = "10s"
        config[""]["password"] = "foobar"
        self.session.SetConfig(True, False, config, utf8_strings=True)

        def session_ready(object, ready):
            if self.running and object != self.sessionpath and \
                (self.auto_sync_session_path == None and ready or \
                 self.auto_sync_session_path == object):
                self.auto_sync_session_path = object
                DBusUtil.quit_events.append("session " + object + (ready and " ready" or " done"))
                loop.quit()

        signal = bus.add_signal_receiver(session_ready,
                                         'SessionChanged',
                                         'org.syncevolution.Server',
                                         self.server.bus_name,
                                         None,
                                         byte_arrays=True,
                                         utf8_strings=True)

        # shut down current session, will allow auto-sync
        self.session.Detach()

        # wait for start and end of auto-sync session
        loop.run()
        start1 = time.time()
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["session " + self.auto_sync_session_path + " ready",
                                                "session " + self.auto_sync_session_path + " done"])
        DBusUtil.quit_events = []
        # session must be around for a while after terminating, to allow
        # reading information about it by clients who didn't start it
        # and thus wouldn't know what the session was about otherwise
        session = dbus.Interface(bus.get_object(self.server.bus_name,
                                                self.auto_sync_session_path),
                                 'org.syncevolution.Session')
        reports = session.GetReports(0, 100, utf8_strings=True)
        self.assertEqual(len(reports), 1)
        self.assertEqual(reports[0]["status"], "20043")
        name = session.GetConfigName()
        self.assertEqual(name, "dummy-test")
        flags = session.GetFlags()
        self.assertEqual(flags, [])
        first_auto = self.auto_sync_session_path
        self.auto_sync_session_path = None

        # check that interval between auto-sync sessions is right
        loop.run()
        start2 = time.time()
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["session " + self.auto_sync_session_path + " ready",
                                                "session " + self.auto_sync_session_path + " done"])
        self.assertNotEqual(first_auto, self.auto_sync_session_path)
        delta = start2 - start1
        # avoid timing checks when running under valgrind
        if not usingValgrind():
            self.assertTrue(delta < 13)
            self.assertTrue(delta > 7)

        # check that org.freedesktop.Notifications.Notify was not called
        # (network errors are considered temporary, can't tell in this case
        # that the name lookup error is permanent)
        def checkDBusLog(self, content):
            notifications = GrepNotifications(content)
            self.assertEqual(notifications, [])

        # done as part of post-processing in runTest()
        self.runTestDBusCheck = checkDBusLog

    @timeout(60)
    def testAutoSyncLocalConfigError(self):
        """TestSessionAPIsDummy.testAutoSyncLocalConfigError - test that auto-sync is triggered for local sync, fails due to permanent config error here"""
        self.setupConfig()
        # enable auto-sync
        config = copy.deepcopy(self.config)
        config[""]["syncURL"] = "local://@foobar" # will fail
        config[""]["autoSync"] = "1"
        config[""]["autoSyncDelay"] = "0"
        config[""]["autoSyncInterval"] = "10s"
        config[""]["password"] = "foobar"
        self.session.SetConfig(True, False, config, utf8_strings=True)

        def session_ready(object, ready):
            if self.running and object != self.sessionpath and \
                (self.auto_sync_session_path == None and ready or \
                 self.auto_sync_session_path == object):
                self.auto_sync_session_path = object
                DBusUtil.quit_events.append("session " + object + (ready and " ready" or " done"))
                loop.quit()

        signal = bus.add_signal_receiver(session_ready,
                                         'SessionChanged',
                                         'org.syncevolution.Server',
                                         self.server.bus_name,
                                         None,
                                         byte_arrays=True,
                                         utf8_strings=True)

        # shut down current session, will allow auto-sync
        self.session.Detach()

        # wait for start and end of auto-sync session
        loop.run()
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["session " + self.auto_sync_session_path + " ready",
                                                "session " + self.auto_sync_session_path + " done"])
        session = dbus.Interface(bus.get_object(self.server.bus_name,
                                                self.auto_sync_session_path),
                                 'org.syncevolution.Session')
        reports = session.GetReports(0, 100, utf8_strings=True)
        self.assertEqual(len(reports), 1)
        self.assertEqual(reports[0]["status"], "10500")
        name = session.GetConfigName()
        self.assertEqual(name, "dummy-test")
        flags = session.GetFlags()
        self.assertEqual(flags, [])

        # check that org.freedesktop.Notifications.Notify was called
        # once to report the failed attempt to start the sync
        def checkDBusLog(self, content):
            notifications = GrepNotifications(content)
            self.assertEqual(notifications,
                             ['   string "SyncEvolution"\n'
                              '   uint32 0\n'
                              '   string ""\n'
                              '   string "Sync problem."\n'
                              '   string "Sorry, there\'s a problem with your sync that you need to attend to."\n'
                              '   array [\n'
                              '      string "view"\n'
                              '      string "View"\n'
                              '      string "default"\n'
                              '      string "Dismiss"\n'
                              '   ]\n'
                              '   array [\n'
                              '   ]\n'
                              '   int32 -1\n'])

        # check that no other session is started at the time of
        # the next regular auto sync session
        def testDone():
            DBusUtil.quit_events.append("test done")
            loop.quit()
            return False
        def any_session_ready(object, ready):
            if self.running:
                DBusUtil.quit_events.append("session " + object + (ready and " ready" or " done"))
                loop.quit()
        signal = bus.add_signal_receiver(any_session_ready,
                                         'SessionChanged',
                                         'org.syncevolution.Server',
                                         self.server.bus_name,
                                         None,
                                         byte_arrays=True,
                                         utf8_strings=True)

        try:
            timeout = glib.timeout_add(15 * 1000, testDone)
            loop.run()
        finally:
            glib.source_remove(timeout)
        self.assertEqual(DBusUtil.quit_events, ["session " + self.auto_sync_session_path + " ready",
                                                "session " + self.auto_sync_session_path + " done",
                                                "test done"])

        # done as part of post-processing in runTest()
        self.runTestDBusCheck = checkDBusLog

    @timeout(120)
    def testAutoSyncLocalSuccess(self):
        """TestSessionAPIsDummy.testAutoSyncLocalSuccess - test that auto-sync is done successfully for local sync between file backends"""
        # create @foobar config
        self.session.Detach()
        self.setUpSession("target-config@foobar")
        config = copy.deepcopy(self.config)
        config[""]["remoteDeviceId"] = "foo"
        config[""]["deviceId"] = "bar"
        del config[""]["password"]
        for i in ("addressbook", "calendar", "todo", "memo"):
            source = config["source/" + i]
            source["database"] = source["database"] + ".server"
        self.session.SetConfig(False, False, config, utf8_strings=True)
        self.session.Detach()

        # create dummy-test@default auto-sync config
        self.setUpSession("dummy-test")
        config = copy.deepcopy(self.config)
        config[""]["syncURL"] = "local://@foobar"
        config[""]["PeerIsClient"] = "1"
        config[""]["autoSync"] = "1"
        config[""]["autoSyncDelay"] = "0"
        del config[""]["password"]
        # must be small enough (otherwise test runs a long time)
        # but not too small (otherwise the next sync already starts
        # before we can check the result and kill the daemon)
        config[""]["autoSyncInterval"] = usingValgrind() and "60s" or "10s"
        config["source/addressbook"]["uri"] = "addressbook"
        self.session.SetConfig(False, False, config, utf8_strings=True)

        def session_ready(object, ready):
            if self.running and object != self.sessionpath and \
                (self.auto_sync_session_path == None and ready or \
                 self.auto_sync_session_path == object):
                self.auto_sync_session_path = object
                DBusUtil.quit_events.append("session " + object + (ready and " ready" or " done"))
                loop.quit()

        signal = bus.add_signal_receiver(session_ready,
                                         'SessionChanged',
                                         'org.syncevolution.Server',
                                         self.server.bus_name,
                                         None,
                                         byte_arrays=True,
                                         utf8_strings=True)

        # shut down current session, will allow auto-sync
        self.session.Detach()

        # wait for start and end of auto-sync session
        loop.run()
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["session " + self.auto_sync_session_path + " ready",
                                                "session " + self.auto_sync_session_path + " done"])
        session = dbus.Interface(bus.get_object(self.server.bus_name,
                                                self.auto_sync_session_path),
                                 'org.syncevolution.Session')
        reports = session.GetReports(0, 100, utf8_strings=True)
        self.assertEqual(len(reports), 1)
        self.assertEqual(reports[0]["status"], "200")
        name = session.GetConfigName()
        self.assertEqual(name, "dummy-test")
        flags = session.GetFlags()
        self.assertEqual(flags, [])

        # check that org.freedesktop.Notifications.Notify was called
        # when starting and completing the sync
        def checkDBusLog(self, content):
            notifications = GrepNotifications(content)
            self.assertEqual(notifications,
                             ['   string "SyncEvolution"\n'
                              '   uint32 0\n'
                              '   string ""\n'
                              '   string "dummy-test is syncing"\n'
                              '   string "We have just started to sync your computer with the dummy-test sync service."\n'
                              '   array [\n'
                              '      string "view"\n'
                              '      string "View"\n'
                              '      string "default"\n'
                              '      string "Dismiss"\n'
                              '   ]\n'
                              '   array [\n'
                              '   ]\n'
                              '   int32 -1\n',

                              '   string "SyncEvolution"\n'
                              '   uint32 0\n'
                              '   string ""\n'
                              '   string "dummy-test sync complete"\n'
                              '   string "We have just finished syncing your computer with the dummy-test sync service."\n'
                              '   array [\n'
                              '      string "view"\n'
                              '      string "View"\n'
                              '      string "default"\n'
                              '      string "Dismiss"\n'
                              '   ]\n'
                              '   array [\n'
                              '   ]\n'
                              '   int32 -1\n'])

        # done as part of post-processing in runTest()
        self.runTestDBusCheck = checkDBusLog


class TestSessionAPIsReal(DBusUtil, unittest.TestCase):
    """ This class is used to test those unit tests of session APIs, depending on doing sync.
        Thus we need a real server configuration to confirm sync could be run successfully.
        Typically we need make sure that at least one sync has been done before testing our
        desired unit tests. Note that it also covers session.Sync API itself """
    """ All unit tests in this class have a dependency on a real sync
    config named 'dbus_unittest'. That config must have preventSlowSync=0,
    maxLogDirs=1, username, password set such that syncing succeeds
    for at least one source. It does not matter which data is synchronized.
    For example, the following config will work:
    syncevolution --configure --template <server of your choice> \
                  username=<your username> \
                  password=<your password> \
                  preventSlowSync=0 \
                  maxLogDirs=1 \
                  backend=file \
                  database=file:///tmp/test_dbus_data \
                  databaseFormat=text/vcard \
                  dbus_unittest@test-dbus addressbook
                  """

    def setUp(self):
        self.setUpServer()
        self.setUpSession(configName)
        self.operation = "" 

    def run(self, result):
        self.runTest(result, own_xdg=False)

    def setupConfig(self):
        """ Apply for user settings. Used internally. """
        configProps = { }
        # check whether 'dbus_unittest' is configured.
        try:
            configProps = self.session.GetConfig(False, utf8_strings=True)
        except dbus.DBusException, ex:
            self.fail(str(ex) + 
                      ". To test this case, please first set up a correct config named 'dbus_unittest'.")

    def doSync(self):
        self.setupConfig()
        self.setUpListeners(self.sessionpath)
        self.session.Sync("slow", {})
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["session " + self.sessionpath + " done"])

    def progressChanged(self, *args, **keywords):
        '''abort or suspend once session has progressed far enough'''
        percentage = args[0]
        # make sure sync is really running
        if percentage > 20:
            if self.operation == "abort":
                self.session.Abort()
                self.operation = "aborted"
            if self.operation == "suspend":
                self.session.Suspend()
                self.operation = "suspended"

    @timeout(300)
    def testSync(self):
        """TestSessionAPIsReal.testSync - run a real sync with default server and test status list and progress number"""
        """ check events list is correct for StatusChanged and ProgressChanged """
        # do sync
        self.doSync()
        self.checkSync()
    
    @timeout(300)
    def testSyncStatusAbort(self):
        """TestSessionAPIsReal.testSyncStatusAbort -  test status is set correctly when the session is aborted """
        self.operation = "abort"
        self.doSync()
        hasAbortingStatus = False
        for item in DBusUtil.events:
            if item[0] == "status" and item[1][0] == "aborting":
                hasAbortingStatus = True
                break
        self.assertEqual(hasAbortingStatus, True)

    @timeout(300)
    def testSyncStatusSuspend(self):
        """TestSessionAPIsReal.testSyncStatusSuspend -  test status is set correctly when the session is suspended """
        self.operation = "suspend"
        self.doSync()
        hasSuspendingStatus = False
        for item in DBusUtil.events:
            if item[0] == "status" and "suspending" in item[1][0] :
                hasSuspendingStatus = True
                break
        self.assertEqual(hasSuspendingStatus, True)

    @timeout(300)
    def testSyncSecondSession(self):
        """TestSessionAPIsReal.testSyncSecondSession - ask for a second session that becomes ready after a real sync"""
        sessionpath2, session2 = self.createSession("", False)
        status, error, sources = session2.GetStatus(utf8_strings=True)
        self.assertEqual(status, "queueing")
        self.testSync()
        # now wait for second session becoming ready
        loop.run()
        status, error, sources = session2.GetStatus(utf8_strings=True)
        self.assertEqual(status, "idle")
        self.assertEqual(DBusUtil.quit_events, ["session " + self.sessionpath + " done",
                                                    "session " + sessionpath2 + " ready"])
        session2.Detach()

class TestDBusSyncError(DBusUtil, unittest.TestCase):
    def setUp(self):
        self.setUpServer()
        self.setUpSession(configName)

    def run(self, result):
        self.runTest(result, own_xdg=True)

    def testSyncNoConfig(self):
        """TestDBusSyncError.testSyncNoConfig - Executes a real sync with no corresponding config."""
        self.setUpListeners(self.sessionpath)
        self.session.Sync("", {})
        loop.run()
        # TODO: check recorded events in DBusUtil.events
        status, error, sources = self.session.GetStatus(utf8_strings=True)
        self.assertEqual(status, "done")
        self.assertEqual(error, 10500)

class TestConnection(DBusUtil, unittest.TestCase):
    """Tests Server.Connect(). Tests depend on getting one Abort signal to terminate."""

    """a real message sent to our own server, DevInf stripped, username/password foo/bar"""
    message1 = '''<?xml version="1.0" encoding="UTF-8"?><SyncML xmlns='SYNCML:SYNCML1.2'><SyncHdr><VerDTD>1.2</VerDTD><VerProto>SyncML/1.2</VerProto><SessionID>255</SessionID><MsgID>1</MsgID><Target><LocURI>http://127.0.0.1:9000/syncevolution</LocURI></Target><Source><LocURI>sc-api-nat</LocURI><LocName>test</LocName></Source><Cred><Meta><Format xmlns='syncml:metinf'>b64</Format><Type xmlns='syncml:metinf'>syncml:auth-md5</Type></Meta><Data>kHzMn3RWFGWSKeBpXicppQ==</Data></Cred><Meta><MaxMsgSize xmlns='syncml:metinf'>20000</MaxMsgSize><MaxObjSize xmlns='syncml:metinf'>4000000</MaxObjSize></Meta></SyncHdr><SyncBody><Alert><CmdID>1</CmdID><Data>200</Data><Item><Target><LocURI>addressbook</LocURI></Target><Source><LocURI>./addressbook</LocURI></Source><Meta><Anchor xmlns='syncml:metinf'><Last>20091105T092757Z</Last><Next>20091105T092831Z</Next></Anchor><MaxObjSize xmlns='syncml:metinf'>4000000</MaxObjSize></Meta></Item></Alert><Final/></SyncBody></SyncML>'''

    """a real WBXML message, expected to trigger an authentication failure"""
    message1WBXML = base64.b64decode('AqQBagBtbHEDMS4yAAFyA1N5bmNNTC8xLjIAAWUDMjExAAFbAzEAAW5XA2h0dHA6Ly9teS5mdW5hbWJvbC5jb20vc3luYwABAWdXA3NjLWFwaS1uYXQAAVYDcGF0cmljay5vaGx5AAEBTloAAUcDYjY0AAFTA3N5bmNtbDphdXRoLW1kNQABAQAATwNyT0dFbGR1Y2FjNE5mc3dZSm5lR2lnPT0AAQFaAAFMAzE1MDAwMAABVQM0MDAwMDAwAAEBAQAAa19LAzEAAVoAAVMDYXBwbGljYXRpb24vdm5kLnN5bmNtbC1kZXZpbmYrd2J4bWwAAQEAAFRnVwMuL2RldmluZjEyAAEBT8OMewKkA2oASmUDMS4yAAFRA1BhdHJpY2sgT2hseQABVQNTeW5jRXZvbHV0aW9uAAFWA1N5bnRoZXNpcyBBRwABTwMxLjIuOTkrMjAxMjAzMjcrU0UrMjI1MGVhMCt1bmNsZWFuAAFeAzMuNC4wLjM1AAFQA3Vua25vd24AAUkDc2MtYXBpLW5hdAABSwN3b3Jrc3RhdGlvbgABKCkqR10DLi9lZHNfZXZlbnQAAUwDZWRzX2V2ZW50AAFSAzY0AAFaRgN0ZXh0L2NhbGVuZGFyAAFkAzIuMAABAWJGA3RleHQvY2FsZW5kYXIAAWQDMi4wAAEBRUYDdGV4dC9jYWxlbmRhcgABZAMyLjAAAWtYA0JFR0lOAAFjA1ZDQUxFTkRBUgABYwNWVElNRVpPTkUAAWMDU1RBTkRBUkQAAWMDREFZTElHSFQAAWMDVlRPRE8AAWMDVkFMQVJNAAFjA1ZFVkVOVAABYwNWQUxBUk0AAQFrWANFTkQAAWMDVkNBTEVOREFSAAFjA1ZUSU1FWk9ORQABYwNTVEFOREFSRAABYwNEQVlMSUdIVAABYwNWVE9ETwABYwNWQUxBUk0AAWMDVkVWRU5UAAFjA1ZBTEFSTQABAWtYA1ZFUlNJT04AAWMDMi4wAAFtAzEAAQFrWANQUk9ESUQAAW0DMQABAWtYA1RaSUQAAQFrWANEVFNUQVJUAAEBa1gDUlJVTEUAAQFrWANUWk9GRlNFVEZST00AAQFrWANUWk9GRlNFVFRPAAEBa1gDVFpOQU1FAAEBa1gDTEFTVC1NT0RJRklFRAABbQMxAAEBa1gDRFRTVEFNUAABbQMxAAEBa1gDQ1JFQVRFRAABbQMxAAEBa1gDVUlEAAFtAzEAAQFrWANTRVFVRU5DRQABbQMxAAEBa1gDR0VPAAFtAzEAAQFrWANDQVRFR09SSUVTAAEBa1gDQ0xBU1MAAW0DMQABAWtYA1NVTU1BUlkAAW0DMQABAWtYA0RFU0NSSVBUSU9OAAFtAzEAAQFrWANMT0NBVElPTgABbQMxAAEBa1gDVVJMAAFtAzEAAQFrWANDT01QTEVURUQAAW0DMQABbFcDVFpJRAABAWxXA1ZBTFVFAAEBAWtYA0RVRQABbQMxAAFsVwNUWklEAAEBbFcDVkFMVUUAAQEBa1gDUFJJT1JJVFkAAW0DMQABAWtYA1NUQVRVUwABbQMxAAEBa1gDUEVSQ0VOVC1DT01QTEVURQABbQMxAAEBa1gDUkVMQVRFRC1UTwABbQMxAAFsVwNSRUxUWVBFAAFjA1BBUkVOVAABAQFrWANUUklHR0VSAAFtAzEAAWxXA1ZBTFVFAAEBbFcDUkVMQVRFRAABYwNTVEFSVAABYwNFTkQAAQEBa1gDQUNUSU9OAAFtAzEAAQFrWANSRVBFQVQAAW0DMQABAWtYA1gtRVZPTFVUSU9OLUFMQVJNLVVJRAABbQMxAAEBa1gDVFoAAW0DMQABAWtYA1RSQU5TUAABbQMxAAEBa1gDUkVDVVJSRU5DRS1JRAABbQMxAAFsVwNUWklEAAEBbFcDVkFMVUUAAQEBa1gDRVhEQVRFAAEBa1gDWC1TWU5DRVZPTFVUSU9OLUVYREFURS1ERVRBQ0hFRAABbFcDVFpJRAABAQFrWANEVEVORAABbQMxAAFsVwNUWklEAAEBbFcDVkFMVUUAAQEBa1gDRFVSQVRJT04AAW0DMQABAWtYA0FUVEVOREVFAAFsVwNDTgABAWxXA1BBUlRTVEFUAAFjA05FRURTLUFDVElPTgABYwNBQ0NFUFRFRAABYwNERUNMSU5FRAABYwNURU5UQVRJVkUAAWMDREVMRUdBVEVEAAEBbFcDUk9MRQABYwNDSEFJUgABYwNSRVEtUEFSVElDSVBBTlQAAWMDT1BULVBBUlRJQ0lQQU5UAAFjA05PTi1QQVJUSUNJUEFOVAABAWxXA1JTVlAAAWMDVFJVRQABYwNGQUxTRQABAWxXA0xBTkdVQUdFAAEBbFcDQ1VUWVBFAAFjA0lORElWSURVQUwAAWMDR1JPVVAAAWMDUkVTT1VSQ0UAAWMDUk9PTQABYwNVTktOT1dOAAEBAWtYA09SR0FOSVpFUgABbQMxAAFsVwNDTgABAQEBX2ADMQABYAMyAAFgAzMAAWADNAABYAM1AAFgAzYAAWADNwABYAM1NDQwMDEAAQEBAQEBAVNLAzIAAVoAAVMDYXBwbGljYXRpb24vdm5kLnN5bmNtbC1kZXZpbmYrd2J4bWwAAQEAAFRuVwMuL2RldmluZjEyAAEBAQFGSwMzAAFPAzIwMAABVG5XA2V2ZW50AAEBZ1cDLi9lZHNfZXZlbnQAAQFaAAFFSgMyMDEyMDMyOVQxMTAxMjZaAAFPAzIwMTIwMzI5VDExMDE1MloAAQFVAzQwMDAwMDAAAQEBAQAAEgEB')

    def setUp(self):
        self.setUpServer()
        self.setUpListeners(None)
        # default config
        self.config = { 
                         "" : { "remoteDeviceId" : "sc-api-nat",
                                "password" : "test",
                                "username" : "test",
                                "PeerIsClient" : "1",
                                "RetryInterval" : "1",
                                "RetryDuration" : "10"
                              },
                         "source/addressbook" : { "sync" : "slow",
                                                  "backend" : "file",
                                                  "database" : "file://" + xdg_root + "/addressbook",
                                                  "databaseFormat" : "text/vcard",
                                                  "uri" : "card"
                                                },
                         "source/calendar"    : { "sync" : "disabled",
                                                  "backend" : "file",
                                                  "database" : "file://" + xdg_root + "/calendar",
                                                  "databaseFormat" : "text/calendar",
                                                  "uri" : "cal"
                                                },
                         "source/todo"        : { "sync" : "disabled",
                                                  "backend" : "file",
                                                  "database" : "file://" + xdg_root + "/todo",
                                                  "databaseFormat" : "text/calendar",
                                                  "uri" : "task"
                                                },
                         "source/memo"        : { "sync" : "disabled",
                                                  "backend" : "file",
                                                  "database" : "file://" + xdg_root + "/memo",
                                                  "databaseFormat" : "text/calendar",
                                                  "uri" : "text"
                                                }
                       }

    def setupConfig(self, name="dummy-test", deviceId="sc-api-nat"):
        self.setUpSession(name)
        self.config[""]["remoteDeviceId"] = deviceId
        self.session.SetConfig(False, False, self.config, utf8_strings=True)
        self.session.Detach()
        # SyncEvolution <= 1.2.2 delayed the "Session.StatusChanged"
        # "done" signal. The correct behavior is to send that
        # important change right away.
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["session done"])
        DBusUtil.quit_events = []

    def run(self, result):
        self.runTest(result, own_xdg=True)

    def getConnection(self, must_authenticate=False):
        conpath = self.server.Connect({'description': 'test-dbus.py',
                                       'transport': 'dummy'},
                                      must_authenticate,
                                      "")
        self.setUpConnectionListeners(conpath)
        connection = dbus.Interface(bus.get_object(self.server.bus_name,
                                                   conpath),
                                    'org.syncevolution.Connection')
        return (conpath, connection)

    def testConnect(self):
        """TestConnection.testConnect - get connection and close it"""
        conpath, connection = self.getConnection()
        connection.Close(False, 'good bye')
        loop.run()
        self.assertEqual(DBusUtil.events, [('abort',)])

    def testInvalidConnect(self):
        """TestConnection.testInvalidConnect - get connection, send invalid initial message"""
        self.setupConfig()
        conpath, connection = self.getConnection()
        try:
            connection.Process('1234', 'invalid message type')
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                                 "org.syncevolution.Exception: message type 'invalid message type' not supported for starting a sync")
        else:
            self.fail("no exception thrown")
        loop.run()
        # 'idle' status doesn't be checked
        self.assertIn(('abort',), DBusUtil.events)

    @timeout(60)
    def testStartSync(self):
        """TestConnection.testStartSync - send a valid initial SyncML message"""
        self.setupConfig()
        conpath, connection = self.getConnection()
        connection.Process(TestConnection.message1, 'application/vnd.syncml+xml')
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath + " got reply"])
        DBusUtil.quit_events = []
        # TODO: check events
        self.assertNotEqual(DBusUtil.reply, None)
        self.assertEqual(DBusUtil.reply[1], 'application/vnd.syncml+xml')
        # credentials should have been accepted because must_authenticate=False
        # in Connect(); 508 = "refresh required" is normal
        self.assertIn('<Status><CmdID>2</CmdID><MsgRef>1</MsgRef><CmdRef>1</CmdRef><Cmd>Alert</Cmd><TargetRef>addressbook</TargetRef><SourceRef>./addressbook</SourceRef><Data>508</Data>', DBusUtil.reply[0])
        self.assertNotIn('<Chal>', DBusUtil.reply[0])
        self.assertEqual(DBusUtil.reply[3], False)
        self.assertNotEqual(DBusUtil.reply[4], '')
        connection.Close(False, 'good bye')
        loop.run()
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath + " aborted",
                                                    "session done"])
        # start another session for the server (ensures that the previous one is done),
        # then check the server side report
        DBusUtil.quit_events = []
        self.setUpSession("dummy-test")
        sessions = self.session.GetReports(0, 100)
        self.assertEqual(len(sessions), 1)
        # transport failure, only addressbook active and later aborted
        self.assertEqual(sessions[0]["status"], "20043")
        # Exact error string doesn't matter much, it is mostly internal.
        if sessions[0]["error"] != "D-Bus peer has disconnected": # old error
            self.assertEqual(sessions[0]["error"], "transport problem: send() on connection which is not ready")
        self.assertEqual(sessions[0]["source-addressbook-status"], "20017")
        # The other three sources are disabled and should not be listed in the
        # report. Used to be listed with status 0 in the past, which would also
        # be acceptable, but here we use the strict check for "not present" to
        # ensure that the current behavior is preserved.
        self.assertNotIn("source-calendar-status", sessions[0])
        self.assertNotIn("source-todo-status", sessions[0])
        self.assertNotIn("source-memo-status", sessions[0])

    @timeout(60)
    def testCredentialsWrong(self):
        """TestConnection.testCredentialsWrong - send invalid credentials"""
        self.setupConfig()
        conpath, connection = self.getConnection(must_authenticate=True)
        connection.Process(TestConnection.message1, 'application/vnd.syncml+xml')
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath + " got reply"])
        DBusUtil.quit_events = []
        # TODO: check events
        self.assertNotEqual(DBusUtil.reply, None)
        self.assertEqual(DBusUtil.reply[1], 'application/vnd.syncml+xml')
        # credentials should have been rejected because of wrong Nonce
        self.assertIn('<Chal>', DBusUtil.reply[0])
        self.assertEqual(DBusUtil.reply[3], False)
        self.assertNotEqual(DBusUtil.reply[4], '')
        # When the login fails, the server also ends the session and the connection;
        # no more Connection.close() possible (could fail, depending on the timing).
        loop.run()
        loop.run()
        DBusUtil.quit_events.sort()
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath + " aborted",
                                                    "session done"])

    @timeout(60)
    def testCredentialsWrongWBXML(self):
        """TestConnection.testCredentialsWrongWBXML - send invalid credentials using WBXML"""
        self.setupConfig()
        conpath, connection = self.getConnection(must_authenticate=True)
        connection.Process(TestConnection.message1WBXML, 'application/vnd.syncml+wbxml')
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath + " got reply"])
        DBusUtil.quit_events = []
        # TODO: check events
        self.assertNotEqual(DBusUtil.reply, None)
        self.assertEqual(DBusUtil.reply[1], 'application/vnd.syncml+wbxml')
        # Credentials should have been rejected because of wrong Nonce.
        # Impossible to check with WBXML...
        # self.assertTrue('<Chal>' in DBusUtil.reply[0])
        self.assertEqual(DBusUtil.reply[3], False)
        self.assertNotEqual(DBusUtil.reply[4], '')
        loop.run()
        loop.run()
        DBusUtil.quit_events.sort()
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath + " aborted",
                                                "session done"])

    @timeout(60)
    def testCredentialsRight(self):
        """TestConnection.testCredentialsRight - send correct credentials"""
        self.setupConfig()
        conpath, connection = self.getConnection(must_authenticate=True)
        plain_auth = TestConnection.message1.replace("<Type xmlns='syncml:metinf'>syncml:auth-md5</Type></Meta><Data>kHzMn3RWFGWSKeBpXicppQ==</Data>",
                                                     "<Type xmlns='syncml:metinf'>syncml:auth-basic</Type></Meta><Data>dGVzdDp0ZXN0</Data>")
        connection.Process(plain_auth, 'application/vnd.syncml+xml')
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath + " got reply"])
        DBusUtil.quit_events = []
        self.assertNotEqual(DBusUtil.reply, None)
        self.assertEqual(DBusUtil.reply[1], 'application/vnd.syncml+xml')
        # credentials should have been accepted because with basic auth,
        # credentials can be replayed; 508 = "refresh required" is normal
        self.assertIn('<Status><CmdID>2</CmdID><MsgRef>1</MsgRef><CmdRef>1</CmdRef><Cmd>Alert</Cmd><TargetRef>addressbook</TargetRef><SourceRef>./addressbook</SourceRef><Data>508</Data>', DBusUtil.reply[0])
        self.assertEqual(DBusUtil.reply[3], False)
        self.assertNotEqual(DBusUtil.reply[4], '')
        connection.Close(False, 'good bye')
        loop.run()
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath + " aborted",
                                                    "session done"])

    @timeout(60)
    def testStartSyncTwice(self):
        """TestConnection.testStartSyncTwice - send the same SyncML message twice, starting two sessions"""
        self.setupConfig()
        conpath, connection = self.getConnection()
        connection.Process(TestConnection.message1, 'application/vnd.syncml+xml')
        loop.run()
        # TODO: check events
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath + " got reply"])
        self.assertNotEqual(DBusUtil.reply, None)
        self.assertEqual(DBusUtil.reply[1], 'application/vnd.syncml+xml')
        self.assertEqual(DBusUtil.reply[3], False)
        self.assertNotEqual(DBusUtil.reply[4], '')
        DBusUtil.reply = None
        DBusUtil.quit_events = []

        # Now start another session with the same client *without*
        # closing the first one. The server should detect this
        # and forcefully close the first one.
        conpath2, connection2 = self.getConnection()
        connection2.Process(TestConnection.message1, 'application/vnd.syncml+xml')

        # reasons for leaving the loop, in random order:
        # - abort of first connection
        # - first session done
        # - reply for second one
        loop.run()
        loop.run()
        loop.run()
        DBusUtil.quit_events.sort()
        expected = [ "connection " + conpath + " aborted",
                     "session done",
                     "connection " + conpath2 + " got reply" ]
        expected.sort()
        self.assertEqual(DBusUtil.quit_events, expected)
        self.assertNotEqual(DBusUtil.reply, None)
        self.assertEqual(DBusUtil.reply[1], 'application/vnd.syncml+xml')
        self.assertEqual(DBusUtil.reply[3], False)
        self.assertNotEqual(DBusUtil.reply[4], '')
        DBusUtil.quit_events = []

        # now quit for good
        connection2.Close(False, 'good bye')
        loop.run()
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath2 + " aborted",
                                                    "session done"])

    @timeout(60)
    def testKillInactive(self):
        """TestConnection.testKillInactive - block server with client A, then let client B connect twice"""
        #set up 2 configs
        self.setupConfig()
        self.setupConfig("dummy", "sc-pim-ppc")
        conpath, connection = self.getConnection()
        connection.Process(TestConnection.message1, 'application/vnd.syncml+xml')
        loop.run()
        # TODO: check events
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath + " got reply"])
        self.assertNotEqual(DBusUtil.reply, None)
        self.assertEqual(DBusUtil.reply[1], 'application/vnd.syncml+xml')
        self.assertEqual(DBusUtil.reply[3], False)
        self.assertNotEqual(DBusUtil.reply[4], '')
        DBusUtil.reply = None
        DBusUtil.quit_events = []

        # Now start two more sessions with the second client *without*
        # closing the first one. The server should remove only the
        # first connection of client B.
        message1_clientB = TestConnection.message1.replace("sc-api-nat", "sc-pim-ppc")
        conpath2, connection2 = self.getConnection()
        connection2.Process(message1_clientB, 'application/vnd.syncml+xml')
        conpath3, connection3 = self.getConnection()
        connection3.Process(message1_clientB, 'application/vnd.syncml+xml')
        # Queueing session for connection2 done now.
        loop.run()
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath2 + " aborted",
                                                "session done"])
        DBusUtil.quit_events = []

        # now quit for good
        connection3.Close(False, 'good bye client B')
        loop.run()
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath3 + " aborted",
                                                "session done"])
        DBusUtil.quit_events = []
        connection.Close(False, 'good bye client A')
        loop.run()
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath + " aborted",
                                                    "session done"])

    @timeout(60)
    def testTimeoutSync(self):
        """TestConnection.testTimeoutSync - start a sync, then wait for server to detect that we stopped replying"""

        # The server-side configuration for sc-api-nat must contain a retryDuration=10
        # because this test itself will time out with a failure after 60 seconds.
        self.setupConfig()
        conpath, connection = self.getConnection()
        connection.Process(TestConnection.message1, 'application/vnd.syncml+xml')
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath + " got reply"])
        DBusUtil.quit_events = []
        # TODO: check events
        self.assertNotEqual(DBusUtil.reply, None)
        self.assertEqual(DBusUtil.reply[1], 'application/vnd.syncml+xml')
        # wait for connection reset and "session done" due to timeout
        loop.run()
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["connection " + conpath + " aborted",
                                                    "session done"])

class TestMultipleConfigs(unittest.TestCase, DBusUtil):
    """ sharing of properties between configs

    Creates and tests the configs 'foo', 'bar', 'foo@other_context',
    '@default' and checks that 'defaultPeer' (global), 'syncURL' (per
    peer), 'database' (per source), 'uri' (per source and peer)
    are shared correctly.

    Runs with a the server ready, without session."""

    def setUp(self):
        self.setUpServer()

    def run(self, result):
        self.runTest(result)

    def setupEmpty(self):
        """Creates empty configs 'foo', 'bar', 'foo@other_context'.
        Updating non-existant configs is an error. Use this
        function before trying to update one of these configs."""
        self.setUpSession("foo")
        self.session.SetConfig(False, False, {"" : {}})
        self.session.Detach()
        self.setUpSession("bar")
        self.session.SetConfig(False, False, {"": {}})
        self.session.Detach()
        self.setUpSession("foo@other_CONTEXT")
        self.session.SetConfig(False, False, {"": {}})
        self.session.Detach()

    def setupConfigs(self):
        """Creates polulated configs 'foo', 'bar', 'foo@other_context'."""
        self.setupEmpty()

        # update normal view on "foo"
        self.setUpSession("foo")
        self.session.SetConfig(True, False,
                               { "" : { "defaultPeer" : "foobar_peer",
                                        "deviceId" : "shared-device-identifier",
                                        "syncURL": "http://scheduleworld" },
                                 "source/calendar" : { "uri" : "cal3" },
                                 "source/addressbook" : { "database": "Personal",
                                                          "sync" : "two-way",
                                                          "uri": "card3" } },
                               utf8_strings=True)
        self.session.Detach()

        # "bar" shares properties with "foo"
        self.setUpSession("bar")
        config = self.session.GetConfig(False, utf8_strings=True)
        self.assertEqual(config[""]["defaultPeer"], "foobar_peer")
        self.assertEqual(config[""]["deviceId"], "shared-device-identifier")
        self.assertEqual(config["source/addressbook"]["database"], "Personal")
        self.session.SetConfig(True, False,
                               { "" : { "syncURL": "http://funambol" },
                                 "source/calendar" : { "uri" : "cal" },
                                 "source/addressbook" : { "database": "Work",
                                                          "sync" : "refresh-from-client",
                                                          "uri": "card" } },
                               utf8_strings=True)
        self.session.Detach()

    def testSharing(self):
        """TestMultipleConfigs.testSharing - set up configs and tests reading them"""
        self.setupConfigs()

        # check how view "foo" has been modified
        self.setUpSession("Foo@deFAULT")
        config = self.session.GetConfig(False, utf8_strings=True)
        self.assertEqual(config[""]["defaultPeer"], "foobar_peer")
        self.assertEqual(config[""]["syncURL"], "http://scheduleworld")
        self.assertEqual(config["source/addressbook"]["database"], "Work")
        self.assertEqual(config["source/addressbook"]["uri"], "card3")
        self.session.Detach()

        # different ways of addressing this context
        self.setUpSession("")
        config = self.session.GetConfig(False, utf8_strings=True)
        self.assertEqual(config[""]["defaultPeer"], "foobar_peer")
        self.assertIn("source/addressbook", config)
        self.assertNotIn("uri", config["source/addressbook"])
        self.session.Detach()

        self.setUpSession("@DEFAULT")
        config = self.session.GetConfig(False, utf8_strings=True)
        self.assertEqual(config[""]["defaultPeer"], "foobar_peer")
        self.assertEqual(config[""]["deviceId"], "shared-device-identifier")
        self.assertIn("source/addressbook", config)
        self.assertNotIn("uri", config["source/addressbook"])
        self.session.Detach()

        # different context
        self.setUpSession("@other_context")
        config = self.session.GetConfig(False, utf8_strings=True)
        self.assertEqual(config[""]["defaultPeer"], "foobar_peer")
        self.assertNotIn("source/addressbook", config)
        self.session.Detach()

    def testSharedTemplate(self):
        """TestMultipleConfigs.testSharedTemplate - templates must contain shared properties"""
        self.setupConfigs()

        config = self.server.GetConfig("scheduleworld", True, utf8_strings=True)
        self.assertEqual(config[""]["defaultPeer"], "foobar_peer")
        self.assertEqual(config[""]["deviceId"], "shared-device-identifier")
        self.assertEqual(config["source/addressbook"]["database"], "Work")

    def testSharedProperties(self):
        """TestMultipleConfigs.testSharedProperties - 'type' consists of per-peer and shared properties"""
        self.setupConfigs()

        # writing for peer modifies properties in "foo" and context
        self.setUpSession("Foo@deFAULT")
        config = self.session.GetConfig(False, utf8_strings=True)
        config["source/addressbook"]["syncFormat"] = "text/vcard"
        config["source/addressbook"]["backend"] = "file"
        config["source/addressbook"]["databaseFormat"] = "text/x-vcard"
        self.session.SetConfig(True, False,
                               config,
                               utf8_strings=True)
        config = self.server.GetConfig("Foo", False, utf8_strings=True)
        self.assertEqual(config["source/addressbook"]["syncFormat"], "text/vcard")
        config = self.server.GetConfig("@default", False, utf8_strings=True)
        self.assertEqual(config["source/addressbook"]["backend"], "file")
        self.assertEqual(config["source/addressbook"]["databaseFormat"], "text/x-vcard")
        self.session.Detach()

    def testSharedPropertyOther(self):
        """TestMultipleConfigs.testSharedPropertyOther - shared backend properties must be preserved when adding peers"""
        # writing peer modifies properties in "foo" and creates context "@other"
        self.setUpSession("Foo@other")
        config = self.server.GetConfig("ScheduleWorld@other", True, utf8_strings=True)
        config["source/addressbook"]["backend"] = "file"
        config["source/addressbook"]["databaseFormat"] = "text/x-vcard"
        self.session.SetConfig(False, False,
                               config,
                               utf8_strings=True)
        config = self.server.GetConfig("Foo", False, utf8_strings=True)
        self.assertEqual(config["source/addressbook"]["backend"], "file")
        config = self.server.GetConfig("@other", False, utf8_strings=True)
        self.assertEqual(config["source/addressbook"]["databaseFormat"], "text/x-vcard")
        self.session.Detach()

        # adding second client must preserve backend value
        self.setUpSession("bar@other")
        config = self.server.GetConfig("Funambol@other", True, utf8_strings=True)
        self.assertEqual(config["source/addressbook"]["backend"], "file")
        self.session.SetConfig(False, False,
                               config,
                               utf8_strings=True)
        config = self.server.GetConfig("bar", False, utf8_strings=True)
        self.assertEqual(config["source/addressbook"]["backend"], "file")
        self.assertEqual(config["source/addressbook"].get("syncFormat"), None)
        config = self.server.GetConfig("@other", False, utf8_strings=True)
        self.assertEqual(config["source/addressbook"]["databaseFormat"], "text/x-vcard")

    def testOtherContext(self):
        """TestMultipleConfigs.testOtherContext - write into independent context"""
        self.setupConfigs()

        # write independent "foo@other_context" config
        self.setUpSession("foo@other_context")
        config = self.session.GetConfig(False, utf8_strings=True)
        config[""]["syncURL"] = "http://scheduleworld2"
        config["source/addressbook"] = { "database": "Play",
                                         "uri": "card30" }
        self.session.SetConfig(True, False,
                               config,
                               utf8_strings=True)
        config = self.session.GetConfig(False, utf8_strings=True)
        self.assertEqual(config[""]["defaultPeer"], "foobar_peer")
        self.assertEqual(config[""]["syncURL"], "http://scheduleworld2")
        self.assertEqual(config["source/addressbook"]["database"], "Play")
        self.assertEqual(config["source/addressbook"]["uri"], "card30")
        self.session.Detach()

        # "foo" modified?
        self.setUpSession("foo")
        config = self.session.GetConfig(False, utf8_strings=True)
        self.assertEqual(config[""]["defaultPeer"], "foobar_peer")
        self.assertEqual(config[""]["syncURL"], "http://scheduleworld")
        self.assertEqual(config["source/addressbook"]["database"], "Work")
        self.assertEqual(config["source/addressbook"]["uri"], "card3")
        self.session.Detach()

    def testSourceRemovalLocal(self):
        """TestMultipleConfigs.testSourceRemovalLocal - remove 'addressbook' source in 'foo'"""
        self.setupConfigs()
        self.setUpSession("foo")
        config = self.session.GetConfig(False, utf8_strings=True)
        del config["source/addressbook"]
        self.session.SetConfig(False, False, config, utf8_strings=True)
        self.session.Detach()

        # "addressbook" still exists in "foo" but only with default values
        config = self.server.GetConfig("foo", False, utf8_strings=True)
        self.assertNotIn("uri", config["source/addressbook"])
        self.assertNotIn("sync", config["source/addressbook"])

        # "addressbook" unchanged in "bar"
        config = self.server.GetConfig("bar", False, utf8_strings=True)
        self.assertEqual(config["source/addressbook"]["uri"], "card")
        self.assertEqual(config["source/addressbook"]["sync"], "refresh-from-client")

    def testSourceRemovalGlobal(self):
        """TestMultipleConfigs.testSourceRemovalGlobal - remove "addressbook" everywhere"""
        self.setupConfigs()
        self.setUpSession("")
        config = self.session.GetConfig(False, utf8_strings=True)
        del config["source/addressbook"]
        self.session.SetConfig(False, False, config, utf8_strings=True)
        self.session.Detach()

        # "addressbook" gone in "foo" and "bar"
        config = self.server.GetConfig("foo", False, utf8_strings=True)
        self.assertNotIn("source/addressbook", config)
        config = self.server.GetConfig("bar", False, utf8_strings=True)
        self.assertNotIn("source/addressbook", config)

    def testRemovePeer(self):
        """TestMultipleConfigs.testRemovePeer - check listing of peers while removing 'bar'"""
        self.setupConfigs()
        self.testOtherContext()
        self.setUpSession("bar")
        peers = self.session.GetConfigs(False, utf8_strings=True)
        self.assertEqual(peers,
                             [ "bar", "foo", "foo@other_context" ])
        peers2 = self.server.GetConfigs(False, utf8_strings=True)
        self.assertEqual(peers, peers2)
        # remove "bar"
        self.session.SetConfig(False, False, {}, utf8_strings=True)
        peers = self.server.GetConfigs(False, utf8_strings=True)
        self.assertEqual(peers,
                             [ "foo", "foo@other_context" ])
        self.session.Detach()

        # other configs should not have been affected
        config = self.server.GetConfig("foo", False, utf8_strings=True)
        self.assertEqual(config[""]["defaultPeer"], "foobar_peer")
        self.assertEqual(config[""]["syncURL"], "http://scheduleworld")
        self.assertEqual(config["source/calendar"]["uri"], "cal3")
        config = self.server.GetConfig("foo@other_context", False, utf8_strings=True)
        self.assertEqual(config[""]["defaultPeer"], "foobar_peer")
        self.assertEqual(config[""]["syncURL"], "http://scheduleworld2")
        self.assertEqual(config["source/addressbook"]["database"], "Play")
        self.assertEqual(config["source/addressbook"]["uri"], "card30")

    def testRemoveContext(self):
        """TestMultipleConfigs.testRemoveContext - remove complete config"""
        self.setupConfigs()
        self.setUpSession("")
        self.session.SetConfig(False, False, {}, utf8_strings=True)
        config = self.session.GetConfig(False, utf8_strings=True)
        self.assertEqual(config[""]["defaultPeer"], "foobar_peer")
        peers = self.server.GetConfigs(False, utf8_strings=True)
        self.assertEqual(peers, ['foo@other_context'])
        self.session.Detach()

    def testTemplates(self):
        """TestMultipleConfigs.testTemplates - templates reuse common properties"""
        self.setupConfigs()

        # deviceID must be shared and thus be reused in templates
        self.setUpSession("")
        config = self.session.GetConfig(False, utf8_strings=True)
        config[""]["DEVICEID"] = "shared-device-identifier"
        self.session.SetConfig(True, False, config, utf8_strings=True)
        config = self.server.GetConfig("", False, utf8_strings=True)
        self.assertEqual(config[""]["deviceId"], "shared-device-identifier")

        # get template for default context
        config = self.server.GetConfig("scheduleworld", True, utf8_strings=True)
        self.assertEqual(config[""]["defaultPeer"], "foobar_peer")
        self.assertEqual(config[""]["deviceId"], "shared-device-identifier")

        # now for @other_context - different device ID!
        config = self.server.GetConfig("scheduleworld@other_context", True, utf8_strings=True)
        self.assertEqual(config[""]["defaultPeer"], "foobar_peer")
        self.assertNotEqual(config[""]["deviceId"], "shared-device-identifier")

class TestLocalSync(unittest.TestCase, DBusUtil):
    """Tests involving local sync."""

    def run(self, result):
        self.runTest(result)

    def setUp(self):
        self.setUpServer()

    def setUpConfigs(self, childPassword=None):
        self.setUpLocalSyncConfigs(childPassword)

    @timeout(100)
    def testSync(self):
        """TestLocalSync.testSync - run a simple slow sync between local dirs"""
        self.setUpConfigs()
        os.makedirs(xdg_root + "/server")
        output = open(xdg_root + "/server/0", "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
END:VCARD''')
        output.close()
        self.setUpListeners(self.sessionpath)
        self.session.Sync("slow", {})
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["session " + self.sessionpath + " done"])
        self.checkSync()
        input = open(xdg_root + "/server/0", "r")
        self.assertIn("FN:John Doe", input.read())

    def setUpInfoRequest(self, response={"password" : "123456"}):
        self.lastState = "unknown"
        def infoRequest(id, session, state, handler, type, params):
            if state == "request":
                self.assertEqual(self.lastState, "unknown")
                self.lastState = "request"
                if response != None:
                    self.server.InfoResponse(id, "working", {}, utf8_strings=True)
            elif state == "waiting":
                self.assertEqual(self.lastState, "request")
                self.lastState = "waiting"
                if response != None:
                    self.server.InfoResponse(id, "response", response, utf8_strings=True)
            elif state == "done":
                self.assertEqual(self.lastState, "waiting")
                self.lastState = "done"
            else:
                self.fail("state should not be '" + state + "'")

        signal = bus.add_signal_receiver(infoRequest,
                                         'InfoRequest',
                                         'org.syncevolution.Server',
                                         self.server.bus_name,
                                         None,
                                         byte_arrays=True,
                                         utf8_strings=True)
        return signal

    @timeout(100)
    def testPasswordRequest(self):
        """TestLocalSync.testPasswordRequest - check that password request child->parent->us works"""
        self.setUpConfigs(childPassword="-")
        self.setUpListeners(self.sessionpath)
        signal = self.setUpInfoRequest()
        try:
            self.session.Sync("slow", {})
            loop.run()
        finally:
            signal.remove()

        self.assertEqual(DBusUtil.quit_events, ["session " + self.sessionpath + " done"])
        self.assertEqual(self.lastState, "done")
        self.checkSync()
        self.assertSyncStatus('server', 200, None)

    @timeout(100)
    def testPasswordRequestAbort(self):
        """TestLocalSync.testPasswordRequestAbort - let user cancel password request"""
        self.setUpConfigs(childPassword="-")
        self.setUpListeners(self.sessionpath)
        signal = self.setUpInfoRequest(response={})
        try:
            self.session.Sync("slow", {})
            loop.run()
        finally:
            signal.remove()

        self.assertEqual(DBusUtil.quit_events, ["session " + self.sessionpath + " done"])
        self.assertEqual(self.lastState, "done")
        self.checkSync(expectedError=20017, expectedResult=20017)
        self.assertSyncStatus('server', 20017, "error code from SyncEvolution aborted on behalf of user (local, status 20017): failure in local sync child: User did not provide the 'addressbook backend' password.")

    @timeout(200)
    def testPasswordRequestTimeout(self):
        """TestLocalSync.testPassswordRequestTimeout - let password request time out"""
        self.setUpConfigs(childPassword="-")
        self.setUpListeners(self.sessionpath)
        signal = self.setUpInfoRequest(response=None)
        try:
            # Will time out roughly after 120 seconds.
            start = time.time()
            self.session.Sync("slow", {})
            loop.run()
        finally:
            signal.remove()

        self.assertEqual(DBusUtil.quit_events, ["session " + self.sessionpath + " done"])
        self.assertEqual(self.lastState, "request")
        self.checkSync(expectedError=22003, expectedResult=22003)
        self.assertSyncStatus('server', 22003, "error code from SyncEvolution password request timed out (local, status 22003): failure in local sync child: Could not get the 'addressbook backend' password from user.")
        end = time.time()
        self.assertAlmostEqual(120 + (usingValgrind() and 20 or 0),
                               end - start,
                               delta=usingValgrind() and 60 or 20)


    # Killing the syncevo-dbus-helper before it even starts (SYNCEVOLUTION_LOCAL_CHILD_DELAY=5)
    # is possible, but leads to ugly valgrind warnings about "possibly lost" memory because
    # SIGTERM really kills the process right away. Better wait until syncing really has started
    # in the helper (SYNCEVOLUTION_SYNC_DELAY=5). The test is more realistic that way, too.
    @property("ENV", usingValgrind() and "SYNCEVOLUTION_SYNC_DELAY=55" or "SYNCEVOLUTION_SYNC_DELAY=5")
    @timeout(100)
    def testConcurrency(self):
        """TestLocalSync.testConcurrency - D-Bus server must remain responsive while sync runs"""
        self.setUpConfigs()
        self.setUpListeners(self.sessionpath)
        self.session.Sync("slow", {})
        time.sleep(usingValgrind() and 30 or 3)
        status, error, sources = self.session.GetStatus(utf8_strings=True)
        self.assertEqual(status, "running")
        self.assertEqual(error, 0)
        self.session.Abort()
        loop.run()
        self.assertEqual(DBusUtil.quit_events, ["session " + self.sessionpath + " done"])
        report = self.checkSync(20017, 20017, reportOptional=True) # aborted, with or without report
        if report:
            self.assertNotIn("error", report) # ... but without error message
            self.assertEqual(report["source-addressbook-status"], "0") # unknown status for source (aborted early)

    @timeout(200)
    def testParentFailure(self):
        """TestLocalSync.testParentFailure - check that child detects when parent dies"""
        self.setUpConfigs(childPassword="-")
        self.setUpListeners(self.sessionpath)
        self.lastState = "unknown"
        pid = self.serverPid()
        serverPid = DBusUtil.pserver.pid
        def infoRequest(id, session, state, handler, type, params):
            if state == "request":
                self.assertEqual(self.lastState, "unknown")
                self.lastState = "request"
                # kill syncevo-dbus-server
                if pid != serverPid:
                    logging.printf('killing syncevo-dbus-server wrapper with pid %d', serverPid)
                    os.kill(serverPid, signal.SIGKILL)
                logging.printf('killing syncevo-dbus-server with pid %d', pid)
                os.kill(pid, signal.SIGKILL)
                loop.quit()
        dbusSignal = bus.add_signal_receiver(infoRequest,
                                             'InfoRequest',
                                             'org.syncevolution.Server',
                                             self.server.bus_name,
                                             None,
                                             byte_arrays=True,
                                             utf8_strings=True)

        try:
            self.session.Sync("slow", {})
            loop.run()
        finally:
            dbusSignal.remove()

        # Give syncevo-dbus-helper and syncevo-local-sync some time to shut down.
        time.sleep(usingValgrind() and 60 or 10)

        # Remove syncevo-dbus-server zombie process(es).
        DBusUtil.pserver.wait()
        DBusUtil.pserver = None
        try:
            while True:
                res = os.waitpid(-1, os.WNOHANG)
                if res[0]:
                    logging.printf('got status %d for pid %d', res[1], res[0])
                else:
                    break
        except OSError, ex:
            if ex.errno != errno.ECHILD:
                raise ex

        # Now no processes should be left in the process group
        # of the syncevo-dbus-server.
        self.assertEqual({}, self.getChildren())

        # Sync should have failed with an explanation that it was
        # because of the password.
        self.assertSyncStatus('server', 22003, "error code from SyncEvolution password request timed out (local, status 22003): failure in local sync child: Could not get the 'addressbook backend' password from user.")

    @timeout(200)
    @property("ENV", "SYNCEVOLUTION_LOCAL_CHILD_DELAY=10") # allow killing syncevo-dbus-server in middle of sync
    def testNoParent(self):
        """TestLocalSync.testNoParent - check that sync helper can continue without parent"""
        self.setUpConfigs()
        self.setUpListeners(self.sessionpath)
        pid = self.serverPid()
        serverPid = DBusUtil.pserver.pid
        def killServer(*args, **keywords):
            if pid != serverPid:
                logging.printf('killing syncevo-dbus-server wrapper with pid %d', serverPid)
                os.kill(serverPid, signal.SIGKILL)
            logging.printf('killing syncevo-dbus-server with pid %d', pid)
            os.kill(pid, signal.SIGKILL)
            DBusUtil.pserver.wait()
            DBusUtil.pserver = None
            loop.quit()
        self.progressChanged = killServer

        self.session.Sync("slow", {})
        loop.run()

        # Should have killed server.
        self.assertFalse(DBusUtil.pserver)

        # Give syncevo-dbus-helper and syncevo-local-sync some time to shut down.
        time.sleep(usingValgrind() and 60 or 20)
        logging.log('sync should be done now')

        # Remove syncevo-dbus-server zombie process(es).
        try:
            while True:
                res = os.waitpid(-1, os.WNOHANG)
                if res[0]:
                    logging.printf('got status %d for pid %d', res[1], res[0])
                else:
                    break
        except OSError, ex:
            if ex.errno != errno.ECHILD:
                raise ex

        # Now no processes should be left in the process group
        # of the syncevo-dbus-server.
        self.assertEqual({}, self.getChildren())

        # Sync should have succeeded.
        self.assertSyncStatus('server', 200, None)

class TestFileNotify(unittest.TestCase, DBusUtil):
    """syncevo-dbus-server must stop if one of its files mapped into
    memory (executable, libraries) change. Furthermore it must restart
    if automatic syncs are enabled. This class simulates such file changes
    by starting the server, identifying the location of the main executable,
    and renaming it back and forth."""

    def setUp(self):
        self.setUpServer()
        self.serverexe = self.serverExecutable()

    def tearDown(self):
        if os.path.isfile(self.serverexe + ".bak"):
            os.rename(self.serverexe + ".bak", self.serverexe)

    def run(self, result):
        self.runTest(result)

    def modifyServerFile(self):
        """rename server executable to trigger shutdown"""
        os.rename(self.serverexe, self.serverexe + ".bak")
        os.rename(self.serverexe + ".bak", self.serverexe)        

    @timeout(100)
    def testShutdown(self):
        """TestFileNotify.testShutdown - update server binary for 30 seconds, check that it shuts down at most 15 seconds after last mod"""
        self.assertTrue(self.isServerRunning())
        i = 0
        # Server must not shut down immediately, more changes might follow.
        # Simulate that.
        while i < 6:
            self.modifyServerFile()
            time.sleep(5)
            i = i + 1
        self.assertTrue(self.isServerRunning())
        time.sleep(10)
        self.assertFalse(self.isServerRunning())

    @timeout(30)
    def testSession(self):
        """TestFileNotify.testSession - create session, shut down directly after closing it"""
        self.assertTrue(self.isServerRunning())
        self.setUpSession("")
        self.modifyServerFile()
        time.sleep(15)
        self.assertTrue(self.isServerRunning())
        self.session.Detach()
        # should shut down almost immediately,
        # except when using valgrind
        if usingValgrind():
            time.sleep(10)
        else:
            time.sleep(1)
        self.assertFalse(self.isServerRunning())

    @timeout(30)
    def testSession2(self):
        """TestFileNotify.testSession2 - create session, shut down after quiesence period after closing it"""
        self.assertTrue(self.isServerRunning())
        self.setUpSession("")
        self.modifyServerFile()
        self.assertTrue(self.isServerRunning())
        self.session.Detach()
        time.sleep(8)
        self.assertTrue(self.isServerRunning())
        if usingValgrind():
            time.sleep(10)
        else:
            time.sleep(4)
        self.assertFalse(self.isServerRunning())

    @timeout(30)
    def testSession3(self):
        """TestFileNotify.testSession3 - shut down after quiesence period without activating a pending session request"""
        self.assertTrue(self.isServerRunning())
        self.modifyServerFile()
        self.assertTrue(self.isServerRunning())
        time.sleep(8)
        self.assertTrue(self.isServerRunning())
        self.session = None
        try:
            # this should not succeed: the server should rejects the
            # session request because it is shutting down
            self.setUpSession("")
        except dbus.DBusException, ex:
            self.assertEqual(str(ex),
                             "org.syncevolution.Exception: server shutting down")
        else:
            self.fail("no exception thrown")

    @timeout(100)
    def testRestart(self):
        """TestFileNotify.testRestart - set up auto sync, then check that server restarts"""
        self.assertTrue(self.isServerRunning())
        self.setUpSession("memotoo")
        config = self.session.GetConfig(True, utf8_strings=True)
        config[""]["autoSync"] = "1"
        self.session.SetConfig(False, False, config)
        self.assertTrue(self.isServerRunning())
        self.session.Detach()
        self.modifyServerFile()
        bus_name = self.server.bus_name
        # give server time to restart
        if usingValgrind():
            time.sleep(40)
        else:
            time.sleep(15)
        self.setUpServer()
        self.assertNotEqual(bus_name, self.server.bus_name)
        # serverExecutable() will fail if the service wasn't properly
        # with execve() because then the old process is dead.
        self.assertEqual(self.serverexe, self.serverExecutable())

bt_mac         = "D4:5D:42:73:E4:6C"
bt_fingerprint = "Nokia 5230"
bt_name        = "My Nokia 5230"
bt_template    = "Bluetooth_%s_1" % (bt_mac)
bt_device      = "%s/dev_%s" % (bt_adaptor, string.replace(bt_mac, ':', '_'))

class BluezAdapter (dbus.service.Object):
    def __init__(self):
        self.SUPPORTS_MULTIPLE_OBJECT_PATHS = True
        bus_name = dbus.service.BusName('org.bluez', bus)
        dbus.service.Object.__init__(self, bus_name, bt_adaptor)

    @dbus.service.signal(dbus_interface='org.bluez.Adapter', signature='o')
    def DeviceCreated(self, obj):
        return bt_adaptor

    @dbus.service.signal(dbus_interface='org.bluez.Adapter', signature='o')
    def DeviceRemoved(self, obj):
        return bt_device

    @dbus.service.method(dbus_interface='org.bluez.Adapter', in_signature='', out_signature='ao')
    def ListDevices(self):
        return [bt_device]

class BluezDevice (dbus.service.Object):
    def __init__(self):
        self.SUPPORTS_MULTIPLE_OBJECT_PATHS = True
        bus_name = dbus.service.BusName('org.bluez', bus)
        dbus.service.Object.__init__(self, bus_name, bt_device)

    @dbus.service.method(dbus_interface='org.bluez.Device', in_signature='', out_signature='a{sv}')
    def GetProperties(self):
        return {"Name": bt_name,
                "Address": bt_mac,
                "UUIDs": ['00000002-0000-1000-8000-0002ee000002',
                          '00001000-0000-1000-8000-00805f9b34fb',
                          '00001101-0000-1000-8000-00805f9b34fb',
                          '00001103-0000-1000-8000-00805f9b34fb',
                          '00001105-0000-1000-8000-00805f9b34fb',
                          '00001106-0000-1000-8000-00805f9b34fb',
                          '0000110a-0000-1000-8000-00805f9b34fb',
                          '0000110c-0000-1000-8000-00805f9b34fb',
                          '0000110e-0000-1000-8000-00805f9b34fb',
                          '00001112-0000-1000-8000-00805f9b34fb',
                          '0000111b-0000-1000-8000-00805f9b34fb',
                          '0000111f-0000-1000-8000-00805f9b34fb',
                          '0000112d-0000-1000-8000-00805f9b34fb',
                          '0000112f-0000-1000-8000-00805f9b34fb',
                          '00001200-0000-1000-8000-00805f9b34fb',
                          '00005005-0000-1000-8000-0002ee000001',
                          '00005557-0000-1000-8000-0002ee000001',
                          '00005601-0000-1000-8000-0002ee000001']}

    @dbus.service.method(dbus_interface='org.bluez.Device', in_signature='s', out_signature='a{us}')
    def DiscoverServices(self, ignore):
        # This should be the last method to call. So, we need to quit the loop to exit.
        loop.quit()
        return { 65569L: '<?xml version="1.0" encoding="UTF-8" ?><record><attribute id="0x0000"><uint32 value="0x00010021" /></attribute><attribute id="0x0001"><sequence><uuid value="0x1200" /></sequence></attribute><attribute id="0x0005"><sequence><uuid value="0x1002" /></sequence></attribute><attribute id="0x0006"><sequence><uint16 value="0x454e" /><uint16 value="0x006a" /><uint16 value="0x0100" /></sequence></attribute><attribute id="0x0100"><text value="PnP Information" /></attribute><attribute id="0x0200"><uint16 value="0x0102" /></attribute><attribute id="0x0201"><uint16 value="0x0001" /></attribute><attribute id="0x0202"><uint16 value="0x00e7" /></attribute><attribute id="0x0203"><uint16 value="0x0000" /></attribute><attribute id="0x0204"><boolean value="true" /></attribute><attribute id="0x0205"><uint16 value="0x0001" /></attribute></record>'}

    @dbus.service.signal(dbus_interface='org.bluez.Device', signature='sv')
    def PropertyChanged(self, key, value):
        if(key == "Name"):
            bt_name = value

    def emitSignal(self):
        """ Change the device name. """
        self.PropertyChanged("Name", [string.replace(bt_name, "My", "Changed")])
        return

class TestBluetooth(unittest.TestCase, DBusUtil):
    """Tests that Bluetooth works properly."""

    def setUp(self):
        self.adp_conn = BluezAdapter()
        self.dev_conn = BluezDevice()
        loop.run()
        self.setUpServer()

    def tearDown(self):
        self.adp_conn.remove_from_connection()
        self.dev_conn.remove_from_connection()

    def run(self, result):
        self.runTest(result)

    @property("ENV", "DBUS_TEST_BLUETOOTH=session")
    @timeout(100)
    def testBluetoothTemplates(self):
        """TestBluetooth.testBluetoothTemplates - check for the bluetooth device's template"""
        configs = self.server.GetConfigs(True, utf8_strings=True)
        config = next((config for config in configs if config == bt_template), None)
        self.failUnless(config)

    @property("ENV", "DBUS_TEST_BLUETOOTH=session")
    @timeout(100)
    def testBluetoothNames(self):
        """TestBluetooth.testBluetoothNames - check that fingerPrint/peerName/deviceName/hardwareName are set correctly"""
        # This needs to be called before we can fetch the single config.
        configs = self.server.GetConfigs(True, utf8_strings=True)
        config  = self.server.GetConfig(bt_template, True, utf8_strings=True)
        # user-configurable name
        self.failUnlessEqual(config['']["deviceName"], bt_name)
        # must not be set
        self.assertNotIn("peerName", config[''])
        # all of the possible strings in the template, must include the hardware name of this example device
        self.failIf(string.find(config['']["fingerPrint"], bt_fingerprint) < 0)
        # real hardware information
        self.failUnlessEqual(config['']["hardwareName"], bt_fingerprint)

def createFiles(root, content, append = False):
    '''create directory hierarchy, overwriting previous content'''
    if not append:
        shutil.rmtree(root, True)

    entries = content.split("\n")
    outname = ''
    outfile = None
    for entry in entries:
        if not entry:
            continue
        parts = entry.split(":", 1)
        newname = parts[0]
        line = parts[1]
        if newname != outname:
            fullpath = root + "/" + newname
            try:
                os.makedirs(fullpath[0:fullpath.rindex("/")])
            except:
                pass
            mode = "w"
            if append:
                mode = "a"
            outfile = open(fullpath, mode)
            outname = newname
        outfile.write(line + "\n")
    outfile.close()

isPropRegEx = re.compile(r'^([a-zA-Z]+) = ')
def isPropAssignment (line):
    '''true if "<word> = "'''
    m = isPropRegEx.search(line)
    if not m:
        return False
    # exclude some false positives
    if m.group(1) in ('KCalExtended', 'mkcal', 'QtContacts'):
        return False
    return True

def scanFiles(root, peer = '', onlyProps = True, directory = ''):
    '''turn directory hierarchy into string
    root      - root path in file system
    peer      - if non-empty, then ignore all <root>/peers/<foo>
                directories where <foo> != peer
    onlyProps - ignore lines which are comments
    directory - a subdirectory of root (used for recursion)'''
    newroot = root + '/' + directory
    out = ''

    for entry in sorted(os.listdir(newroot)):
        fullEntry = newroot + "/" + entry
        if os.path.isdir(fullEntry):
            if not (newroot.endswith("/peers") and peer and entry != peer):
                if directory:
                    newdir = directory + '/' + entry
                else:
                    newdir = entry
                out += scanFiles(root, peer, onlyProps, newdir)
        else:
            infile = open (fullEntry)
            for line in infile:
                line = line.rstrip("\r\n")
                if (line):
                    takeIt = False
                    if (line.startswith("# ")):
                        takeIt = isPropAssignment(line[2:])
                    else:
                        takeIt = True
                    if (not onlyProps or takeIt):
                        if (directory):
                            out += directory + "/"
                        out += entry + ':' + line + "\n"
    return out

def sortConfig(config):
    '''sort lines by file, preserving order inside each line'''
    lines = config.splitlines()
    linenr = -1

    unsorted = []
    for line in lines:
        linenr += 1
        if not line:
            continue
        parts = line.split(":", 1)
        element = parts[0], linenr, parts[1]
        unsorted.append(element)

    # stable sort because of line number
    # probably it would be stable without it
    # but better be safe than sorry
    lines = sorted(unsorted)
    unsorted = []
    newconfig = ""
    for line in lines:
        newconfig += line[0] + ":" + line[2] + "\n"

    return newconfig

def lastLine(string):
    return string.splitlines(True)[-1]

def stripOutput(string):
    # strip debug output, if it was enabled via env var
    if os.environ.get('SYNCEVOLUTION_DEBUG', None) != None:
        string = re.sub(r'\[DEBUG *\S*?\].*?\n', '', string)
    # remove time
    r = re.compile(r'^\[(\w+)\s+\d\d:\d\d:\d\d\]', re.MULTILINE)
    string = r.sub(r'[\1]', string)
    return string

def injectValues(config):
    res = config

    if False:
        # username/password not set in templates, only in configs
        # created via the command line - not anymore, but if it ever
        # comes back, here's the place for it
        res = res.replace("# username = ",
                          "username = your SyncML server account name",
                          1)
        res = res.replace("# password = ",
                          "password = your SyncML server password",
                          1)

    return res

def filterConfig(config):
    '''remove pure comment lines from buffer, also empty lines, also
    defaultPeer (because reference properties do not include global
    props)'''
    config_lines = config.splitlines()
    out = ''

    for line in config_lines:
        if line:
            index = line.find("defaultPeer =")
            if index == -1:
                if line.startswith("# ") == False or isPropAssignment(line[2:]):
                    out += line + "\n"

    return out

def internalToIni(config):
    '''convert the internal config dump to .ini style (--print-config)'''
    config_lines = config.splitlines()
    ini = ''
    section = ''

    for line in config_lines:
        if not line:
            continue
        colon = line.find(':')
        prefix = line[0:colon]
        # internal values are not part of the --print-config output
        if ".internal.ini" in prefix or "= internal value" in line:
            continue

        # --print-config also does not duplicate the "type" property
        # remove the shared property
        if ":type" in line and line.startswith("sources/"):
            continue

        # sources/<name>/config.ini or spds/sources/<name>/config.ini
        endslash = prefix.rfind("/")
        if endslash > 1:
            slash = prefix.rfind("/", 0, endslash)
            if slash != -1:
                newsource = prefix[slash + 1:endslash]
                if newsource != section:
                    sources = prefix.find("/sources/")
                    if sources != -1 and newsource != "syncml":
                        ini += "[" + newsource + "]\n"
                        section = newsource

        assignment = line[colon + 1:]
        # substitute aliases with generic values
        assignment = assignment.replace("= syncml:auth-md5", "= md5", 1)
        assignment = assignment.replace("= syncml:auth-basic", "= basic", 1)

        ini += assignment + "\n"

    return ini

# result of removeComments(self.removeRandomUUID(filterConfig())) for
# Google Calendar template/config
googlecaldav = '''syncURL = https://www.google.com/calendar/dav/%u/user/?SyncEvolution=Google
printChanges = 0
dumpData = 0
deviceId = fixed-devid
IconURI = image://themedimage/icons/services/google-calendar
ConsumerReady = 1
peerType = WebDAV
[calendar]
sync = two-way
backend = CalDAV
'''

# result of removeComments(self.removeRandomUUID(filterConfig())) for
# Yahoo Calendar + Contacts
yahoo = '''printChanges = 0
dumpData = 0
deviceId = fixed-devid
IconURI = image://themedimage/icons/services/yahoo
ConsumerReady = 1
peerType = WebDAV
[addressbook]
sync = disabled
backend = CardDAV
[calendar]
sync = two-way
backend = CalDAV
'''

def removeComments(config):
    lines = config.splitlines()

    out = ''
    for line in lines:
        if line and not line.startswith("#"):
            out += line + "\n"

    return out

def filterIndented(config):
    '''remove lines indented with spaces'''
    lines = config.splitlines()

    out = ''
    first = True
    for line in lines:
        if not line.startswith(" "):
            if first:
                first = False
            else:
                out += "\n"
            out += line

    return out

def filterFiles(config):
    '''remove comment lines from scanFiles() output'''
    out = ''
    lines = config.splitlines()

    for line in lines:
        if line.find(":#") == -1:
            out += line + "\n"
    # strip last newline
#    if out:
#        return out[:-1]
    return out

class TestCmdline(DBusUtil, unittest.TestCase):
    """Tests cmdline by Session::Execute()."""

    def setUp(self):
        self.setUpServer()
        self.setUpListeners(None)
        # All tests run with their own XDG root hierarchy.
        # Here are the config files.
        self.configdir = xdg_root + "/config/syncevolution"

    def run(self, result):
        # Runtime varies a lot when using valgrind, because
        # of the need to check an additional process. Allow
        # a lot more time when running under valgrind.
        self.runTest(result, own_xdg=True, own_home=True,
                     defTimeout=usingValgrind() and 600 or 20)

    def statusChanged(self, *args, **keywords):
        '''remember the command line session'''
        if args[0] == 'idle' and self.session == None:
            self.session = dbus.Interface(bus.get_object(self.server.bus_name,
                                                         keywords['path']),
                                          'org.syncevolution.Session')

    def runCmdline(self, args, env=None, expectSuccess=True, preserveOutputOrder=False,
                   sessionFlags=['no-sync']):
        '''Run the 'syncevolution' command line (from PATH) with the
        given arguments (list or tuple of strings). Uses environment
        used to run syncevo-dbus-server unless one is set
        explicitly. Unless told otherwise, the result of the command
        is checked for success. Usually stdout and stderr are captured
        separately, in which case relative order of messages from
        different streams cannot be tested. When that is relevant, set
        preserveOutputOrder=True and look only at the stdout.

        Returns tuple with stdout, stderr and result code. DBusUtil.events
        contains the status and progress events seen while the command line
        ran. self.session is the proxy for that session.'''

        # Watch all future events, ignore old ones.
        while loop.get_context().iteration(False):
            pass
        DBusUtil.events = []
        self.session = None
        a = [ 'syncevolution' ]
        a.extend(args)
        # Explicitly pass an environment. Otherwise subprocess.Popen()
        # from Python 2.6 uses the environment passed to a previous
        # call to subprocess.Popen() (which will fail if the previous
        # test ran with an environment which had SYNCEVOLUTION_DEBUG
        # set).
        if env == None:
            cmdline_env = self.storedenv
        else:
            cmdline_env = env
        if preserveOutputOrder:
            s = subprocess.Popen(a, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 env=cmdline_env)
        else:
            s = subprocess.Popen(a, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 env=cmdline_env)
        out, err = s.communicate()
        doFail = False
        if expectSuccess and s.returncode != 0:
            result = 'syncevolution command failed.'
            doFail = True
        elif not expectSuccess and s.returncode == 0:
            result = 'syncevolution was expected to fail, but it succeeded.'
            doFail = True
        if doFail:
            result += '\nOutput:\n%s' % out
            if not preserveOutputOrder:
                result += '\nSeparate stderr:\n%s' % err
            self.fail(result)

        # Collect D-Bus events, check session.
        while loop.get_context().iteration(False):
            pass
        if sessionFlags != None:
            self.assertTrue(self.session)
            self.assertEqual(self.session.GetFlags(), sessionFlags)

        return (out, err, s.returncode)

    def assertNoErrors(self, err):
        '''check that error output is empty'''
        self.assertEqualDiff('', err)

    cachedSSLServerCertificates = None
    def getSSLServerCertificates(self):
        '''Default SSLServerCertificates path as compiled into the SyncEvolution
        binaries. Determined once by asking for a template.'''
        if TestCmdline.cachedSSLServerCertificates == None:
            out, err, code = self.runCmdline(['--template', 'default',
                                              '--print-config'])
            self.assertNoErrors(err)
            m = re.search(r'^# SSLServerCertificates = (.*)\n', out, re.MULTILINE)
            self.assertTrue(m)
            TestCmdline.cachedSSLServerCertificates = m.group(1)
        return TestCmdline.cachedSSLServerCertificates

    cachedVersions = {}
    def getVersion(self, versionName):
        '''Parses the SyncConfig.h file to get a version number
        described by versionName.'''
        if not versionName in TestCmdline.cachedVersions:
            # Get the absolute path of the current python file.
            scriptpath = os.path.abspath(os.path.expanduser(os.path.expandvars(sys.argv[0])))
            # Get the path to SyncConfig.h
            header = os.path.join(os.path.dirname(scriptpath), '..', 'src', 'syncevo', 'SyncConfig.h')
            # do not care about escaping the versionName variable.
            # this function is only used from some version getters and
            # those are passing some simple strings with no regexp
            # metacharacters.
            prog = re.compile("^\s*static\s+const\s+int\s+{0}\s*=\s*(\d+);".format(versionName))
            found = False
            # will throw IOError if opening header fails
            for line in open(header):
                m = prog.match(line)
                if m:
                    TestCmdline.cachedVersions[versionName] = m.group(1)
                    found = True
                    break
            if not found:
                self.fail(versionname + " not found in SyncConfig.h")
        return TestCmdline.cachedVersions[versionName]

    def getRootMinVersion(self):
        return self.getVersion('CONFIG_ROOT_MIN_VERSION')

    def getRootCurVersion(self):
        return self.getVersion('CONFIG_ROOT_CUR_VERSION')

    def getContextMinVersion(self):
        return self.getVersion('CONFIG_CONTEXT_MIN_VERSION')

    def getContextCurVersion(self):
        return self.getVersion('CONFIG_CONTEXT_CUR_VERSION')

    def getPeerMinVersion(self):
        return self.getVersion('CONFIG_PEER_MIN_VERSION')

    def getPeerCurVersion(self):
        return self.getVersion('CONFIG_PEER_CUR_VERSION')

    def ScheduleWorldConfig(self,
                            peerMinVersion = None,
                            peerCurVersion = None,
                            contextMinVersion = None,
                            contextCurVersion = None):
        '''properties sorted by the order in which they are defined in
        the sync and sync source property registry'''
        if peerMinVersion == None:
            peerMinVersion = self.getPeerMinVersion()
        if peerCurVersion == None:
            peerCurVersion = self.getPeerCurVersion()
        if contextMinVersion == None:
            contextMinVersion = self.getContextMinVersion()
        if contextCurVersion == None:
            contextCurVersion = self.getContextCurVersion()

        return '''peers/scheduleworld/.internal.ini:peerMinVersion = {0}
peers/scheduleworld/.internal.ini:peerCurVersion = {1}
peers/scheduleworld/.internal.ini:# HashCode = 0
peers/scheduleworld/.internal.ini:# ConfigDate = 
peers/scheduleworld/.internal.ini:# lastNonce = 
peers/scheduleworld/.internal.ini:# deviceData = 
peers/scheduleworld/.internal.ini:# webDAVCredentialsOkay = 0
peers/scheduleworld/config.ini:syncURL = http://sync.scheduleworld.com/funambol/ds
peers/scheduleworld/config.ini:# username = 
peers/scheduleworld/config.ini:# password = 
.internal.ini:contextMinVersion = {2}
.internal.ini:contextCurVersion = {3}
config.ini:# logdir = 
peers/scheduleworld/config.ini:# loglevel = 0
peers/scheduleworld/config.ini:# printChanges = 1
peers/scheduleworld/config.ini:# dumpData = 1
config.ini:# maxlogdirs = 10
peers/scheduleworld/config.ini:# autoSync = 0
peers/scheduleworld/config.ini:# autoSyncInterval = 30M
peers/scheduleworld/config.ini:# autoSyncDelay = 5M
peers/scheduleworld/config.ini:# preventSlowSync = 1
peers/scheduleworld/config.ini:# useProxy = 0
peers/scheduleworld/config.ini:# proxyHost = 
peers/scheduleworld/config.ini:# proxyUsername = 
peers/scheduleworld/config.ini:# proxyPassword = 
peers/scheduleworld/config.ini:# clientAuthType = md5
peers/scheduleworld/config.ini:# RetryDuration = 5M
peers/scheduleworld/config.ini:# RetryInterval = 2M
peers/scheduleworld/config.ini:# remoteIdentifier = 
peers/scheduleworld/config.ini:# PeerIsClient = 0
peers/scheduleworld/config.ini:# SyncMLVersion = 
peers/scheduleworld/config.ini:PeerName = ScheduleWorld
config.ini:deviceId = fixed-devid
peers/scheduleworld/config.ini:# remoteDeviceId = 
peers/scheduleworld/config.ini:# enableWBXML = 1
peers/scheduleworld/config.ini:# maxMsgSize = 150000
peers/scheduleworld/config.ini:# maxObjSize = 4000000
peers/scheduleworld/config.ini:# SSLServerCertificates = {4}
peers/scheduleworld/config.ini:# SSLVerifyServer = 1
peers/scheduleworld/config.ini:# SSLVerifyHost = 1
peers/scheduleworld/config.ini:WebURL = http://www.scheduleworld.com
peers/scheduleworld/config.ini:IconURI = image://themedimage/icons/services/scheduleworld
peers/scheduleworld/config.ini:# ConsumerReady = 0
peers/scheduleworld/config.ini:# peerType = 
peers/scheduleworld/sources/addressbook/.internal.ini:# adminData = 
peers/scheduleworld/sources/addressbook/.internal.ini:# synthesisID = 0
peers/scheduleworld/sources/addressbook/config.ini:sync = two-way
peers/scheduleworld/sources/addressbook/config.ini:uri = card3
sources/addressbook/config.ini:backend = addressbook
peers/scheduleworld/sources/addressbook/config.ini:syncFormat = text/vcard
peers/scheduleworld/sources/addressbook/config.ini:# forceSyncFormat = 0
sources/addressbook/config.ini:# database = 
sources/addressbook/config.ini:# databaseFormat = 
sources/addressbook/config.ini:# databaseUser = 
sources/addressbook/config.ini:# databasePassword = 
peers/scheduleworld/sources/calendar/.internal.ini:# adminData = 
peers/scheduleworld/sources/calendar/.internal.ini:# synthesisID = 0
peers/scheduleworld/sources/calendar/config.ini:sync = two-way
peers/scheduleworld/sources/calendar/config.ini:uri = cal2
sources/calendar/config.ini:backend = calendar
peers/scheduleworld/sources/calendar/config.ini:# syncFormat = 
peers/scheduleworld/sources/calendar/config.ini:# forceSyncFormat = 0
sources/calendar/config.ini:# database = 
sources/calendar/config.ini:# databaseFormat = 
sources/calendar/config.ini:# databaseUser = 
sources/calendar/config.ini:# databasePassword = 
peers/scheduleworld/sources/memo/.internal.ini:# adminData = 
peers/scheduleworld/sources/memo/.internal.ini:# synthesisID = 0
peers/scheduleworld/sources/memo/config.ini:sync = two-way
peers/scheduleworld/sources/memo/config.ini:uri = note
sources/memo/config.ini:backend = memo
peers/scheduleworld/sources/memo/config.ini:# syncFormat = 
peers/scheduleworld/sources/memo/config.ini:# forceSyncFormat = 0
sources/memo/config.ini:# database = 
sources/memo/config.ini:# databaseFormat = 
sources/memo/config.ini:# databaseUser = 
sources/memo/config.ini:# databasePassword = 
peers/scheduleworld/sources/todo/.internal.ini:# adminData = 
peers/scheduleworld/sources/todo/.internal.ini:# synthesisID = 0
peers/scheduleworld/sources/todo/config.ini:sync = two-way
peers/scheduleworld/sources/todo/config.ini:uri = task2
sources/todo/config.ini:backend = todo
peers/scheduleworld/sources/todo/config.ini:# syncFormat = 
peers/scheduleworld/sources/todo/config.ini:# forceSyncFormat = 0
sources/todo/config.ini:# database = 
sources/todo/config.ini:# databaseFormat = 
sources/todo/config.ini:# databaseUser = 
sources/todo/config.ini:# databasePassword = '''.format(
           peerMinVersion, peerCurVersion,
           contextMinVersion, contextCurVersion,
           self.getSSLServerCertificates())

    def DefaultConfig(self):
        config = self.ScheduleWorldConfig()
        config = config.replace("syncURL = http://sync.scheduleworld.com/funambol/ds",
                                "syncURL = http://yourserver:port",
                                1)
        config = config.replace("http://www.scheduleworld.com",
                                "http://www.syncevolution.org",
                                1)
        config = config.replace("ScheduleWorld",
                                "SyncEvolution")
        config = config.replace("scheduleworld",
                                "syncevolution")
        config = config.replace("PeerName = SyncEvolution",
                                "# PeerName = ",
                                1)
        config = config.replace("# ConsumerReady = 0",
                                "ConsumerReady = 1",
                                1)
        config = config.replace("uri = card3",
                                "uri = addressbook",
                                1)
        config = config.replace("uri = cal2",
                                "uri = calendar",
                                1)
        config = config.replace("uri = task2",
                                "uri = todo",
                                1)
        config = config.replace("uri = note",
                                "uri = memo",
                                1)
        config = config.replace("syncFormat = text/vcard",
                                "# syncFormat = ",
                                1)
        return config

    def FunambolConfig(self):
        config = self.ScheduleWorldConfig()
        config = config.replace("/scheduleworld/",
                                "/funambol/")
        config = config.replace("PeerName = ScheduleWorld",
                                "PeerName = Funambol")
        config = config.replace("syncURL = http://sync.scheduleworld.com/funambol/ds",
                                "syncURL = http://my.funambol.com/sync",
                                1)
        config = config.replace("WebURL = http://www.scheduleworld.com",
                                "WebURL = http://my.funambol.com",
                                1)
        config = config.replace("IconURI = image://themedimage/icons/services/scheduleworld",
                                "IconURI = image://themedimage/icons/services/funambol",
                                1)
        config = config.replace("# ConsumerReady = 0",
                                "ConsumerReady = 1",
                                1)
        config = config.replace("# enableWBXML = 1",
                                "enableWBXML = 0",
                                1)
        config = config.replace("# RetryInterval = 2M",
                                "RetryInterval = 0",
                                1)
        config = config.replace("addressbook/config.ini:uri = card3",
                                "addressbook/config.ini:uri = card",
                                1)
        config = config.replace("addressbook/config.ini:syncFormat = text/vcard",
                                "addressbook/config.ini:# syncFormat = ")
        config = config.replace("calendar/config.ini:uri = cal2",
                                "calendar/config.ini:uri = event",
                                1)
        config = config.replace("calendar/config.ini:# syncFormat = ",
                                "calendar/config.ini:syncFormat = text/calendar")
        config = config.replace("calendar/config.ini:# forceSyncFormat = 0",
                                "calendar/config.ini:forceSyncFormat = 1")
        config = config.replace("todo/config.ini:uri = task2",
                                "todo/config.ini:uri = task",
                                1)
        config = config.replace("todo/config.ini:# syncFormat = ",
                                "todo/config.ini:syncFormat = text/calendar")
        config = config.replace("todo/config.ini:# forceSyncFormat = 0",
                                "todo/config.ini:forceSyncFormat = 1")
        return config

    def SynthesisConfig(self):
        config = self.ScheduleWorldConfig()
        config = config.replace("/scheduleworld/",
                                "/synthesis/")
        config = config.replace("PeerName = ScheduleWorld",
                                "PeerName = Synthesis")
        config = config.replace("syncURL = http://sync.scheduleworld.com/funambol/ds",
                                "syncURL = http://www.synthesis.ch/sync",
                                1)
        config = config.replace("WebURL = http://www.scheduleworld.com",
                                "WebURL = http://www.synthesis.ch",
                                1)
        config = config.replace("IconURI = image://themedimage/icons/services/scheduleworld",
                                "IconURI = image://themedimage/icons/services/synthesis",
                                1)
        config = config.replace("addressbook/config.ini:uri = card3",
                                "addressbook/config.ini:uri = contacts",
                                1)
        config = config.replace("addressbook/config.ini:syncFormat = text/vcard",
                                "addressbook/config.ini:# syncFormat = ")
        config = config.replace("calendar/config.ini:uri = cal2",
                                "calendar/config.ini:uri = events",
                                1)
        config = config.replace("calendar/config.ini:sync = two-way",
                                "calendar/config.ini:sync = disabled",
                                1)
        config = config.replace("memo/config.ini:uri = note",
                                "memo/config.ini:uri = notes",
                                1)
        config = config.replace("todo/config.ini:uri = task2",
                                "todo/config.ini:uri = tasks",
                                1)
        config = config.replace("todo/config.ini:sync = two-way",
                                "todo/config.ini:sync = disabled",
                                1)
        return config

    def OldScheduleWorldConfig(self):
        return '''spds/syncml/config.txt:syncURL = http://sync.scheduleworld.com/funambol/ds
spds/syncml/config.txt:# username = 
spds/syncml/config.txt:# password = 
spds/syncml/config.txt:# logdir = 
spds/syncml/config.txt:# loglevel = 0
spds/syncml/config.txt:# printChanges = 1
spds/syncml/config.txt:# dumpData = 1
spds/syncml/config.txt:# maxlogdirs = 10
spds/syncml/config.txt:# autoSync = 0
spds/syncml/config.txt:# autoSyncInterval = 30M
spds/syncml/config.txt:# autoSyncDelay = 5M
spds/syncml/config.txt:# preventSlowSync = 1
spds/syncml/config.txt:# useProxy = 0
spds/syncml/config.txt:# proxyHost = 
spds/syncml/config.txt:# proxyUsername = 
spds/syncml/config.txt:# proxyPassword = 
spds/syncml/config.txt:# clientAuthType = md5
spds/syncml/config.txt:# RetryDuration = 5M
spds/syncml/config.txt:# RetryInterval = 2M
spds/syncml/config.txt:# remoteIdentifier = 
spds/syncml/config.txt:# PeerIsClient = 0
spds/syncml/config.txt:# SyncMLVersion = 
spds/syncml/config.txt:PeerName = ScheduleWorld
spds/syncml/config.txt:deviceId = fixed-devid
spds/syncml/config.txt:# remoteDeviceId = 
spds/syncml/config.txt:# enableWBXML = 1
spds/syncml/config.txt:# maxMsgSize = 150000
spds/syncml/config.txt:# maxObjSize = 4000000
spds/syncml/config.txt:# SSLServerCertificates = {0}
spds/syncml/config.txt:# SSLVerifyServer = 1
spds/syncml/config.txt:# SSLVerifyHost = 1
spds/syncml/config.txt:WebURL = http://www.scheduleworld.com
spds/syncml/config.txt:IconURI = image://themedimage/icons/services/scheduleworld
spds/syncml/config.txt:# ConsumerReady = 0
spds/sources/addressbook/config.txt:sync = two-way
spds/sources/addressbook/config.txt:type = addressbook:text/vcard
spds/sources/addressbook/config.txt:evolutionsource = xyz
spds/sources/addressbook/config.txt:uri = card3
spds/sources/addressbook/config.txt:evolutionuser = foo
spds/sources/addressbook/config.txt:evolutionpassword = bar
spds/sources/calendar/config.txt:sync = two-way
spds/sources/calendar/config.txt:type = calendar
spds/sources/calendar/config.txt:# database = 
spds/sources/calendar/config.txt:uri = cal2
spds/sources/calendar/config.txt:# evolutionuser = 
spds/sources/calendar/config.txt:# evolutionpassword = 
spds/sources/memo/config.txt:sync = two-way
spds/sources/memo/config.txt:type = memo
spds/sources/memo/config.txt:# database = 
spds/sources/memo/config.txt:uri = note
spds/sources/memo/config.txt:# evolutionuser = 
spds/sources/memo/config.txt:# evolutionpassword = 
spds/sources/todo/config.txt:sync = two-way
spds/sources/todo/config.txt:type = todo
spds/sources/todo/config.txt:# database = 
spds/sources/todo/config.txt:uri = task2
spds/sources/todo/config.txt:# evolutionuser = 
spds/sources/todo/config.txt:# evolutionpassword = 
'''.format(self.getSSLServerCertificates())

    def replaceLineInConfig(self, config, begin, to):
        index = config.find(begin)
        self.assertNotEqual(index, -1)
        newline = config.find("\n", index + len(begin))
        self.assertNotEqual(newline, -1)
        return config[:index] + to + config[newline:]

    def removeRandomUUID(self, config):
        return self.replaceLineInConfig(config,
                                        "deviceId = syncevolution-",
                                        "deviceId = fixed-devid")

    def expectUsageError(self, out, err, specific_error):
        '''verify a short usage info was produced and specific error
        message was printed'''
        self.assertTrue(out.startswith("List databases:\n"))
        self.assertEqual(out.find("\nOptions:\n"), -1)
        self.assertTrue(out.endswith("Remove item(s):\n" \
                                     "  syncevolution --delete-items [--] <config> <source> (<luid> ... | '*')\n\n"))
        self.assertEqualDiff(specific_error, stripOutput(err))

    @property('debug', False)
    def testFramework(self):
        """TestCmdline.testFramework - tests whether utility functions work"""
        content = "baz:line\n" \
                  "caz/subdir:booh\n" \
                  "caz/subdir2/sub:# comment\n" \
                  "caz/subdir2/sub:# foo = bar\n" \
                  "caz/subdir2/sub:# empty = \n" \
                  "caz/subdir2/sub:# another comment\n" \
                  "foo:bar1\n" \
                  "foo:\n" \
                  "foo: \n" \
                  "foo:bar2\n"

        filtered = "baz:line\n" \
                   "caz/subdir:booh\n" \
                   "caz/subdir2/sub:# foo = bar\n" \
                   "caz/subdir2/sub:# empty = \n" \
                   "foo:bar1\n" \
                   "foo: \n" \
                   "foo:bar2\n"

        createFiles(self.configdir, content)
        res = scanFiles(self.configdir)
        self.assertEqualDiff(filtered, res)
        randomUUID = "deviceId = syncevolution-blabla\n"
        fixedUUID = "deviceId = fixed-devid\n"
        res = self.removeRandomUUID(randomUUID)
        self.assertEqual(fixedUUID, res)

        unsorted = "f:g\n" \
                   "f:j\n" \
                   "a:b\n" \
                   "f:a\n" \
                   "a/b:a\n"
        expected = "a:b\n" \
                   "a/b:a\n" \
                   "f:g\n" \
                   "f:j\n" \
                   "f:a\n"
        res = sortConfig(unsorted)
        self.assertEqualDiff(expected, res)

        # test DBusUtil.assertEqualDiff()
        try:
            self.assertEqualDiff('foo\nbar\n', 'foo\nxxx\nbar\n')
        except AssertionError, ex:
            expected = '''differences between expected and actual text

  foo
+ xxx
  bar
'''
            self.assertTrue(str(ex).endswith(expected), 'actual exception differs\n' + str(ex))
        else:
            self.fail('''DBusUtil.assertEqualDiff() did not detect diff''')

        self.assertEqualDiff('foo\nbar', [ 'foo\n', 'bar' ])
        self.assertEqualDiff([ 'foo\n', 'bar' ], 'foo\nbar')
        self.assertEqualDiff([ 'foo\n', 'bar' ], [ 'foo\n', 'bar' ])

        # test our own regex match
        self.assertRegexpMatchesCustom('foo\nbar\nend', 'bar')
        self.assertRegexpMatchesCustom('foo\nbar\nend', 'b.r')
        self.assertRegexpMatchesCustom('foo\nbar\nend', re.compile('^b.r$', re.MULTILINE))
        try:
            self.assertRegexpMatchesCustom('foo\nbar\nend', 'xxx')
        except AssertionError, ex:
            expected = '''text does not match regex\n\nText:\nfoo\nbar\nend\n\nRegex:\nxxx'''
            self.assertTrue(str(ex).endswith(expected), 'actual exception differs\n' + str(ex))
        else:
            self.fail('''DBusUtil.assertRegexpMatchesCustom() did not fail''')
        self.assertRegexpMatches('foo\nbar\nend', 'bar')

        haystack = {'in': None}

        self.assertInCustom('in', 'inside')
        self.assertNotInCustom('in', 'outside')
        self.assertInCustom('in', haystack)
        self.assertNotIn('out', haystack)

        try:
            self.assertInCustom('in', 'outside')
        except AssertionError, ex:
            expected = "'in' not found in 'outside'"
            self.assertEqual(expected, str(ex))
        else:
            self.fail('''DBusUtil.assertInCustom() did not fail''')

        try:
            self.assertInCustom('out', haystack)
        except AssertionError, ex:
            expected = "'out' not found in '{'in': None}'"
            self.assertEqual(expected, str(ex))
        else:
            self.fail('''DBusUtil.assertInCustom() did not fail''')

        try:
            self.assertNotInCustom('in', 'inside')
        except AssertionError, ex:
            expected = "'in' found in 'inside'"
            self.assertEqual(expected, str(ex))
        else:
            self.fail('''DBusUtil.assertNotInCustom() did not fail''')

        try:
            self.assertNotInCustom('in', haystack)
        except AssertionError, ex:
            expected = "'in' found in '{'in': None}'"
            self.assertEqual(expected, str(ex))
        else:
            self.fail('''DBusUtil.assertNotInCustom() did not fail''')

        lines = "a\nb\nc\n"
        lastline = "c\n"
        res = lastLine(lines)
        self.assertEqualDiff(lastline, res)

        message = "[ERROR 12:34:56] msg\n"
        stripped = "[ERROR] msg\n"
        res = stripOutput(message)
        self.assertEqualDiff(stripped, res)

        # Run command without talking to server, separate streams.
        out, err, code = self.runCmdline(['--foo-bar'],
                                         sessionFlags=None,
                                         expectSuccess=False)
        self.assertEqualDiff('[ERROR] --foo-bar: unknown parameter\n', stripOutput(err))
        self.assertRegexpMatches(out, '^List databases:\n')
        self.assertEqual(1, code)

        # Run command without talking to server, joined streams.
        out, err, code = self.runCmdline(['--foo-bar'],
                                         sessionFlags=None,
                                         expectSuccess=False,
                                         preserveOutputOrder=True)
        self.assertEqual(err, None)
        self.assertRegexpMatches(stripOutput(out), r'^List databases:\n(.*\n)*\[ERROR\] --foo-bar: unknown parameter\n$')
        self.assertEqual(1, code)

        peerMin = self.getPeerMinVersion()
        peerCur = self.getPeerCurVersion()
        contextMin = self.getContextMinVersion()
        contextCur = self.getContextCurVersion()
        rootMin = self.getRootMinVersion()
        rootCur = self.getRootCurVersion()

        self.assertRegexpMatches(peerMin, r'^\d+$', 'Peer min version is not a number.')
        self.assertRegexpMatches(peerCur, r'^\d+$', 'Peer cur version is not a number.')
        self.assertRegexpMatches(contextMin, r'^\d+$', 'Context min version is not a number.')
        self.assertRegexpMatches(contextCur, r'^\d+$', 'Context cur version is not a number.')
        self.assertRegexpMatches(rootMin, r'^\d+$', 'Peer min version is not a number.')
        self.assertRegexpMatches(rootCur, r'^\d+$', 'Root cur version is not a number.')

    def assertSilent(self, out, err):
        if err != None and \
                os.environ.get('SYNCEVOLUTION_DEBUG', None) == None:
            self.assertNoErrors(err)
        self.assertEqualDiff('', out)

    def doSetupScheduleWorld(self, shared):
        root = self.configdir + "/default"
        peer = ""

        if shared:
            peer = root + "/peers/scheduleworld"
        else:
            peer = root

        shutil.rmtree(peer, True)
        out, err, code = self.runCmdline(['--configure',
                                          '--sync-property', 'proxyHost = proxy',
                                          'scheduleworld', 'addressbook'])
        self.assertSilent(out, err)
        res = sortConfig(scanFiles(root))
        res = self.removeRandomUUID(res)
        expected = self.ScheduleWorldConfig()
        expected = sortConfig(expected)
        expected = expected.replace("# proxyHost = ",
                                    "proxyHost = proxy",
                                    1)
        expected = expected.replace("sync = two-way",
                                    "sync = disabled")
        expected = expected.replace("addressbook/config.ini:sync = disabled",
                                    "addressbook/config.ini:sync = two-way",
                                    1)
        self.assertEqualDiff(expected, res)

        shutil.rmtree(peer, True)
        out, err, code = self.runCmdline(['--configure',
                                          '--sync-property', 'deviceId = fixed-devid',
                                          'scheduleworld'])
        self.assertSilent(out, err)
        res = sortConfig(scanFiles(root))
        expected = self.ScheduleWorldConfig()
        expected = sortConfig(expected)
        self.assertEqualDiff(expected, res)

    @property('debug', False)
    def testSetupScheduleWorld(self):
        """TestCmdline.testSetupScheduleWorld - configure ScheduleWorld"""
        self.doSetupScheduleWorld(False)

    @property("debug", False)
    def testSetupDefault(self):
        """TestCmdline.testSetupDefault - configure Default"""
        root = self.configdir + '/default'
        out, err, code = self.runCmdline(['--configure',
                                          '--template', 'default',
                                          '--sync-property', 'deviceId = fixed-devid',
                                          'some-other-server'])
        self.assertSilent(out, err)
        res = scanFiles(root, 'some-other-server')
        expected = sortConfig(self.DefaultConfig()).replace('/syncevolution/', '/some-other-server/')
        self.assertEqualDiff(expected, res)

    @property("debug", False)
    def testSetupRenamed(self):
        """TestCmdline.testSetupRenamed - configure ScheduleWorld with other name"""
        root = self.configdir + "/default"
        args = ["--configure",
                "--template",
                "scheduleworld",
                "--sync-property",
                "deviceId = fixed-devid",
                "scheduleworld2"]
        out, err, code = self.runCmdline(["--configure",
                                          "--template", "scheduleworld",
                                          "--sync-property", "deviceId = fixed-devid",
                                          "scheduleworld2"])
        self.assertSilent(out, err)
        res = scanFiles(root, "scheduleworld2")
        expected = sortConfig(self.ScheduleWorldConfig()).replace("/scheduleworld/", "/scheduleworld2/")
        self.assertEqualDiff(expected, res)

    def doSetupFunambol(self, shared):
        root = self.configdir + "/default"
        peer = ""
        args = ["--configure"]

        if shared:
            peer = root + "/peers/funambol"
        else:
            peer = root
            args.extend(["--sync-property", "deviceId = fixed-devid"])
        shutil.rmtree(peer, True)

        args.append("FunamBOL")
        out, err, code = self.runCmdline(args)
        self.assertSilent(out, err)
        res = scanFiles(root, "funambol")
        expected = sortConfig(self.FunambolConfig())
        self.assertEqualDiff(expected, res)

    @property("debug", False)
    def testSetupFunambol(self):
        """TestCmdline.testSetupFunambol - configure Funambol"""
        self.doSetupFunambol(False)

    def doSetupSynthesis(self, shared):
        root = self.configdir + "/default"
        peer = ""
        args = ["--configure"]

        if shared:
            peer = root + "/peers/synthesis"
        else:
            peer = root
            args.extend(["--sync-property", "deviceId = fixed-devid"])
        shutil.rmtree(peer, True)

        args.append("synthesis")
        out, err, code = self.runCmdline(args)
        self.assertSilent(out, err)
        res = scanFiles(root, "synthesis")
        expected = sortConfig(self.SynthesisConfig())
        self.assertEqualDiff(expected, res)

    @property("debug", False)
    def testSetupSynthesis(self):
        """TestCmdline.testSetupSynthesis - configure Synthesis"""
        self.doSetupSynthesis(False)

    @property("debug", False)
    def testTemplate(self):
        """TestCmdline.testTemplate - check --template parameter"""
        out, err, code = self.runCmdline(['--template'],
                                         sessionFlags=None,
                                         expectSuccess = False)
        self.expectUsageError(out, err,
                              "[ERROR] missing parameter for '--template'\n")

        expected = "Available configuration templates (servers):\n" \
                   "   template name = template description\n" \
                   "   eGroupware = http://www.egroupware.org\n" \
                   "   Funambol = http://my.funambol.com\n" \
                   "   Google_Calendar = event sync via CalDAV, use for the 'target-config@google-calendar' config\n" \
                   "   Google_Contacts = contact sync via SyncML, see http://www.google.com/support/mobile/bin/topic.py?topic=22181\n" \
                   "   Goosync = http://www.goosync.com/\n" \
                   "   Memotoo = http://www.memotoo.com\n" \
                   "   Mobical = https://www.everdroid.com\n" \
                   "   Oracle = http://www.oracle.com/technology/products/beehive/index.html\n" \
                   "   Ovi = http://www.ovi.com\n" \
                   "   ScheduleWorld = server no longer in operation\n" \
                   "   SyncEvolution = http://www.syncevolution.org\n" \
                   "   Synthesis = http://www.synthesis.ch\n" \
                   "   WebDAV = contact and event sync using WebDAV, use for the 'target-config@<server>' config\n" \
                   "   Yahoo = contact and event sync using WebDAV, use for the 'target-config@yahoo' config\n"
        out, err, code = self.runCmdline(["--template", "? "])
        self.assertEqualDiff(expected, out)
        self.assertNoErrors(err)

    @property("ENV", "SYNCEVOLUTION_TEMPLATE_DIR=" + xdg_root + "/templates")
    @property("debug", False)
    def testMatchTemplate(self):
        """TestCmdline.testMatchTemplate - test template matching"""
        env = copy.deepcopy(self.storedenv)
        env["XDG_CONFIG_HOME"] = "/dev/null"
        self.setUpFiles('templates')
        out, err, code = self.runCmdline(["--template", "?nokia 7210c"], env)
        expected = "Available configuration templates (clients):\n" \
                   "   template name = template description    matching score in percent (100% = exact match)\n" \
                   "   Nokia_7210c = Template for Nokia S40 series Phone    100%\n" \
                   "   SyncEvolution_Client = SyncEvolution server side template    40%\n"
        self.assertEqualDiff(expected, out)
        self.assertNoErrors(err)

        out, err, code = self.runCmdline(["--template", "?nokia"], env)
        self.assertEqualDiff(expected, out)
        self.assertNoErrors(err)

        out, err, code = self.runCmdline(["--template", "?7210c"], env)
        expected = "Available configuration templates (clients):\n" \
                   "   template name = template description    matching score in percent (100% = exact match)\n" \
                   "   Nokia_7210c = Template for Nokia S40 series Phone    60%\n" \
                   "   SyncEvolution_Client = SyncEvolution server side template    20%\n"
        self.assertEqualDiff(expected, out)
        self.assertNoErrors(err)

        out, err, code = self.runCmdline(["--template", "?syncevolution client"], env)
        expected = "Available configuration templates (clients):\n" \
                   "   template name = template description    matching score in percent (100% = exact match)\n" \
                   "   SyncEvolution_Client = SyncEvolution server side template    100%\n" \
                   "   Nokia_7210c = Template for Nokia S40 series Phone    40%\n"
        self.assertEqualDiff(expected, out)
        self.assertNoErrors(err)

    @property("debug", False)
    def testPrintServers(self):
        """TestCmdline.testPrintServers - print correct servers"""

        self.doSetupScheduleWorld(False)
        self.doSetupSynthesis(True)
        self.doSetupFunambol(True)

        out, err, code = self.runCmdline(["--print-servers"])
        expected = "Configured servers:\n" \
                   "   funambol = " + os.path.abspath(self.configdir) + "/default/peers/funambol\n" \
                   "   scheduleworld = " + os.path.abspath(self.configdir) + "/default/peers/scheduleworld\n" \
                   "   synthesis = " + os.path.abspath(self.configdir) + "/default/peers/synthesis\n"
        self.assertEqualDiff(expected, out)
        self.assertNoErrors(err)

    @property("debug", False)
    def testPrintConfig(self):
        """TestCmdline.testPrintConfig - print various configurations"""
        self.doSetupFunambol(False)

        out, err, code = self.runCmdline(["--print-config"],
                                         expectSuccess = False)
        self.expectUsageError(out, err,
                              "[ERROR] --print-config requires either a --template or a server name.\n")

        out, err, code = self.runCmdline(["--print-config", "foo"],
                                         expectSuccess = False)
        self.assertEqualDiff('', out)
        self.assertEqualDiff("[ERROR] Server 'foo' has not been configured yet.\n",
                             stripOutput(err))

        out, err, code = self.runCmdline(["--print-config", "--template", "foo"],
                                         expectSuccess = False)
        self.assertEqualDiff("", out)
        self.assertEqualDiff("[ERROR] No configuration template for 'foo' available.\n",
                             stripOutput(err))

        out, err, code = self.runCmdline(["--print-config", "--template", "scheduleworld"])
        self.assertEqualDiff("", err)
        # deviceId must be the one from Funambol
        self.assertIn("deviceId = fixed-devid", out)
        filtered = injectValues(filterConfig(out))
        self.assertEqualDiff(filterConfig(internalToIni(self.ScheduleWorldConfig())),
                             filtered)
        # there should have been comments
        self.assertTrue(len(out) > len(filtered))

        out, err, code = self.runCmdline(["--print-config", "--template", "scheduleworld@nosuchcontext"])
        self.assertEqualDiff("", err)
        # deviceId must *not* be the one from Funambol because of the new context
        self.assertNotIn("deviceId = fixed-devid", out)

        out, err, code = self.runCmdline(["--print-config", "--template", "Default"])
        self.assertEqualDiff("", err)
        actual = injectValues(filterConfig(out))
        self.assertIn("deviceId = fixed-devid", actual)
        self.assertEqualDiff(filterConfig(internalToIni(self.DefaultConfig())),
                             actual)

        out, err, code = self.runCmdline(["--print-config", "funambol"])
        self.assertEqualDiff("", err)
        self.assertEqualDiff(filterConfig(internalToIni(self.FunambolConfig())),
                             injectValues(filterConfig(out)))

        # override context and template properties
        out, err, code = self.runCmdline(["--print-config", "--template", "scheduleworld",
                                          "syncURL=foo",
                                          "database=Personal",
                                          "--source-property", "sync=disabled"])
        self.assertNoErrors(err)
        expected = filterConfig(internalToIni(self.ScheduleWorldConfig()))
        expected = expected.replace("syncURL = http://sync.scheduleworld.com/funambol/ds",
                                    "syncURL = foo",
                                    1)
        expected = expected.replace("# database = ",
                                    "database = Personal")
        expected = expected.replace("sync = two-way",
                                    "sync = disabled")
        actual = injectValues(filterConfig(out))
        self.assertIn("deviceId = fixed-devid", actual)
        self.assertEqualDiff(expected, actual)

        # override context and template properties, using legacy
        # property name
        out, err, code = self.runCmdline(["--print-config", "--template", "scheduleworld",
                                          "--sync-property", "syncURL=foo",
                                          "--source-property", "evolutionsource=Personal",
                                          "--source-property", "sync=disabled"])
        self.assertNoErrors(err)
        expected = filterConfig(internalToIni(self.ScheduleWorldConfig()))
        expected = expected.replace("syncURL = http://sync.scheduleworld.com/funambol/ds",
                                    "syncURL = foo",
                                    1)
        expected = expected.replace("# database = ",
                                    "database = Personal")
        expected = expected.replace("sync = two-way",
                                    "sync = disabled")
        actual = injectValues(filterConfig(out))
        self.assertIn("deviceId = fixed-devid", actual)
        self.assertEqualDiff(expected,
                             actual)

        out, err, code = self.runCmdline(["--print-config", "--quiet",
                                          "--template", "scheduleworld",
                                          "funambol"])
        self.assertEqualDiff("", err)
        self.assertIn("deviceId = fixed-devid", out)
        self.assertEqualDiff(internalToIni(self.ScheduleWorldConfig()),
                             injectValues(filterConfig(out)))

        # change shared source properties, then check template again
        out, err, code = self.runCmdline(["--configure",
                                          "--source-property", "database=Personal",
                                          "funambol"])
        self.assertEqualDiff("", err)

        out, err, code = self.runCmdline(["--print-config", "--quiet",
                                          "--template", "scheduleworld",
                                          "funambol"])
        self.assertEqualDiff("", err)
        # from modified Funambol config
        expected = filterConfig(internalToIni(self.ScheduleWorldConfig())).replace("# database = ",
                                                                                   "database = Personal")
        actual = injectValues(filterConfig(out))
        self.assertIn("deviceId = fixed-devid", actual)
        self.assertEqualDiff(expected, actual)

        # print config => must not use settings from default context
        out, err, code = self.runCmdline(["--print-config", "--template", "scheduleworld@nosuchcontext"])
        self.assertEqualDiff("", err)
        expected = filterConfig(internalToIni(self.ScheduleWorldConfig()))
        actual = injectValues(filterConfig(out))
        # source settings *not* from modified Funambol config
        self.assertNotIn("deviceId = fixed-devid", actual)
        actual = self.removeRandomUUID(actual)
        self.assertEqualDiff(expected, actual)

        # create config => again, must not use settings from default context
        out, err, code = self.runCmdline(["--configure", "--template", "scheduleworld", "other@other"])
        self.assertEqualDiff("", err)

        out, err, code = self.runCmdline(["--print-config", "other@other"])
        self.assertEqualDiff("", err)
        # source settings *not* from modified Funambol config
        expected = filterConfig(internalToIni(self.ScheduleWorldConfig()))
        actual = injectValues(filterConfig(out))
        self.assertNotIn("deviceId = fixed-devid", actual)
        actual = self.removeRandomUUID(actual)
        self.assertEqualDiff(expected, actual)

    def doPrintFileTemplates(self):
        '''Compare only the properties which are really set'''

        # note that "backend" will be taken from the @default context
        # if one exists, so run this before setting up Funambol below
        out, err, code = self.runCmdline(["--print-config", "--template", "google calendar"])
        self.assertNoErrors(err)
        self.assertEqualDiff(googlecaldav,
                             removeComments(self.removeRandomUUID(filterConfig(out))))

        out, err, code = self.runCmdline(["--print-config", "--template", "yahoo"])
        self.assertNoErrors(err)
        self.assertEqualDiff(yahoo,
                             removeComments(self.removeRandomUUID(filterConfig(out))))

        self.doSetupFunambol(False)

        out, err, code = self.runCmdline(["--print-config", "--template", "scheduleworld"])
        self.assertNoErrors(err)
        # deviceId must be the one from Funambol
        self.assertIn("deviceId = fixed-devid", out)
        filtered = injectValues(filterConfig(out))
        self.assertEqualDiff(filterConfig(internalToIni(self.ScheduleWorldConfig())),
                             filtered)
        # there should have been comments
        self.assertTrue(len(out) > len(filtered))

        out, err, code = self.runCmdline(["--print-config", "funambol"])
        self.assertNoErrors(err)
        self.assertEqualDiff(filterConfig(internalToIni(self.FunambolConfig())),
                             injectValues(filterConfig(out)))

    # Use local copy of templates in build dir (no need to install);
    # this assumes that test-dbus.py is run in the build "src"
    # directory.
    @property("ENV", "SYNCEVOLUTION_TEMPLATE_DIR=./templates")
    @property("debug", False)
    def testPrintFileTemplates(self):
        """TestCmdline.testPrintFileTemplates - print file templates"""
        self.doPrintFileTemplates()

    # Disable reading default templates, rely on finding the user
    # ones (set up below via symlink).
    @property("ENV", "SYNCEVOLUTION_TEMPLATE_DIR=/dev/null")
    @property("debug", False)
    def testPrintFileTemplatesConfig(self):
        """TestCmdline.testPrintFileTemplatesConfig - print file templates from user home directory"""
        # The user's "home directory" or more precisely, his
        # XDG_CONFIG_HOME, is set to xdg_root + "config". Make sure
        # that there is a "syncevolution-templates" with valid
        # templates in that "config" directory, because "templates"
        # will not be found otherwise (SYNCEVOLUTION_TEMPLATE_DIR
        # doesn't point to it).
        os.makedirs(xdg_root + "/config")
        # Use same "./templates" as in testPrintFileTemplates().
        os.symlink("../../templates", xdg_root + "/config/syncevolution-templates")
        self.doPrintFileTemplates()

    @property("debug", False)
    def testAddSource(self):
        '''TestCmdline.testAddSource - add a source'''
        self.doSetupScheduleWorld(False)
        out, err, code = self.runCmdline(["--configure",
                                          "--source-property", "uri = dummy",
                                          "scheduleworld",
                                          "xyz"])
        res = scanFiles(self.configdir + "/default")
        expected = sortConfig(self.ScheduleWorldConfig() + """
peers/scheduleworld/sources/xyz/.internal.ini:# adminData = 
peers/scheduleworld/sources/xyz/.internal.ini:# synthesisID = 0
peers/scheduleworld/sources/xyz/config.ini:# sync = disabled
peers/scheduleworld/sources/xyz/config.ini:uri = dummy
peers/scheduleworld/sources/xyz/config.ini:# syncFormat = 
peers/scheduleworld/sources/xyz/config.ini:# forceSyncFormat = 0
sources/xyz/config.ini:# backend = select backend
sources/xyz/config.ini:# database = 
sources/xyz/config.ini:# databaseFormat = 
sources/xyz/config.ini:# databaseUser = 
sources/xyz/config.ini:# databasePassword = """)
        self.assertEqualDiff(expected, res)

    @property("debug", False)
    def testSync(self):
        """TestCmdline.testSync - check sync with various options"""
        out, err, code = self.runCmdline(["--sync"],
                                         sessionFlags=None,
                                         expectSuccess = False)
        self.expectUsageError(out, err,
                              "[ERROR] missing parameter for '--sync'\n")

        out, err, code = self.runCmdline(["--sync", "foo"],
                                         sessionFlags=None,
                                         expectSuccess = False)
        self.assertEqualDiff('', out)
        self.assertEqualDiff("[ERROR] '--sync foo': not one of the valid values (two-way, slow, refresh-from-local, refresh-from-remote = refresh, one-way-from-local, one-way-from-remote = one-way, refresh-from-client = refresh-client, refresh-from-server = refresh-server, one-way-from-client = one-way-client, one-way-from-server = one-way-server, disabled = none)\n",
                             stripOutput(err))

        out, err, code = self.runCmdline(["--sync", " ?"],
                                         sessionFlags=None)
        self.assertEqualDiff("""--sync
   Requests a certain synchronization mode when initiating a sync:
   
     two-way
       only send/receive changes since last sync
     slow
       exchange all items
     refresh-from-remote
       discard all local items and replace with
       the items on the peer
     refresh-from-local
       discard all items on the peer and replace
       with the local items
     one-way-from-remote
       transmit changes from peer
     one-way-from-local
       transmit local changes
     disabled (or none)
       synchronization disabled
   
   refresh/one-way-from-server/client are also supported. Their use is
   discouraged because the direction of the data transfer depends
   on the role of the local side (can be server or client), which is
   not always obvious.
   
   When accepting a sync session in a SyncML server (HTTP server), only
   sources with sync != disabled are made available to the client,
   which chooses the final sync mode based on its own configuration.
   When accepting a sync session in a SyncML client (local sync with
   the server contacting SyncEvolution on a device), the sync mode
   specified in the client is typically overriden by the server.
""",
                             out)
        self.assertEqualDiff("", err)

        out, err, code = self.runCmdline(["--sync", "refresh-from-server"],
                                         sessionFlags=None,
                                         expectSuccess = False)
        self.expectUsageError(out, err,
                              "[ERROR] No configuration name specified.\n")

        out, err, code = self.runCmdline(["--source-property", "sync=refresh"],
                                         sessionFlags=None,
                                         expectSuccess = False)
        self.expectUsageError(out, err,
                              "[ERROR] No configuration name specified.\n")

        out, err, code = self.runCmdline(["--source-property", "xyz=1"],
                                         sessionFlags=None,
                                         expectSuccess = False)
        self.assertEqualDiff('', out)
        self.assertEqualDiff("[ERROR] '--source-property xyz=1': no such property\n",
                             stripOutput(err))

        out, err, code = self.runCmdline(["xyz=1"],
                                         sessionFlags=None,
                                         expectSuccess = False)
        self.expectUsageError(out, err,
                              "[ERROR] unrecognized property in 'xyz=1'\n")

        out, err, code = self.runCmdline(["=1"],
                                         sessionFlags=None,
                                         expectSuccess = False)
        self.expectUsageError(out, err,
                              "[ERROR] a property name must be given in '=1'\n")

    # TODO: scan output from "backend=?" to determine whether CalDAV/CardDAV are enabled
    def isWebDAVEnabled(self):
        '''Checks config.h for existence of '#define ENABLE_DAV' or
        '/* #undef ENABLE_DAV*/'. It assumes that the test is being
        run in $(top_builddir)/src, which is the same assumption like
        some tests already have.'''
        configfile = open('../config.h', 'r')

        for line in configfile:
            if line == '#define ENABLE_DAV 1\n':
                return True
            if line == '/* #undef ENABLE_DAV */\n':
                return False
        self.fail('Could not find out whether DAV is enabled or not.')

    @property("debug", False)
    def testWebDAV(self):
        """TestCmdline.testWebDAV - configure and print WebDAV configs"""

        # configure Yahoo under a different name, with explicit
        # template selection
        out, err, code = self.runCmdline(["--configure",
                                          "--template", "yahoo",
                                          "target-config@my-yahoo"])
        self.assertSilent(out, err)

        out, err, code = self.runCmdline(["--print-config", "target-config@my-yahoo"])
        self.assertNoErrors(err)
        davenabled = self.isWebDAVEnabled()
        if davenabled:
            self.assertEqualDiff(yahoo,
                                 removeComments(self.removeRandomUUID(filterConfig(out))))
        else:
            self.assertEqualDiff(yahoo.replace("sync = two-way", "sync = disabled"),
                                 removeComments(self.removeRandomUUID(filterConfig(out))))

        # configure Google Calendar with template derived from config name
        out, err, code = self.runCmdline(["--configure",
                                          "target-config@google-calendar"])
        self.assertSilent(out, err)

        out, err, code = self.runCmdline(["--print-config", "target-config@google-calendar"])
        self.assertNoErrors(err)
        if davenabled:
            self.assertEqualDiff(googlecaldav,
                                 removeComments(self.removeRandomUUID(filterConfig(out))))
        else:
            self.assertEqualDiff(googlecaldav.replace("sync = two-way", "sync = disabled"),
                                 removeComments(self.removeRandomUUID(filterConfig(out))))

        # test "template not found" error cases
        out, err, code = self.runCmdline(["--configure",
                                          "--template", "yahooxyz",
                                          "target-config@my-yahoo-xyz"],
                                         expectSuccess = False)
        error = """[ERROR] No configuration template for 'yahooxyz' available.
[INFO] 
[INFO] Available configuration templates (clients and servers):
"""
        self.assertEqualDiff('', out)
        self.assertTrue(err.startswith(error))
        self.assertTrue(err.endswith("\n"))
        self.assertFalse(err.endswith("\n\n"))

        out, err, code = self.runCmdline(["--configure",
                                          "target-config@foobar"],
                                         expectSuccess = False)
        error = """[ERROR] No configuration template for 'foobar' available.
[INFO] Use '--template none' and/or specify relevant properties on the command line to create a configuration without a template. Need values for: syncURL
[INFO] 
[INFO] Available configuration templates (clients and servers):
"""

        self.assertEqualDiff('', out)
        self.assertTrue(err.startswith(error))
        self.assertTrue(err.endswith("\n"))
        self.assertFalse(err.endswith("\n\n"))

    def printConfig(self, server):
        out, err, code = self.runCmdline(["--print-config", server])
        self.assertNoErrors(err)
        return out

    def doConfigure(self, config, prefix):
        out, err, code = self.runCmdline(["--configure",
                                          "--source-property", "sync = disabled",
                                          "scheduleworld"])
        self.assertSilent(out, err)
        expected = filterConfig(internalToIni(config)).replace("sync = two-way",
                                                               "sync = disabled")
        self.assertEqualDiff(expected,
                             filterConfig(self.printConfig("scheduleworld")))

        out, err, code = self.runCmdline(["--configure",
                                          "--source-property", "sync = one-way-from-server",
                                          "scheduleworld",
                                          "addressbook"])
        self.assertSilent(out, err)
        expected = config.replace("sync = two-way",
                                  "sync = disabled")
        expected = expected.replace(prefix + "sync = disabled",
                                    prefix + "sync = one-way-from-server",
                                    1)
        expected = filterConfig(internalToIni(expected))
        self.assertEqualDiff(expected,
                             filterConfig(self.printConfig("scheduleworld")))

        out, err, code = self.runCmdline(["--configure",
                                          "--sync", "two-way",
                                          "-z", "database=source",
                                          # note priority of suffix: most specific wins
                                          "--sync-property", "maxlogdirs@scheduleworld@default=20",
                                          "--sync-property", "maxlogdirs@default=10",
                                          "--sync-property", "maxlogdirs=5",
                                          "-y", "LOGDIR@default=logdir",
                                          "scheduleworld"])
        self.assertSilent(out, err)
        expected = expected.replace("sync = one-way-from-server",
                                    "sync = two-way")
        expected = expected.replace("sync = disabled",
                                    "sync = two-way")
        expected = expected.replace("# database = ",
                                    "database = source")
        expected = expected.replace("database = xyz",
                                    "database = source")
        expected = expected.replace("# maxlogdirs = 10",
                                    "maxlogdirs = 20")
        expected = expected.replace("# logdir = ",
                                    "logdir = logdir")
        self.assertEqualDiff(expected,
                             filterConfig(self.printConfig("scheduleworld")))
        return expected

    @property("debug", False)
    def testConfigure(self):
        """TestCmdline.testConfigure - run configures"""
        self.doSetupScheduleWorld(False)

        expected = self.doConfigure(self.ScheduleWorldConfig(), "sources/addressbook/config.ini:")

        # using "type" for peer is mapped to updating "backend",
        # "databaseFormat", "syncFormat", "forceSyncFormat"
        out, err, code = self.runCmdline(["--configure",
                                          "--source-property", "addressbook/type=file:text/vcard:3.0",
                                          "scheduleworld"])
        self.assertSilent(out, err)
        expected = expected.replace("backend = addressbook",
                                    "backend = file",
                                    1)
        expected = expected.replace("# databaseFormat = ",
                                    "databaseFormat = text/vcard",
                                    1)
        expected = expected.replace("# forceSyncFormat = 0",
                                    "forceSyncFormat = 0",
                                    1)
        self.assertEqualDiff(expected,
                             filterConfig(self.printConfig("scheduleworld")))
        shared = filterConfig(self.printConfig("@default"))
        self.assertIn("backend = file", shared)
        self.assertIn("databaseFormat = text/vcard", shared)

        # updating type for context must not affect peer
        out, err, code = self.runCmdline(["--configure",
                                          "--source-property", "type=file:text/x-vcard:2.1",
                                          "@default", "addressbook"])
        self.assertSilent(out, err)
        expected = expected.replace("databaseFormat = text/vcard",
                                    "databaseFormat = text/x-vcard",
                                    1)
        self.assertEqualDiff(expected,
                             filterConfig(self.printConfig("scheduleworld")))
        shared = filterConfig(self.printConfig("@default"))
        self.assertIn("backend = file", shared)
        self.assertIn("databaseFormat = text/x-vcard", shared)

        syncproperties = """syncURL (no default, unshared, required)

username (no default, unshared)

password (no default, unshared)

logdir (no default, shared)

loglevel (0, unshared)

printChanges (TRUE, unshared)

dumpData (TRUE, unshared)

maxlogdirs (10, shared)

autoSync (0, unshared)

autoSyncInterval (30M, unshared)

autoSyncDelay (5M, unshared)

preventSlowSync (TRUE, unshared)

useProxy (FALSE, unshared)

proxyHost (no default, unshared)

proxyUsername (no default, unshared)

proxyPassword (no default, unshared)

clientAuthType (md5, unshared)

RetryDuration (5M, unshared)

RetryInterval (2M, unshared)

remoteIdentifier (no default, unshared)

PeerIsClient (FALSE, unshared)

SyncMLVersion (no default, unshared)

PeerName (no default, unshared)

deviceId (no default, shared)

remoteDeviceId (no default, unshared)

enableWBXML (TRUE, unshared)

maxMsgSize (150000, unshared), maxObjSize (4000000, unshared)

SSLServerCertificates ({0}, unshared)

SSLVerifyServer (TRUE, unshared)

SSLVerifyHost (TRUE, unshared)

WebURL (no default, unshared)

IconURI (no default, unshared)

ConsumerReady (FALSE, unshared)

peerType (no default, unshared)

defaultPeer (no default, global)
""".format(self.getSSLServerCertificates())

        sourceproperties = """sync (disabled, unshared, required)

uri (no default, unshared)

backend (select backend, shared)

syncFormat (no default, unshared)

forceSyncFormat (FALSE, unshared)

database = evolutionsource (no default, shared)

databaseFormat (no default, shared)

databaseUser = evolutionuser (no default, shared), databasePassword = evolutionpassword (no default, shared)
"""

        # The WORKAROUND lines remove trailing newline from expected
        # output.  This should be fixed in Cmdline, I guess. For now I
        # adapted the test to actual output to check if there are
        # other errors.

        out, err, code = self.runCmdline(["--sync-property", "?"],
                                         sessionFlags=None)
        self.assertNoErrors(err)
        # WORKAROUND-----------------------vvvvv
        self.assertEqualDiff(syncproperties[:-1],
                             filterIndented(out))

        out, err, code = self.runCmdline(["--source-property", "?"],
                                         sessionFlags=None)
        self.assertNoErrors(err)
        # WORKAROUND-------------------------vvvvv
        self.assertEqualDiff(sourceproperties[:-1],
                             filterIndented(out))

        out, err, code = self.runCmdline(["--source-property", "?",
                                          "--sync-property", "?"],
                                         sessionFlags=None)
        self.assertNoErrors(err)
        # WORKAROUND------------------------------------------vvvvv
        self.assertEqualDiff(sourceproperties + syncproperties[:-1],
                             filterIndented(out))

        out, err, code = self.runCmdline(["--sync-property", "?",
                                          "--source-property", "?"],
                                         sessionFlags=None)
        self.assertNoErrors(err)
        # WORKAROUND------------------------------------------vvvvv
        self.assertEqualDiff(syncproperties + sourceproperties[:-1],
                             filterIndented(out))

        out, err, code = self.runCmdline(["--source-property", "sync=?"],
                                         sessionFlags=None)
        self.assertNoErrors(err)
        # WORKAROUND---------------------------------------vvvvv
        self.assertEqualDiff("'--source-property sync=?'\n"[:-1],
                             filterIndented(out))

        out, err, code = self.runCmdline(["sync=?"],
                                         sessionFlags=None)
        self.assertNoErrors(err)
        # WORKAROUND---------------------vvvvv
        self.assertEqualDiff("'sync=?'\n"[:-1],
                             filterIndented(out))

        out, err, code = self.runCmdline(["syncURL=?"],
                                         sessionFlags=None)
        self.assertNoErrors(err)
        # WORKAROUND------------------------vvvvv
        self.assertEqualDiff("'syncURL=?'\n"[:-1],
                             filterIndented(out))

    # test semantic of config creation (instead of updating) with and
    # without templates. See BMC # 14805
    @property("debug", False)
    def testConfigureTemplates(self):
        """TestCmdline.testConfigureTemplates - make configuration with user provided informations"""
        shutil.rmtree(self.configdir, True)

        # catch possible typos like "sheduleworld"
        out, err, code = self.runCmdline(["--configure", "foo"],
                                         expectSuccess = False)
        error = """[ERROR] No configuration template for 'foo@default' available.
[INFO] Use '--template none' and/or specify relevant properties on the command line to create a configuration without a template. Need values for: syncURL
[INFO] 
[INFO] Available configuration templates (clients and servers):
"""
        err = stripOutput(err)
        self.assertTrue(err.startswith(error))
        self.assertTrue(err.endswith("\n"))
        self.assertFalse(err.endswith("\n\n"))
        self.assertEqualDiff('', out)

        shutil.rmtree(self.configdir, True)
        # catch possible typos like "sheduleworld" when enough
        # properties are specified to continue without a template
        out, err, code = self.runCmdline(["--configure", "syncURL=http://foo.com", "--template", "foo", "bar"],
                                         expectSuccess = False)
        error = """[ERROR] No configuration template for 'foo' available.
[INFO] All relevant properties seem to be set, omit the --template parameter to proceed.
[INFO] 
[INFO] Available configuration templates (clients and servers):
"""
        err = stripOutput(err)
        self.assertTrue(err.startswith(error))
        self.assertTrue(err.endswith("\n"))
        self.assertFalse(err.endswith("\n\n"))
        self.assertEqualDiff('', out)

        fooconfig = """syncevolution/.internal.ini:rootMinVersion = {0}
syncevolution/.internal.ini:rootCurVersion = {1}
syncevolution/default/.internal.ini:contextMinVersion = {2}
syncevolution/default/.internal.ini:contextCurVersion = {3}
syncevolution/default/config.ini:deviceId = fixed-devid
syncevolution/default/peers/foo/.internal.ini:peerMinVersion = {4}
syncevolution/default/peers/foo/.internal.ini:peerCurVersion = {5}
""".format(self.getRootMinVersion(),
           self.getRootCurVersion(),
           self.getContextMinVersion(),
           self.getContextCurVersion(),
           self.getPeerMinVersion(),
           self.getPeerCurVersion())
        syncurl = "syncevolution/default/peers/foo/config.ini:syncURL = local://@bar\n"
        configsource = """syncevolution/default/peers/foo/sources/eds_event/config.ini:sync = two-way
syncevolution/default/sources/eds_event/config.ini:backend = calendar
"""
        xdg_config = xdg_root + "/config"
        shutil.rmtree(self.configdir, True)
        # allow users to proceed if they wish: should result in no
        # sources configured
        out, err, code = self.runCmdline(["--configure", "--template", "none", "foo"])
        self.assertSilent(out, err)
        res = filterFiles(self.removeRandomUUID(scanFiles(xdg_config)))
        self.assertEqualDiff(fooconfig, res)

        shutil.rmtree(self.configdir, True)
        # allow user to proceed if they wish: should result in no
        # sources configured even if general source properties are
        # specified
        out, err, code = self.runCmdline(["--configure", "--template", "none", "backend=calendar", "foo"])
        self.assertSilent(out, err)
        res = filterFiles(self.removeRandomUUID(scanFiles(xdg_config)))
        self.assertEqualDiff(fooconfig, res)

        shutil.rmtree(self.configdir, True)
        # allow user to proceed if they wish: should result in no
        # sources configured, even if specific source properties are
        # specified
        out, err, code = self.runCmdline(["--configure", "--template", "none", "eds_event/backend=calendar", "foo"])
        self.assertSilent(out, err)
        res = filterFiles(self.removeRandomUUID(scanFiles(xdg_config)))
        self.assertEqualDiff(fooconfig, res)

        shutil.rmtree(self.configdir, True)
        # allow user to proceed if they wish and possible: here
        # eds_event is not usable
        out, err, code = self.runCmdline(["--configure", "--template", "none", "foo", "eds_event"],
                                         expectSuccess = False)
        self.assertEqualDiff('', out)
        err = stripOutput(err)
        self.assertEqualDiff('[ERROR] error code from SyncEvolution fatal error (local, status 10500): eds_event: no backend available\n', err)

        shutil.rmtree(self.configdir, True)
        # allow user to proceed if they wish and possible: here
        # eds_event is not configurable
        out, err, code = self.runCmdline(["--configure", "syncURL=local://@bar", "foo", "eds_event"],
                                         expectSuccess = False)
        self.assertEqualDiff('', out)
        err = stripOutput(err)
        self.assertEqualDiff('[ERROR] error code from SyncEvolution fatal error (local, status 10500): no such source(s): eds_event\n', err)

        shutil.rmtree(self.configdir, True)
        # allow user to proceed if they wish and possible: here
        # eds_event is not configurable (wrong context)
        out, err, code = self.runCmdline(["--configure", "syncURL=local://@bar", "eds_event/backend@xyz=calendar", "foo", "eds_event"],
                                         expectSuccess = False)
        self.assertEqualDiff('', out)
        err = stripOutput(err)
        self.assertEqualDiff('[ERROR] error code from SyncEvolution fatal error (local, status 10500): no such source(s): eds_event\n', err)

        shutil.rmtree(self.configdir, True)
        # allow user to proceed if they wish: configure exactly the
        # specified sources
        out, err, code = self.runCmdline(["--configure", "--template", "none", "backend=calendar", "foo", "eds_event"])
        self.assertSilent(out, err)
        res = filterFiles(self.removeRandomUUID(scanFiles(xdg_config)))
        self.assertEqualDiff(fooconfig + configsource, res)

        shutil.rmtree(self.configdir, True)
        # allow user to proceed if they provide enough information:
        # should result in no sources configured
        out, err, code = self.runCmdline(["--configure", "syncURL=local://@bar", "foo"])
        self.assertSilent(out, err)
        res = filterFiles(self.removeRandomUUID(scanFiles(xdg_config)))
        self.assertEqualDiff(fooconfig + syncurl, res)

        shutil.rmtree(self.configdir, True)
        # allow user to proceed if they provide enough information:
        # source created because listed and usable
        out, err, code = self.runCmdline(["--configure", "syncURL=local://@bar", "backend=calendar", "foo", "eds_event"])
        self.assertSilent(out, err)
        res = filterFiles(self.removeRandomUUID(scanFiles(xdg_config)))
        self.assertEqualDiff(fooconfig + syncurl + configsource, res)

        shutil.rmtree(self.configdir, True)
        # allow user to proceed if they provide enough information:
        # source created because listed and usable
        out, err, code = self.runCmdline(["--configure", "syncURL=local://@bar", "eds_event/backend@default=calendar", "foo", "eds_event"])
        self.assertSilent(out, err)
        res = filterFiles(self.removeRandomUUID(scanFiles(xdg_config)))
        self.assertEqualDiff(fooconfig + syncurl + configsource, res)

    @property("debug", False)
    def testConfigureSource(self):
        '''TestCmdline.testConfigureSource - configure some sources'''
        # create from scratch with only addressbook configured
        out, err, code = self.runCmdline(["--configure",
                                          "--source-property", "database = file://tmp/test",
                                          "--source-property", "type = file:text/x-vcard",
                                          "@foobar",
                                          "addressbook"])
        self.assertSilent(out, err)

        root = self.configdir + "/foobar"
        res = self.removeRandomUUID(scanFiles(root))
        expected = '''.internal.ini:contextMinVersion = {0}
.internal.ini:contextCurVersion = {1}
config.ini:# logdir = 
config.ini:# maxlogdirs = 10
config.ini:deviceId = fixed-devid
sources/addressbook/config.ini:backend = file
sources/addressbook/config.ini:database = file://tmp/test
sources/addressbook/config.ini:databaseFormat = text/x-vcard
sources/addressbook/config.ini:# databaseUser = 
sources/addressbook/config.ini:# databasePassword = 
'''.format(self.getContextMinVersion(),
           self.getContextCurVersion())
        self.assertEqualDiff(expected, res)

        # add calendar
        out, err, code = self.runCmdline(["--configure",
                                          "--source-property", "database@foobar = file://tmp/test2",
                                          "--source-property", "backend = calendar",
                                          "@foobar",
                                          "calendar"])
        self.assertSilent(out, err)

        res = self.removeRandomUUID(scanFiles(root))
        expected += '''sources/calendar/config.ini:backend = calendar
sources/calendar/config.ini:database = file://tmp/test2
sources/calendar/config.ini:# databaseFormat = 
sources/calendar/config.ini:# databaseUser = 
sources/calendar/config.ini:# databasePassword = 
'''
        self.assertEqualDiff(expected, res)

        # add ScheduleWorld peer: must reuse existing backend settings
        out, err, code = self.runCmdline(["--configure",
                                          "scheduleworld@foobar"])
        self.assertSilent(out, err)

        res = self.removeRandomUUID(scanFiles(root))
        expected = self.ScheduleWorldConfig()
        expected = expected.replace("addressbook/config.ini:backend = addressbook",
                                    "addressbook/config.ini:backend = file")
        expected = expected.replace("addressbook/config.ini:# database = ",
                                    "addressbook/config.ini:database = file://tmp/test")
        expected = expected.replace("addressbook/config.ini:# databaseFormat = ",
                                    "addressbook/config.ini:databaseFormat = text/x-vcard")
        expected = expected.replace("calendar/config.ini:# database = ",
                                    "calendar/config.ini:database = file://tmp/test2")
        expected = sortConfig(expected)
        self.assertEqualDiff(expected, res)

        # disable all sources except for addressbook
        out, err, code = self.runCmdline(["--configure",
                                          "--source-property", "addressbook/sync=two-way",
                                          "--source-property", "sync=none",
                                          "scheduleworld@foobar"])
        self.assertSilent(out, err)

        res = self.removeRandomUUID(scanFiles(root))
        expected = expected.replace("sync = two-way",
                                    "sync = disabled")
        expected = expected.replace("sync = disabled",
                                    "sync = two-way",
                                    1)
        self.assertEqualDiff(expected, res)

        # override type in template while creating from scratch
        out, err, code = self.runCmdline(["--configure",
                                          "--template", "SyncEvolution",
                                          "--source-property", "addressbook/type=file:text/vcard:3.0",
                                          "--source-property", "calendar/type=file:text/calendar:2.0",
                                          "syncevo@syncevo"])
        self.assertSilent(out, err)

        syncevoroot = self.configdir + "/syncevo"
        res = scanFiles(syncevoroot + "/sources/addressbook")
        self.assertIn("backend = file\n", res)
        self.assertIn("databaseFormat = text/vcard\n", res)

        res = scanFiles(syncevoroot + "/sources/calendar")
        self.assertIn("backend = file\n", res)
        self.assertIn("databaseFormat = text/calendar\n", res)

    @property("debug", False)
    def testPrintDatabases(self):
        '''TestCmdline.testPrintDatabases - print some databases'''
        # full output
        out, err, code = self.runCmdline(["--print-databases"])
        self.assertNoErrors(err)
        # exact output varies, do not test

        haveEDS = False
        out, err, code = self.runCmdline(["--print-databases", "backend=evolution-contacts"])
        if "not one of the valid values" in err:
            # not enabled, only this error message expected
            self.assertEqualDiff('', out)
        else:
            # enabled, no error, one entry
            haveEDS = True
            self.assertNoErrors(err)
            self.assertTrue(out.startswith("evolution-contacts:\n"))
            entries = 0
            lines = out.splitlines()
            for line in lines:
                if line and not line.startswith(" "):
                    entries += 1
            self.assertEqual(1, entries)
        if haveEDS:
            # limit output to one specific backend, chosen via config
            out, err, code = self.runCmdline(["--configure", "backend=evolution-contacts", "@foo-config", "bar-source"])
            self.assertSilent(out, err)
            out, err, code = self.runCmdline(["--print-databases", "@foo-config", "bar-source"])
            self.assertNoErrors(err)
            self.assertTrue(out.startswith("@foo-config/bar-source:\n"))
            entries = 0
            lines = out.splitlines()
            for line in lines:
                if line and not line.startswith(" "):
                    entries += 1
            self.assertEqual(1, entries)

    @property("debug", False)
    def testMigrate(self):
        '''TestCmdline.testMigrate - migrate from old configuration'''
        oldroot = xdg_root + "/.sync4j/evolution/scheduleworld"
        newroot = self.configdir + "/default"
        oldconfig = self.OldScheduleWorldConfig()

        # migrate old config
        createFiles(oldroot, oldconfig)
        createdoldconfig = scanFiles(oldroot)
        out, err, code = self.runCmdline(["--migrate",
                                          "scheduleworld"])
        self.assertSilent(out, err)

        migratedconfig = scanFiles(newroot)
        expected = sortConfig(self.ScheduleWorldConfig())
        # migrating SyncEvolution < 1.2 configs sets ConsumerReady, to
        # keep config visible in the updated sync-ui
        expected = expected.replace("# ConsumerReady = 0",
                                    "ConsumerReady = 1")
        expected = expected.replace("# database = ",
                                    "database = xyz",
                                    1)
        expected = expected.replace("# databaseUser = ",
                                    "databaseUser = foo",
                                    1)
        # syncevo-dbus-server always uses keyring and doesn't
        # return plain-text password.
        expected = expected.replace("# databasePassword = ",
                                    "databasePassword = -",
                                    1)
        # migrating "type" sets forceSyncFormat (always) and
        # databaseFormat (if format was part of type, as for
        # addressbook
        expected = expected.replace("# forceSyncFormat = 0",
                                    "forceSyncFormat = 0")
        expected = expected.replace("# databaseFormat = ",
                                    "databaseFormat = text/vcard",
                                    1)
        self.assertEqualDiff(expected, migratedconfig)
        renamedconfig = scanFiles(oldroot + ".old")
        self.assertEqualDiff(createdoldconfig, renamedconfig)

        # rewrite existing config with obsolete properties => these
        # properties should get removed
        #
        # There is one limitation: shared nodes are not
        # rewritten. This is acceptable.
        createFiles(newroot + "/peers/scheduleworld",
                    '''config.ini:# obsolete comment
config.ini:obsoleteprop = foo
''',
                    True)
        createdconfig = scanFiles(newroot, "scheduleworld")
        out, err, code = self.runCmdline(["--migrate",
                                          "scheduleworld"])
        self.assertSilent(out, err)

        migratedconfig = scanFiles(newroot, "scheduleworld")
        expected = sortConfig(self.ScheduleWorldConfig())
        expected = expected.replace("# ConsumerReady = 0", "ConsumerReady = 1")
        expected = expected.replace("# database = ", "database = xyz", 1)
        expected = expected.replace("# databaseUser = ", "databaseUser = foo", 1)
        # uses keyring
        expected = expected.replace("# databasePassword = ", "databasePassword = -", 1)
        expected = expected.replace("# forceSyncFormat = 0", "forceSyncFormat = 0")
        expected = expected.replace("# databaseFormat = ", "databaseFormat = text/vcard", 1)
        self.assertEqualDiff(expected, migratedconfig)
        renamedconfig = scanFiles(newroot, "scheduleworld.old.1")
        createdconfig = createdconfig.replace("ConsumerReady = 1", "ConsumerReady = 0", 1)
        createdconfig = createdconfig.replace("/scheduleworld/", "/scheduleworld.old.1/")
        self.assertEqualDiff(createdconfig, renamedconfig)

        # migrate old config with changes and .synthesis directory, a
        # second time
        createFiles(oldroot, oldconfig)
        createFiles(oldroot,
                    '''.synthesis/dummy-file.bfi:dummy = foobar
spds/sources/addressbook/changes/config.txt:foo = bar
spds/sources/addressbook/changes/config.txt:foo2 = bar2
''',
                    True)
        createdconfig = scanFiles(oldroot)
        shutil.rmtree(newroot, True)
        out, err, code = self.runCmdline(["--migrate",
                                          "scheduleworld"])
        self.assertSilent(out, err)

        migratedconfig = scanFiles(newroot)
        expected = sortConfig(self.ScheduleWorldConfig())
        expected = expected.replace("# ConsumerReady = 0",
                                    "ConsumerReady = 1")
        expected = expected.replace("# database = ",
                                    "database = xyz",
                                    1)
        expected = expected.replace("# databaseUser = ",
                                    "databaseUser = foo",
                                    1)
        # uses keyring
        expected = expected.replace("# databasePassword = ",
                                    "databasePassword = -",
                                    1)
        expected = expected.replace("# forceSyncFormat = 0",
                                    "forceSyncFormat = 0")
        expected = expected.replace("# databaseFormat = ",
                                    "databaseFormat = text/vcard",
                                    1)
        expected = expected.replace("peers/scheduleworld/sources/addressbook/config.ini",
                                    '''peers/scheduleworld/sources/addressbook/.other.ini:foo = bar
peers/scheduleworld/sources/addressbook/.other.ini:foo2 = bar2
peers/scheduleworld/sources/addressbook/config.ini''',
                                    1)
        expected = expected.replace("peers/scheduleworld/config.ini",
                                    '''peers/scheduleworld/.synthesis/dummy-file.bfi:dummy = foobar
peers/scheduleworld/config.ini''',
                                    1)
        self.assertEqualDiff(expected, migratedconfig)
        renamedconfig = scanFiles(oldroot + ".old.1")
        createdconfig = createdconfig.replace("ConsumerReady = 1",
                                              "ConsumerReady = 0",
                                              1)
        self.assertEqualDiff(createdconfig, renamedconfig)

        otherroot = self.configdir + "/other"
        shutil.rmtree(otherroot, True)

        # migrate old config into non-default context
        createFiles(oldroot, oldconfig)
        createdconfig = scanFiles(oldroot)

        out, err, code = self.runCmdline(["--migrate",
                                          "scheduleworld@other"])
        self.assertSilent(out, err)

        migratedconfig = scanFiles(otherroot)
        expected = sortConfig(self.ScheduleWorldConfig())
        expected = expected.replace("# ConsumerReady = 0",
                                    "ConsumerReady = 1")
        expected = expected.replace("# database = ",
                                    "database = xyz",
                                    1)
        expected = expected.replace("# databaseUser = ",
                                    "databaseUser = foo",
                                    1)
        # uses keyring
        expected = expected.replace("# databasePassword = ",
                                    "databasePassword = -",
                                    1)
        expected = expected.replace("# forceSyncFormat = 0",
                                    "forceSyncFormat = 0")
        expected = expected.replace("# databaseFormat = ",
                                    "databaseFormat = text/vcard",
                                    1)
        self.assertEqualDiff(expected, migratedconfig)
        renamedconfig = scanFiles(oldroot + ".old")
        self.assertEqualDiff(createdconfig, renamedconfig)

        # migrate the migrated config again inside the "other"
        # context, with no "default" context which might interfere
        # with the tests

        # ConsumerReady was set as part of previous migration, must be
        # removed during migration to hide the migrated config from
        # average users.
        shutil.rmtree(newroot, True)
        out, err, code = self.runCmdline(["--migrate",
                                          "scheduleworld@other"])
        self.assertSilent(out, err)

        migratedconfig = scanFiles(otherroot, "scheduleworld")
        expected = sortConfig(self.ScheduleWorldConfig())
        expected = expected.replace("# ConsumerReady = 0",
                                    "ConsumerReady = 1")
        expected = expected.replace("# database = ",
                                    "database = xyz",
                                    1)
        expected = expected.replace("# databaseUser = ",
                                    "databaseUser = foo",
                                    1)
        # uses keyring
        expected = expected.replace("# databasePassword = ",
                                    "databasePassword = -",
                                    1)
        expected = expected.replace("# forceSyncFormat = 0",
                                    "forceSyncFormat = 0")
        expected = expected.replace("# databaseFormat = ",
                                    "databaseFormat = text/vcard",
                                    1)
        self.assertEqualDiff(expected, migratedconfig)
        renamedconfig = scanFiles(otherroot, "scheduleworld.old.3")
        expected = expected.replace("/scheduleworld/",
                                    "/scheduleworld.old.3/")
        expected = expected.replace("ConsumerReady = 1",
                                    "ConsumerReady = 0")
        self.assertEqualDiff(expected, renamedconfig)

        # migrate once more, this time without the explicit context in
        # the config name => must not change the context, need second
        # .old dir
        out, err, code = self.runCmdline(["--migrate",
                                          "scheduleworld"])
        self.assertSilent(out, err)
        migratedconfig = scanFiles(otherroot, "scheduleworld")
        expected = expected.replace("/scheduleworld.old.3/",
                                    "/scheduleworld/")
        expected = expected.replace("ConsumerReady = 0",
                                    "ConsumerReady = 1")
        self.assertEqualDiff(expected, migratedconfig)
        renamedconfig = scanFiles(otherroot, "scheduleworld.old.4")
        expected = expected.replace("/scheduleworld/",
                                    "/scheduleworld.old.4/")
        expected = expected.replace("ConsumerReady = 1",
                                    "ConsumerReady = 0")
        self.assertEqualDiff(expected, renamedconfig)

        # remove ConsumerReady: must remain unset when migrating
        # hidden Syncevolution >= 1.2 configs
        out, err, code = self.runCmdline(["--configure",
                                          "--sync-property", "ConsumerReady=0",
                                          "scheduleworld"])
        self.assertSilent(out, err)

        # migrate once more => keep ConsumerReady unset
        out, err, code = self.runCmdline(["--migrate",
                                          "scheduleworld"])
        self.assertSilent(out, err)

        migratedconfig = scanFiles(otherroot, "scheduleworld")
        expected = expected.replace("/scheduleworld.old.4/",
                                    "/scheduleworld/")
        self.assertEqualDiff(expected, migratedconfig)
        renamedconfig = scanFiles(otherroot, "scheduleworld.old.5")
        expected = expected.replace("/scheduleworld/",
                                    "/scheduleworld.old.5/")
        self.assertEqualDiff(expected, renamedconfig)

    @property("debug", False)
    def testMigrateContext(self):
        '''TestCmdline.testMigrateContext - migrate context containing a peer'''
        # Must also migrate peer. Covers special case of inconsistent
        # "type".

        root = self.configdir + "/default"
        oldconfig = '''config.ini:logDir = none
peers/scheduleworld/config.ini:syncURL = http://sync.scheduleworld.com/funambol/ds
peers/scheduleworld/config.ini:# username = 
peers/scheduleworld/config.ini:# password = 

peers/scheduleworld/sources/addressbook/config.ini:sync = two-way
peers/scheduleworld/sources/addressbook/config.ini:uri = card3
peers/scheduleworld/sources/addressbook/config.ini:type = addressbook:text/vcard
sources/addressbook/config.ini:type = calendar

peers/funambol/config.ini:syncURL = http://sync.funambol.com/funambol/ds
peers/funambol/config.ini:# username = 
peers/funambol/config.ini:# password = 

peers/funambol/sources/calendar/config.ini:sync = refresh-from-server
peers/funambol/sources/calendar/config.ini:uri = cal
peers/funambol/sources/calendar/config.ini:type = calendar
peers/funambol/sources/addressbook/config.ini:# sync = disabled
peers/funambol/sources/addressbook/config.ini:type = file
sources/calendar/config.ini:type = memos

peers/memotoo/config.ini:syncURL = http://sync.memotoo.com/memotoo/ds
peers/memotoo/config.ini:# username = 
peers/memotoo/config.ini:# password = 

peers/memotoo/sources/memo/config.ini:sync = refresh-from-client
peers/memotoo/sources/memo/config.ini:uri = cal
peers/memotoo/sources/memo/config.ini:type = memo:text/plain
sources/memo/config.ini:type = todo
'''

        createFiles(root, oldconfig)
        out, err, code = self.runCmdline(["--migrate",
                                          "memo/backend=file", # override memo "backend" during migration
                                          "@default"])
        self.assertSilent(out, err)

        migratedconfig = scanFiles(root)

        expectedlines = ["peers/scheduleworld/",
                         "sources/addressbook/config.ini:backend = addressbook",
                         "sources/addressbook/config.ini:databaseFormat = text/vcard",
                         "peers/scheduleworld/sources/addressbook/config.ini:syncFormat = text/vcard",
                         "peers/scheduleworld/sources/addressbook/config.ini:sync = two-way",
                         "peers/scheduleworld/sources/calendar/config.ini:# sync = disabled",
                         "peers/scheduleworld/sources/memo/config.ini:# sync = disabled",
                         "sources/calendar/config.ini:backend = calendar",
                         "sources/calendar/config.ini:# databaseFormat = ",
                         "peers/funambol/sources/calendar/config.ini:# syncFormat = ",
                         "peers/funambol/sources/addressbook/config.ini:# sync = disabled",
                         "peers/funambol/sources/calendar/config.ini:sync = refresh-from-server",
                         "peers/funambol/sources/memo/config.ini:# sync = disabled",
                         "sources/memo/config.ini:backend = file",
                         "sources/memo/config.ini:databaseFormat = text/plain",
                         "peers/memotoo/sources/memo/config.ini:syncFormat = text/plain",
                         "peers/memotoo/sources/addressbook/config.ini:# sync = disabled",
                         "peers/memotoo/sources/calendar/config.ini:# sync = disabled",
                         "peers/memotoo/sources/memo/config.ini:sync = refresh-from-client"]

        for expectedline in expectedlines:
            self.assertIn(expectedline, migratedconfig)

    @property("debug", False)
    def testMigrateAutoSync(self):
        '''TestCmdline.testMigrateAutoSync - migrate old configuration files'''
        oldroot = xdg_root + "/.sync4j/evolution/scheduleworld"
        newroot = self.configdir + "/default"
        oldconfig = "spds/syncml/config.txt:autoSync = 1\n" + self.OldScheduleWorldConfig()

        # migrate old config
        createFiles(oldroot, oldconfig)
        createdconfig = scanFiles(oldroot)
        out, err, code = self.runCmdline(["--migrate",
                                          "scheduleworld"])
        self.assertSilent(out, err)

        migratedconfig = scanFiles(newroot)
        expected = self.ScheduleWorldConfig()
        expected = expected.replace("# autoSync = 0",
                                    "autoSync = 1",
                                    1)
        expected = sortConfig(expected)

        # migrating Syncevolution < 1.2 configs sets ConsumerReady, to
        # keep config visible in the updated sync-ui
        expected = expected.replace("# ConsumerReady = 0",
                                    "ConsumerReady = 1")
        expected = expected.replace("# database = ",
                                    "database = xyz",
                                    1)
        expected = expected.replace("# databaseUser = ",
                                    "databaseUser = foo",
                                    1)
        # uses keyring
        expected = expected.replace("# databasePassword = ",
                                    "databasePassword = -",
                                    1)
        # migrating "type" sets forceSyncFormat (always) and
        # databaseFormat (if format was part of type, as for
        # addressbook)
        expected = expected.replace("# forceSyncFormat = 0",
                                    "forceSyncFormat = 0")
        expected = expected.replace("# databaseFormat = ",
                                    "databaseFormat = text/vcard", 1)
        self.assertEqualDiff(expected, migratedconfig)
        renamedconfig = scanFiles(oldroot + ".old")
        # autoSync must have been unset
        createdconfig = createdconfig.replace(":autoSync = 1",
                                              ":autoSync = 0",
                                              1)
        self.assertEqualDiff(createdconfig, renamedconfig)

        # rewrite existing config with autoSync set
        createdconfig = scanFiles(newroot, "scheduleworld")

        out, err, code = self.runCmdline(["--migrate",
                                          "scheduleworld"])
        self.assertSilent(out, err)

        migratedconfig = scanFiles(newroot, "scheduleworld")
        expected = self.ScheduleWorldConfig()
        expected = expected.replace("# autoSync = 0",
                                    "autoSync = 1",
                                    1)
        expected = sortConfig(expected)
        expected = expected.replace("# ConsumerReady = 0",
                                    "ConsumerReady = 1")
        expected = expected.replace("# database = ",
                                    "database = xyz",
                                    1)
        expected = expected.replace("# databaseUser = ",
                                    "databaseUser = foo",
                                    1)
        # uses keyring
        expected = expected.replace("# databasePassword = ",
                                    "databasePassword = -",
                                    1)
        expected = expected.replace("# forceSyncFormat = 0",
                                    "forceSyncFormat = 0")
        expected = expected.replace("# databaseFormat = ",
                                    "databaseFormat = text/vcard",
                                    1)
        self.assertEqualDiff(expected, migratedconfig)
        renamedconfig = scanFiles(newroot, "scheduleworld.old.1")
        # autoSync must have been unset
        createdconfig = createdconfig.replace(":autoSync = 1",
                                              ":autoSync = 0",
                                              1)
        # the scheduleworld config was consumer ready, the migrated
        # one isn't
        createdconfig = createdconfig.replace("ConsumerReady = 1",
                                              "ConsumerReady = 0")
        createdconfig = createdconfig.replace("/scheduleworld/",
                                              "/scheduleworld.old.1/")
        self.assertEqualDiff(createdconfig, renamedconfig)

    @property("debug", False)
    def testItemOperations(self):
        '''TestCmdline.testItemOperations - add and print items'''
        # "foo" not configured
        out, err, code = self.runCmdline(["--print-items",
                                          "foo",
                                          "bar"],
                                         expectSuccess = False)
        # Information about supported modules is optional, depends on compilation of
        # SyncEvolution.
        self.assertRegexpMatches(err, r'''\[ERROR\] error code from SyncEvolution error parsing config file \(local, status 20010\): bar: backend not supported (by any of the backend modules \((\w+\.so, )+\w+\.so\) )?or not correctly configured \(backend=select backend databaseFormat= syncFormat=\)\n\[ERROR\] configuration 'foo' does not exist\n\[ERROR\] source 'bar' does not exist\n\[ERROR\] backend property not set\n''')
        self.assertEqualDiff('', out)

        # "foo" not configured, no source named
        out, err, code  = self.runCmdline(["--print-items",
                                           "foo"],
                                          expectSuccess = False)
        self.assertRegexpMatches(err, r'''\[ERROR\] error code from SyncEvolution error parsing config file \(local, status 20010\): backend not supported (by any of the backend modules \((\w+\.so, )+\w+\.so\) )?or not correctly configured \(backend=select backend databaseFormat= syncFormat=\)\n\[ERROR\] configuration 'foo' does not exist\n\[ERROR\] no source selected\n\[ERROR\] backend property not set\n''')
        self.assertEqualDiff('', out)

        # nothing known about source
        out, err, code = self.runCmdline(["--print-items"],
                                         expectSuccess = False)
        self.assertRegexpMatches(err, r'''\[ERROR\] error code from SyncEvolution error parsing config file \(local, status 20010\): backend not supported (by any of the backend modules \((\w+\.so, )+\w+\.so\) )?or not correctly configured \(backend=select backend databaseFormat= syncFormat=\)\n\[ERROR\] no source selected\n\[ERROR\] backend property not set\n''')
        self.assertEqualDiff('', out)

        # now create "foo"
        out, err, code = self.runCmdline(["--configure",
                                          "--template", "default",
                                          "foo"])
        self.assertSilent(out, err)

        # "foo" now configured, still no source
        out, err, code  = self.runCmdline(["--print-items",
                                           "foo"],
                                          expectSuccess = False)
        self.assertRegexpMatches(err, r'''\[ERROR\] error code from SyncEvolution error parsing config file \(local, status 20010\): backend not supported (by any of the backend modules \((\w+\.so, )+\w+\.so\) )?or not correctly configured \(backend=select backend databaseFormat= syncFormat=\)\n\[ERROR\] no source selected\n\[ERROR\] backend property not set\n''')
        self.assertEqualDiff('', out)

        # "foo" configured, but "bar" is not
        out, err, code = self.runCmdline(["--print-items",
                                          "foo",
                                          "bar"],
                                         expectSuccess = False)
        self.assertRegexpMatches(err, r'''\[ERROR\] error code from SyncEvolution error parsing config file \(local, status 20010\): bar: backend not supported (by any of the backend modules \((\w+\.so, )+\w+\.so\) )?or not correctly configured \(backend=select backend databaseFormat= syncFormat=\)\n\[ERROR\] source 'bar' does not exist\n\[ERROR\] backend property not set\n''')
        self.assertEqualDiff('', out)

        # add "bar" source, using file backend
        out, err, code = self.runCmdline(["--configure",
                                          "backend=file",
                                          "database=file://" + xdg_root + "/addressbook",
                                          "databaseFormat=text/vcard",
                                          "foo",
                                          "bar"])
        self.assertSilent(out, err)

        # no items yet
        out, err, code = self.runCmdline(["--print-items",
                                          "foo",
                                          "bar"])
        self.assertSilent(out, err)

        john = """BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John;;;
END:VCARD
"""
        joan = """BEGIN:VCARD
VERSION:3.0
FN:Joan Doe
N:Doe;Joan;;;
END:VCARD
"""

        # create one file
        file1 = "1:" + john
        file2 = "2:" + joan
        file1 = file1.replace("\n", "\n1:")
        file1 = file1[:-2]
        file2 = file2.replace("\n", "\n2:")
        file2 = file2[:-2]
        createFiles(xdg_root + "/addressbook", file1 + file2)

        out, err, code = self.runCmdline(["--print-items",
                                          "foo",
                                          "bar"])
        self.assertNoErrors(err)
        self.assertEqualDiff("1\n2\n", out)

        # alternatively just specify enough parameters without the
        # "foo" "bar" config part
        out, err, code = self.runCmdline(["--print-items",
                                          "backend=file",
                                          "database=file://" + xdg_root + "/addressbook",
                                          "databaseFormat=text/vcard"])
        self.assertNoErrors(err)
        self.assertEqualDiff("1\n2\n", out)

        # export all
        out, err, code = self.runCmdline(["--export", "-",
                                          "backend=file",
                                          "database=file://" + xdg_root + "/addressbook",
                                          "databaseFormat=text/vcard"])
        self.assertNoErrors(err)
        self.assertEqualDiff(john + "\n" + joan, out)

        # export all via config
        out, err, code = self.runCmdline(["--export", "-",
                                          "foo", "bar"])
        self.assertNoErrors(err)
        self.assertEqualDiff(john + "\n" + joan, out)

        # export one
        out, err, code = self.runCmdline(["--export", "-",
                                          "backend=file",
                                          "database=file://" + xdg_root + "/addressbook",
                                          "databaseFormat=text/vcard",
                                          "--luids", "1"])
        self.assertNoErrors(err)
        self.assertEqualDiff(john, out)

        # export one via config
        out, err, code = self.runCmdline(["--export", "-",
                                          "foo", "bar", "1"])
        self.assertNoErrors(err)
        self.assertEqualDiff(john, out)

        # Copied from C++ test:
        # TODO: check configuration of just the source as @foo bar
        # without peer

        # check error message for missing config name
        out, err, code = self.runCmdline([],
                                         expectSuccess = False)
        self.expectUsageError(out, err,
                              "[ERROR] No configuration name specified.\n")

        # check error message for missing config name, version II
        out, err, code = self.runCmdline(["--run"],
                                         expectSuccess = False)
        self.expectUsageError(out, err,
                              "[ERROR] No configuration name specified.\n")

    def stripSyncTime(self, out):
        '''remove varying time from sync session output'''
        p = re.compile(r'^\| +start .*?, duration \d:\d\dmin +\|$',
                       re.MULTILINE)
        return p.sub('| start xxx, duration a:bcmin |', out)

    @property("debug", False)
    @timeout(200)
    def testSyncOutput(self):
        """TestCmdline.testSyncOutput - run syncs between local dirs and check output"""
        self.setUpLocalSyncConfigs()
        self.session.Detach()
        os.makedirs(xdg_root + "/server")
        item = xdg_root + "/server/0"
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
END:VCARD''')
        output.close()

        out, err, code = self.runCmdline(["--sync", "slow", "server"],
                                         sessionFlags=[],
                                         preserveOutputOrder=True)
        self.assertEqual(err, None)
        self.assertEqual(0, code)
        out = self.stripSyncTime(out)
        self.assertEqualDiff('''[INFO @client] @client/addressbook: starting first time sync, two-way (peer is server)
[INFO @client] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@client data changes to be applied during synchronization:
*** @client/addressbook ***
Comparison was impossible.

[INFO] @default/addressbook: starting first time sync, two-way (peer is client)
[INFO] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@default data changes to be applied during synchronization:
*** @default/addressbook ***
Comparison was impossible.

[INFO] @default/addressbook: started
[INFO] @default/addressbook: sent 1
[INFO @client] @client/addressbook: started
[INFO @client] @client/addressbook: received 1/1
[INFO @client] @client/addressbook: added 1, updated 0, removed 0
[INFO] @default/addressbook: first time sync done successfully
[INFO @client] @client/addressbook: first time sync done successfully
[INFO @client] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |        @client        |       @default        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  1  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|   slow, 0 KB sent by client, 0 KB received                          |
|   item(s) in database backup: 0 before sync, 1 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @client during synchronization:
*** @client/addressbook ***
                           before sync | after sync
                   removed during sync <
                                       > added during sync
-------------------------------------------------------------------------------
                                       > BEGIN:VCARD                           
                                       > N:Doe;John                            
                                       > FN:John Doe                           
                                       > VERSION:3.0                           
                                       > END:VCARD                             
-------------------------------------------------------------------------------

[INFO] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |       @default        |        @client        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  0  |  0  |  0  |  1  |  0  |  0  |  0  |  0  |
|   slow, 0 KB sent by client, 0 KB received                          |
|   item(s) in database backup: 1 before sync, 1 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @default during synchronization:
*** @default/addressbook ***
no changes

''', out)
        # Only 'addressbook' ever active. When done (= progress 100%),
        # a lot of information seems to be missing (= -1) or dubious
        # (1 out of 0 items sent?!). This information comes straight
        # from libsynthesis; use it as it is for now.
        self.assertRegexpMatches(self.prettyPrintEvents(),
                                 r'''status: idle, .*
(.*\n)+status: running;waiting, 0, \{addressbook: \(slow, running, 0\)\}
(.*\n)*progress: 100, \{addressbook: \(sending, -1, -1, 1, 0, -1, -1\)\}
(.*\n)*status: done, .*''')
        self.checkSync(numReports=1)

        # check result (should be unchanged)
        input = open(item, "r")
        self.assertIn("FN:John Doe", input.read())

        # no changes
        out, err, code = self.runCmdline(["server"],
                                         sessionFlags=[],
                                         preserveOutputOrder=True)
        self.assertEqual(err, None)
        self.assertEqual(0, code)
        out = self.stripSyncTime(out)
        self.assertEqualDiff('''[INFO @client] @client/addressbook: starting normal sync, two-way (peer is server)
[INFO @client] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@client data changes to be applied during synchronization:
*** @client/addressbook ***
no changes

[INFO] @default/addressbook: starting normal sync, two-way (peer is client)
[INFO] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@default data changes to be applied during synchronization:
*** @default/addressbook ***
no changes

[INFO] @default/addressbook: started
[INFO @client] @client/addressbook: started
[INFO] @default/addressbook: normal sync done successfully
[INFO @client] @client/addressbook: normal sync done successfully
[INFO @client] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |        @client        |       @default        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|   two-way, 0 KB sent by client, 0 KB received                       |
|   item(s) in database backup: 1 before sync, 1 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @client during synchronization:
*** @client/addressbook ***
no changes

[INFO] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |       @default        |        @client        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|   two-way, 0 KB sent by client, 0 KB received                       |
|   item(s) in database backup: 1 before sync, 1 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @default during synchronization:
*** @default/addressbook ***
no changes

''', out)
        self.assertRegexpMatches(self.prettyPrintEvents(),
                                 r'''status: idle, .*
(.*\n)+status: running;waiting, 0, \{addressbook: \(two-way, running, 0\)\}
(.*\n)*progress: 100, \{addressbook: \(, -1, -1, -1, -1, -1, -1\)\}
(.*\n)*status: done, .*''')
        self.checkSync(numReports=2)

        # update contact
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:Joan Doe
N:Doe;Joan
END:VCARD''')
        output.close()
        out, err, code = self.runCmdline(["server"],
                                         sessionFlags=[],
                                         preserveOutputOrder=True)
        self.assertEqual(err, None)
        self.assertEqual(0, code)
        out = self.stripSyncTime(out)
        self.assertEqualDiff('''[INFO @client] @client/addressbook: starting normal sync, two-way (peer is server)
[INFO @client] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@client data changes to be applied during synchronization:
*** @client/addressbook ***
no changes

[INFO] @default/addressbook: starting normal sync, two-way (peer is client)
[INFO] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@default data changes to be applied during synchronization:
*** @default/addressbook ***
                       after last sync | current data
               removed since last sync <
                                       > added since last sync
-------------------------------------------------------------------------------
BEGIN:VCARD                              BEGIN:VCARD                           
N:Doe;John                             | N:Doe;Joan                            
FN:John Doe                            | FN:Joan Doe                           
VERSION:3.0                              VERSION:3.0                           
END:VCARD                                END:VCARD                             
-------------------------------------------------------------------------------

[INFO] @default/addressbook: started
[INFO] @default/addressbook: sent 1
[INFO @client] @client/addressbook: started
[INFO @client] @client/addressbook: received 1/1
[INFO @client] @client/addressbook: added 0, updated 1, removed 0
[INFO] @default/addressbook: normal sync done successfully
[INFO @client] @client/addressbook: normal sync done successfully
[INFO @client] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |        @client        |       @default        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  1  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|   two-way, 0 KB sent by client, 0 KB received                       |
|   item(s) in database backup: 1 before sync, 1 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @client during synchronization:
*** @client/addressbook ***
                           before sync | after sync
                   removed during sync <
                                       > added during sync
-------------------------------------------------------------------------------
BEGIN:VCARD                              BEGIN:VCARD                           
N:Doe;John                             | N:Doe;Joan                            
FN:John Doe                            | FN:Joan Doe                           
VERSION:3.0                              VERSION:3.0                           
END:VCARD                                END:VCARD                             
-------------------------------------------------------------------------------

[INFO] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |       @default        |        @client        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  0  |  0  |  0  |  0  |  1  |  0  |  0  |  0  |
|   two-way, 0 KB sent by client, 0 KB received                       |
|   item(s) in database backup: 1 before sync, 1 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @default during synchronization:
*** @default/addressbook ***
no changes

''', out)
        self.assertRegexpMatches(self.prettyPrintEvents(),
                                 r'''status: idle, .*
(.*\n)+status: running;waiting, 0, \{addressbook: \(two-way, running, 0\)\}
(.*\n)*progress: 100, \{addressbook: \(sending, -1, -1, 1, 0, -1, -1\)\}
(.*\n)*status: done, .*''')
        self.checkSync(numReports=3)

        # now remove contact
        os.unlink(item)
        out, err, code = self.runCmdline(["server"],
                                         sessionFlags=[],
                                         preserveOutputOrder=True)
        self.assertEqual(err, None)
        self.assertEqual(0, code)
        out = self.stripSyncTime(out)
        self.assertEqualDiff('''[INFO @client] @client/addressbook: starting normal sync, two-way (peer is server)
[INFO @client] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@client data changes to be applied during synchronization:
*** @client/addressbook ***
no changes

[INFO] @default/addressbook: starting normal sync, two-way (peer is client)
[INFO] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@default data changes to be applied during synchronization:
*** @default/addressbook ***
                       after last sync | current data
               removed since last sync <
                                       > added since last sync
-------------------------------------------------------------------------------
BEGIN:VCARD                            <
N:Doe;Joan                             <
FN:Joan Doe                            <
VERSION:3.0                            <
END:VCARD                              <
-------------------------------------------------------------------------------

[INFO] @default/addressbook: started
[INFO] @default/addressbook: sent 1
[INFO @client] @client/addressbook: started
[INFO @client] @client/addressbook: received 1/1
[INFO @client] @client/addressbook: added 0, updated 0, removed 1
[INFO] @default/addressbook: normal sync done successfully
[INFO @client] @client/addressbook: normal sync done successfully
[INFO @client] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |        @client        |       @default        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  0  |  1  |  0  |  0  |  0  |  0  |  0  |  0  |
|   two-way, 0 KB sent by client, 0 KB received                       |
|   item(s) in database backup: 1 before sync, 0 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @client during synchronization:
*** @client/addressbook ***
                           before sync | after sync
                   removed during sync <
                                       > added during sync
-------------------------------------------------------------------------------
BEGIN:VCARD                            <
N:Doe;Joan                             <
FN:Joan Doe                            <
VERSION:3.0                            <
END:VCARD                              <
-------------------------------------------------------------------------------

[INFO] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |       @default        |        @client        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  0  |  0  |  0  |  0  |  0  |  1  |  0  |  0  |
|   two-way, 0 KB sent by client, 0 KB received                       |
|   item(s) in database backup: 0 before sync, 0 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @default during synchronization:
*** @default/addressbook ***
no changes

''', out)
        self.assertRegexpMatches(self.prettyPrintEvents(),
                                 r'''status: idle, .*
(.*\n)+status: running;waiting, 0, \{addressbook: \(two-way, running, 0\)\}
(.*\n)*progress: 100, \{addressbook: \(sending, -1, -1, 1, 0, -1, -1\)\}
(.*\n)*status: done, .*''')
        self.checkSync(numReports=4)

    @property("debug", False)
    @timeout(200)
    def testSyncOutput2(self):
        """TestCmdline.testSyncOutput2 - run syncs between local dirs and check output, with two sources"""
        self.setUpLocalSyncConfigs(enableCalendar=True)
        self.session.Detach()
        os.makedirs(xdg_root + "/server")
        item = xdg_root + "/server/0"
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
END:VCARD''')
        output.close()

        out, err, code = self.runCmdline(["--sync", "slow", "server"],
                                         sessionFlags=[],
                                         preserveOutputOrder=True)
        self.assertEqual(err, None)
        self.assertEqual(0, code)
        out = self.stripSyncTime(out)
        self.assertEqualDiff('''[INFO @client] @client/addressbook: starting first time sync, two-way (peer is server)
[INFO @client] @client/calendar: starting first time sync, two-way (peer is server)
[INFO @client] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@client data changes to be applied during synchronization:
*** @client/addressbook ***
Comparison was impossible.

[INFO @client] creating complete data backup of source calendar before sync (enabled with dumpData and needed for printChanges)
*** @client/calendar ***
Comparison was impossible.

[INFO] @default/addressbook: starting first time sync, two-way (peer is client)
[INFO] @default/calendar: starting first time sync, two-way (peer is client)
[INFO] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@default data changes to be applied during synchronization:
*** @default/addressbook ***
Comparison was impossible.

[INFO] @default/addressbook: started
[INFO] creating complete data backup of source calendar before sync (enabled with dumpData and needed for printChanges)
*** @default/calendar ***
Comparison was impossible.

[INFO] @default/calendar: started
[INFO] @default/addressbook: sent 1
[INFO @client] @client/addressbook: started
[INFO @client] @client/addressbook: received 1/1
[INFO @client] @client/addressbook: added 1, updated 0, removed 0
[INFO @client] @client/calendar: started
[INFO] @default/addressbook: first time sync done successfully
[INFO] @default/calendar: first time sync done successfully
[INFO @client] @client/addressbook: first time sync done successfully
[INFO @client] @client/calendar: first time sync done successfully
[INFO @client] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |        @client        |       @default        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  1  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|   slow, 0 KB sent by client, 0 KB received                          |
|   item(s) in database backup: 0 before sync, 1 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|      calendar |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|      slow, 0 KB sent by client, 0 KB received                       |
|      item(s) in database backup: 0 before sync, 0 after it          |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @client during synchronization:
*** @client/addressbook ***
                           before sync | after sync
                   removed during sync <
                                       > added during sync
-------------------------------------------------------------------------------
                                       > BEGIN:VCARD                           
                                       > N:Doe;John                            
                                       > FN:John Doe                           
                                       > VERSION:3.0                           
                                       > END:VCARD                             
-------------------------------------------------------------------------------
*** @client/calendar ***
no changes

[INFO] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |       @default        |        @client        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  0  |  0  |  0  |  1  |  0  |  0  |  0  |  0  |
|   slow, 0 KB sent by client, 0 KB received                          |
|   item(s) in database backup: 1 before sync, 1 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|      calendar |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|      slow, 0 KB sent by client, 0 KB received                       |
|      item(s) in database backup: 0 before sync, 0 after it          |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @default during synchronization:
*** @default/addressbook ***
no changes
*** @default/calendar ***
no changes

''', out)
        self.assertRegexpMatches(self.prettyPrintEvents(),
                                 r'''status: idle, .*
(.*\n)+status: running;waiting, 0, \{addressbook: \(slow, running, 0\), calendar: \(slow, running, 0\)\}
(.*\n)*progress: 100, \{addressbook: \(sending, -1, -1, 1, 0, -1, -1\), calendar: \(, -1, -1, -1, -1, -1, -1\)\}
(.*\n)*status: done, .*''')
        self.checkSync(numReports=1)

        # check result (should be unchanged)
        input = open(item, "r")
        self.assertIn("FN:John Doe", input.read())

        # no changes
        out, err, code = self.runCmdline(["server"],
                                         sessionFlags=[],
                                         preserveOutputOrder=True)
        self.assertEqual(err, None)
        self.assertEqual(0, code)
        out = self.stripSyncTime(out)
        self.assertEqualDiff('''[INFO @client] @client/addressbook: starting normal sync, two-way (peer is server)
[INFO @client] @client/calendar: starting normal sync, two-way (peer is server)
[INFO @client] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@client data changes to be applied during synchronization:
*** @client/addressbook ***
no changes

[INFO @client] creating complete data backup of source calendar before sync (enabled with dumpData and needed for printChanges)
*** @client/calendar ***
no changes

[INFO] @default/addressbook: starting normal sync, two-way (peer is client)
[INFO] @default/calendar: starting normal sync, two-way (peer is client)
[INFO] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@default data changes to be applied during synchronization:
*** @default/addressbook ***
no changes

[INFO] @default/addressbook: started
[INFO] creating complete data backup of source calendar before sync (enabled with dumpData and needed for printChanges)
*** @default/calendar ***
no changes

[INFO] @default/calendar: started
[INFO @client] @client/addressbook: started
[INFO @client] @client/calendar: started
[INFO] @default/addressbook: normal sync done successfully
[INFO] @default/calendar: normal sync done successfully
[INFO @client] @client/addressbook: normal sync done successfully
[INFO @client] @client/calendar: normal sync done successfully
[INFO @client] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |        @client        |       @default        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|   two-way, 0 KB sent by client, 0 KB received                       |
|   item(s) in database backup: 1 before sync, 1 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|      calendar |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|      two-way, 0 KB sent by client, 0 KB received                    |
|      item(s) in database backup: 0 before sync, 0 after it          |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @client during synchronization:
*** @client/addressbook ***
no changes
*** @client/calendar ***
no changes

[INFO] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |       @default        |        @client        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|   two-way, 0 KB sent by client, 0 KB received                       |
|   item(s) in database backup: 1 before sync, 1 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|      calendar |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|      two-way, 0 KB sent by client, 0 KB received                    |
|      item(s) in database backup: 0 before sync, 0 after it          |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @default during synchronization:
*** @default/addressbook ***
no changes
*** @default/calendar ***
no changes

''', out)
        self.assertRegexpMatches(self.prettyPrintEvents(),
                                 r'''status: idle, .*
(.*\n)+status: running;waiting, 0, \{addressbook: \(two-way, running, 0\), calendar: \(two-way, running, 0\)\}
(.*\n)*progress: 100, \{addressbook: \(, -1, -1, -1, -1, -1, -1\), calendar: \(, -1, -1, -1, -1, -1, -1\)\}
(.*\n)*status: done, .*''')
        self.checkSync(numReports=2)

        # update contact
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:Joan Doe
N:Doe;Joan
END:VCARD''')
        output.close()
        out, err, code = self.runCmdline(["server"],
                                         sessionFlags=[],
                                         preserveOutputOrder=True)
        self.assertEqual(err, None)
        self.assertEqual(0, code)
        out = self.stripSyncTime(out)
        self.assertEqualDiff('''[INFO @client] @client/addressbook: starting normal sync, two-way (peer is server)
[INFO @client] @client/calendar: starting normal sync, two-way (peer is server)
[INFO @client] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@client data changes to be applied during synchronization:
*** @client/addressbook ***
no changes

[INFO @client] creating complete data backup of source calendar before sync (enabled with dumpData and needed for printChanges)
*** @client/calendar ***
no changes

[INFO] @default/addressbook: starting normal sync, two-way (peer is client)
[INFO] @default/calendar: starting normal sync, two-way (peer is client)
[INFO] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@default data changes to be applied during synchronization:
*** @default/addressbook ***
                       after last sync | current data
               removed since last sync <
                                       > added since last sync
-------------------------------------------------------------------------------
BEGIN:VCARD                              BEGIN:VCARD                           
N:Doe;John                             | N:Doe;Joan                            
FN:John Doe                            | FN:Joan Doe                           
VERSION:3.0                              VERSION:3.0                           
END:VCARD                                END:VCARD                             
-------------------------------------------------------------------------------

[INFO] @default/addressbook: started
[INFO] creating complete data backup of source calendar before sync (enabled with dumpData and needed for printChanges)
*** @default/calendar ***
no changes

[INFO] @default/calendar: started
[INFO] @default/addressbook: sent 1
[INFO @client] @client/addressbook: started
[INFO @client] @client/addressbook: received 1/1
[INFO @client] @client/addressbook: added 0, updated 1, removed 0
[INFO @client] @client/calendar: started
[INFO] @default/addressbook: normal sync done successfully
[INFO] @default/calendar: normal sync done successfully
[INFO @client] @client/addressbook: normal sync done successfully
[INFO @client] @client/calendar: normal sync done successfully
[INFO @client] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |        @client        |       @default        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  1  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|   two-way, 0 KB sent by client, 0 KB received                       |
|   item(s) in database backup: 1 before sync, 1 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|      calendar |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|      two-way, 0 KB sent by client, 0 KB received                    |
|      item(s) in database backup: 0 before sync, 0 after it          |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @client during synchronization:
*** @client/addressbook ***
                           before sync | after sync
                   removed during sync <
                                       > added during sync
-------------------------------------------------------------------------------
BEGIN:VCARD                              BEGIN:VCARD                           
N:Doe;John                             | N:Doe;Joan                            
FN:John Doe                            | FN:Joan Doe                           
VERSION:3.0                              VERSION:3.0                           
END:VCARD                                END:VCARD                             
-------------------------------------------------------------------------------
*** @client/calendar ***
no changes

[INFO] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |       @default        |        @client        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  0  |  0  |  0  |  0  |  1  |  0  |  0  |  0  |
|   two-way, 0 KB sent by client, 0 KB received                       |
|   item(s) in database backup: 1 before sync, 1 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|      calendar |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|      two-way, 0 KB sent by client, 0 KB received                    |
|      item(s) in database backup: 0 before sync, 0 after it          |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @default during synchronization:
*** @default/addressbook ***
no changes
*** @default/calendar ***
no changes

''', out)
        self.assertRegexpMatches(self.prettyPrintEvents(),
                                 r'''status: idle, .*
(.*\n)+status: running;waiting, 0, \{addressbook: \(two-way, running, 0\), calendar: \(two-way, running, 0\)\}
(.*\n)*progress: 100, \{addressbook: \(sending, -1, -1, 1, 0, -1, -1\), calendar: \(, -1, -1, -1, -1, -1, -1\)\}
(.*\n)*status: done, .*''')
        self.checkSync(numReports=3)

        # now remove contact
        os.unlink(item)
        out, err, code = self.runCmdline(["server"],
                                         sessionFlags=[],
                                         preserveOutputOrder=True)
        self.assertEqual(err, None)
        self.assertEqual(0, code)
        out = self.stripSyncTime(out)
        self.assertEqualDiff('''[INFO @client] @client/addressbook: starting normal sync, two-way (peer is server)
[INFO @client] @client/calendar: starting normal sync, two-way (peer is server)
[INFO @client] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@client data changes to be applied during synchronization:
*** @client/addressbook ***
no changes

[INFO @client] creating complete data backup of source calendar before sync (enabled with dumpData and needed for printChanges)
*** @client/calendar ***
no changes

[INFO] @default/addressbook: starting normal sync, two-way (peer is client)
[INFO] @default/calendar: starting normal sync, two-way (peer is client)
[INFO] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@default data changes to be applied during synchronization:
*** @default/addressbook ***
                       after last sync | current data
               removed since last sync <
                                       > added since last sync
-------------------------------------------------------------------------------
BEGIN:VCARD                            <
N:Doe;Joan                             <
FN:Joan Doe                            <
VERSION:3.0                            <
END:VCARD                              <
-------------------------------------------------------------------------------

[INFO] @default/addressbook: started
[INFO] creating complete data backup of source calendar before sync (enabled with dumpData and needed for printChanges)
*** @default/calendar ***
no changes

[INFO] @default/calendar: started
[INFO] @default/addressbook: sent 1
[INFO @client] @client/addressbook: started
[INFO @client] @client/addressbook: received 1/1
[INFO @client] @client/addressbook: added 0, updated 0, removed 1
[INFO @client] @client/calendar: started
[INFO] @default/addressbook: normal sync done successfully
[INFO] @default/calendar: normal sync done successfully
[INFO @client] @client/addressbook: normal sync done successfully
[INFO @client] @client/calendar: normal sync done successfully
[INFO @client] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |        @client        |       @default        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  0  |  1  |  0  |  0  |  0  |  0  |  0  |  0  |
|   two-way, 0 KB sent by client, 0 KB received                       |
|   item(s) in database backup: 1 before sync, 0 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|      calendar |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|      two-way, 0 KB sent by client, 0 KB received                    |
|      item(s) in database backup: 0 before sync, 0 after it          |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @client during synchronization:
*** @client/addressbook ***
                           before sync | after sync
                   removed during sync <
                                       > added during sync
-------------------------------------------------------------------------------
BEGIN:VCARD                            <
N:Doe;Joan                             <
FN:Joan Doe                            <
VERSION:3.0                            <
END:VCARD                              <
-------------------------------------------------------------------------------
*** @client/calendar ***
no changes

[INFO] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |       @default        |        @client        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  0  |  0  |  0  |  0  |  0  |  1  |  0  |  0  |
|   two-way, 0 KB sent by client, 0 KB received                       |
|   item(s) in database backup: 0 before sync, 0 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|      calendar |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|      two-way, 0 KB sent by client, 0 KB received                    |
|      item(s) in database backup: 0 before sync, 0 after it          |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @default during synchronization:
*** @default/addressbook ***
no changes
*** @default/calendar ***
no changes

''', out)
        self.assertRegexpMatches(self.prettyPrintEvents(),
                                 r'''status: idle, .*
(.*\n)+status: running;waiting, 0, \{addressbook: \(two-way, running, 0\), calendar: \(two-way, running, 0\)\}
(.*\n)*progress: 100, \{addressbook: \(sending, -1, -1, 1, 0, -1, -1\), calendar: \(, -1, -1, -1, -1, -1, -1\)\}
(.*\n)*status: done, .*''')
        self.checkSync(numReports=4)

        # only 'addressbook' active
        out, err, code = self.runCmdline(["server", "addressbook"],
                                         sessionFlags=[],
                                         preserveOutputOrder=True)
        self.assertEqual(err, None)
        self.assertEqual(0, code)
        out = self.stripSyncTime(out)
        self.assertEqualDiff('''[INFO] @default/calendar: inactive
[INFO @client] @client/calendar: inactive
[INFO @client] @client/addressbook: starting normal sync, two-way (peer is server)
[INFO @client] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@client data changes to be applied during synchronization:
*** @client/addressbook ***
no changes

[INFO] @default/addressbook: starting normal sync, two-way (peer is client)
[INFO] creating complete data backup of source addressbook before sync (enabled with dumpData and needed for printChanges)
@default data changes to be applied during synchronization:
*** @default/addressbook ***
no changes

[INFO] @default/addressbook: started
[INFO @client] @client/addressbook: started
[INFO] @default/addressbook: normal sync done successfully
[INFO @client] @client/addressbook: normal sync done successfully
[INFO @client] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |        @client        |       @default        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|   two-way, 0 KB sent by client, 0 KB received                       |
|   item(s) in database backup: 0 before sync, 0 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @client during synchronization:
*** @client/addressbook ***
no changes

[INFO] creating complete data backup after sync (enabled with dumpData and needed for printChanges)

Synchronization successful.

Changes applied during synchronization:
+---------------|-----------------------|-----------------------|-CON-+
|               |       @default        |        @client        | FLI |
|        Source | NEW | MOD | DEL | ERR | NEW | MOD | DEL | ERR | CTS |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|   addressbook |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0  |
|   two-way, 0 KB sent by client, 0 KB received                       |
|   item(s) in database backup: 0 before sync, 0 after it             |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+
| start xxx, duration a:bcmin |
|               synchronization completed successfully                |
+---------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+

Data modified @default during synchronization:
*** @default/addressbook ***
no changes

''', out)
        self.assertRegexpMatches(self.prettyPrintEvents(),
                                 r'''status: idle, .*
(.*\n)+status: running;waiting, 0, \{addressbook: \(two-way, running, 0\), calendar: \(none, idle, 0\)\}
(.*\n)*progress: 100, \{addressbook: \(, -1, -1, -1, -1, -1, -1\), calendar: \(, -1, -1, -1, -1, -1, -1\)\}
(.*\n)*status: done, .*''')
        self.checkSync(numReports=5)

if __name__ == '__main__':
    unittest.main()
