#!/usr/bin/python -u

"""
Allocates resources from Murphy and/or a make jobserver while running
some command.
"""

import time
import os
import re
import subprocess
import signal
from optparse import OptionParser

usage = "usage: %prog [options] [--] command arg1 arg2 ..."
parser = OptionParser(usage=usage)
parser.add_option("-r", "--murphy-resource",
                  dest="resources",
                  action="append",
                  help="Name of a Muprhy resource which gets locked while running the command.")
parser.add_option("-j", "--jobs",
                  default=1,
                  type='int',
                  action="store",
                  help="Number of jobs to allocate from job server. Ignored if not running under a job server.")

(options, args) = parser.parse_args()

def log(format, *args):
    now = time.time()
    print time.asctime(time.gmtime(now)), 'UTC', '(+ %.1fs / %.1fs)' % (now - log.latest, now - log.start), format % args
    log.latest = now
log.start = time.time()
log.latest = log.start

# Murphy support: as a first step, lock one resource named like the
# test before running the test.
gobject = None
if options.resources:
    try:
        import gobject
    except ImportError:
        from gi.repository import GObject as gobject
    import dbus
    from dbus.mainloop.glib import DBusGMainLoop
    DBusGMainLoop(set_as_default=True)
    if not os.environ.get('DBUS_SESSION_BUS_ADDRESS', None):
        # Try to set up Murphy with a murphy-launch.py helper script
        # which is expected to be provided by the test environment
        # (not included in SyncEvolution).
        vars = subprocess.check_output(['murphy-launch.py'])
        for line in vars.split('\n'):
            if line:
                var, value = line.split('=', 1)
                os.environ[var] = value
    bus = dbus.SessionBus()
    loop = gobject.MainLoop()
    murphy = dbus.Interface(bus.get_object('org.Murphy', '/org/murphy/resource'), 'org.murphy.manager')

    # Support mapping of resource "foo" to "bar" with RESOURCES_FOO=bar.
    resources = []
    for name in options.resources:
        replacement = os.environ.get('RESOURCES_%s' % name.upper(), None)
        if replacement is not None:
            resources.extend(replacement.split(','))
        else:
            resources.append(name)

    if resources != options.resources:
        log('replaced resource set %s with %s based on RESOURCES_* env vars', options.resources, resources)

    if resources:
        log('=== locking resource(s) %s ===', resources)
        resourcesetpath = murphy.createResourceSet()
        resourceset = dbus.Interface(bus.get_object('org.Murphy', resourcesetpath), 'org.murphy.resourceset')
        for name in resources:
            resourcepath = resourceset.addResource(name)
            # Allow sharing of the resource. Only works if the resource
            # was marked as "shareable" in the murphy config, otherwise
            # we get exclusive access.
            resource = dbus.Interface(bus.get_object('org.Murphy', resourcepath), 'org.murphy.resource')
            resource.setProperty('shared', dbus.Boolean(True, variant_level=1))

        # Track pending request separately, because status == 'pending'
        # either means something else ('unknown'?) or is buggy/unreliable.
        # See https://github.com/01org/murphy/issues/5
        pending = False
        def propertyChanged(prop, value):
            global pending
            log('property changed: %s = %s', prop, value)
            if prop == 'status':
                if value == 'acquired':
                    # Success!
                    loop.quit()
                elif value == 'lost':
                    # Not yet?!
                    log('Murphy request failed, waiting for resource to become available.')
                    pending = False
                elif value == 'pending':
                    pass
                elif value == 'available':
                    if not pending:
                        log('Murphy request may succeed now, try again.')
                        resourceset.request()
                        pending = True
                    else:
                        log('Unexpected status: %s', value)
        try:
            match = bus.add_signal_receiver(propertyChanged, 'propertyChanged', 'org.murphy.resourceset', 'org.Murphy', resourcesetpath)
            resourceset.request()
            pending = True
            loop.run()
        finally:
            match.remove()

class Jobserver:
    '''Allocates the given number of job slots from the "make -j"
    jobserver, then runs the command and finally returns the slots.
    See http://mad-scientist.net/make/jobserver.html'''
    def __init__(self):
        self.havejobserver = False
        self.allocated = 0

        # MAKEFLAGS= --jobserver-fds=3,4 -j
        flags = os.environ.get('MAKEFLAGS', '')
        m = re.search(r'--jobserver-fds=(\d+),(\d+)', flags)
        if m:
            self.receiveslots = int(m.group(1))
            self.returnslots = int(m.group(2))
            self.blocked = {}
            self.havejobserver = True
            log('using jobserver')
        else:
            log('not using jobserver')

    def active(self):
        return self.havejobserver

    def alloc(self, numjobs = 1):
        if not self.havejobserver:
            return
        n = 0
        self._block()
        try:
            while n < numjobs:
                os.read(self.receiveslots, 1)
                n += 1
            self.allocated += n
            n = 0
        except:
            os.write(self.returnslots, ' ' * n)
            raise
        finally:
            self._unblock()

    def free(self, numjobs = 1):
        if not self.havejobserver:
            return
        try:
            self.allocated -= numjobs
            os.write(self.returnslots, ' ' * numjobs)
        finally:
            self._unblock()

    def _block(self):
        '''Block signals if not already done.'''
        if not self.blocked:
            for sig in [ signal.SIGINT, signal.SIGTERM ]:
                self.blocked[sig] = signal.signal(sig, signal.SIG_IGN)

    def _unblock(self):
        '''Unblock signals if blocked and we currently own no slots.'''
        if self.blocked and not self.allocated:
            for sig, handler in self.blocked.items():
                signal.signal(sig, handler)
            self.blocked = {}

jobserver = Jobserver()

jobs = 0
if jobserver.active() and options.jobs:
    log('=== allocating %d job slot(s) ===', options.jobs)
    jobserver.alloc(options.jobs)
    log('=== allocated %d job slot(s) ===', options.jobs)
    jobs = options.jobs

try:
    subprocess.check_call(args)
finally:
    log('=== cleaning up ===')
    # Return job tokens.
    if jobs:
        jobserver.free(jobs)
    # We don't need to unlock the Murphy resource. Quitting will do
    # that automatically.
