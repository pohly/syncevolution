#! /bin/sh
#
# Download current version of all our docbook XSL files.
# Handles download errors by retrying. Does not handle
# new or removed files, that needs to be done manually.

set -x
cd `dirname $0`/xsl
for i in `find * -type f`; do
    for attempt in `seq 0 10`; do
        if wget -O $i.tmp http://docbook.sourceforge.net/release/xsl/current/$i && mv $i.tmp $i; then
            break
        fi
    done
done
