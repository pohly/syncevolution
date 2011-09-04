#!/bin/sh

set -e

# wipe out temporary autotools files, necessary
# when switching between distros
rm -rf aclocal.m4 m4 autom4te.cache config.guess config.sub config.h.in configure depcomp install-sh ltmain.sh missing 

# intltoolize fails to copy its macros unless m4 exits
mkdir m4

sh ./gen-autotools.sh

libtoolize -c
glib-gettextize --force --copy
intltoolize --force --copy --automake
if [ -n "$SBOX_PRELOAD" ]; then
  # I broke fakeroot in my Fremantle SDK when I installed the Harmattan SDK,
  # since the upgraded fakeroot in Scratchbox is apparently not all that
  # backwards compatible. Since I don't want to uninstall the Harmattan SDK
  # and reinstall the Fremantle SDK, I seem to need this hack.
  aclocal-1.9 -I m4 -I m4-repo -I /targets/links/arch_tools/share/aclocal
else
  aclocal-1.9 -I m4 -I m4-repo
fi
autoheader
automake-1.9 -a -c -Wno-portability
autoconf

# This hack is required for the autotools on Debian Etch.
# Without it, configure expects a po/Makefile where
# only po/Makefile.in is available. This patch fixes
# configure so that it uses po/Makefile.in, like more
# recent macros do.
perl -pi -e 's;test ! -f "po/Makefile";test ! -f "po/Makefile.in";; s;mv "po/Makefile" "po/Makefile.tmp";cp "po/Makefile.in" "po/Makefile.tmp";;' configure
