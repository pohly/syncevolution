#! /usr/bin/python3
#
# Copyright (C) 2014 Intel Corporation
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

# Augments stdin by adding a "log starting" preamble and "log ending"
# footer and a time stamp to each line. Flushes after each line
# and ignores SIGTERM and SIGINT, in order to not loose unprocessed
# output.

import os
import re
import signal
import sys
import time

def now():
    return time.strftime('%H:%M:%S', time.gmtime())

signal.signal(signal.SIGTERM, lambda x,y: sys.stdout.write('--- SIGTERM at %s ---\n' % now()))
signal.signal(signal.SIGINT, lambda x,y: sys.stdout.write('--- SIGINT at %s ---\n' % now()))

sys.stdout.write('--- log starting at %s ---\n' % now())
sys.stdout.flush()

while True:
    line = sys.stdin.readline()
    if not line:
        break
    if not line[0].isspace():
        sys.stdout.write(now() + ' ')
    sys.stdout.write(line)
    sys.stdout.flush()

sys.stdout.write('--- log ending at %s ---\n' % now())
sys.stdout.flush()
