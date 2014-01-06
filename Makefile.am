include $(top_srcdir)/setup-variables.am
include $(top_srcdir)/autotroll.am
include $(top_srcdir)/build/build.am

AUTOMAKE_OPTIONS = subdir-objects

## Process this file with automake to produce Makefile.in

ACLOCAL_AMFLAGS = -I m4 -I m4-repo ${ACLOCAL_FLAGS}

if COND_CORE
SUBDIRS += $(SYNTHESIS_SUBDIR)
else
SUBDIRS += $(SYNTHESIS_SUBDIR_INCLUDES)
endif

SUBDIRS += .

# choose D-Bus implementation
if COND_GIO_GDBUS
gdbus_dir = $(top_srcdir)/src/gdbusxx
gdbus_build_dir = src/gdbusxx
else
gdbus_dir = $(top_srcdir)/src/gdbus
gdbus_build_dir = src/gdbus
endif

disted_docs =
distbin_docs =

man_MANS =
include src/src.am
if COND_CORE
include test/test.am
SUBDIRS += po
disted_docs += README NEWS COPYING $(TEST_README_FILES) test/syncevo-http-server-logging.conf
distbin_docs += $(disted_docs)
dist_doc_DATA += $(disted_docs)
if COND_HTML_README
distbin_docs += README.html
# do not distribute in tarball.
doc_DATA += README.html
endif
if COND_MAN_PAGES
man_MANS += syncevolution.1
endif
endif
DIST_SUBDIRS += po

dist_noinst_DATA += \
  HACKING \
  LICENSE.txt \
  LICENSE.LGPL-21 \
  README.rst \
  description \
  autogen.sh \
  Doxyfile \
  po/LINGUAS.README

MAINTAINERCLEANFILES += Makefile.in config.h.in config.guess config.sub configure depcomp install-sh ltmain.sh missing mkinstalldirs

TEST_README_FILES = $(wildcard $(top_srcdir)/test/README.*)

# files and file patterns which are not meant to be packaged.
DEV_FILE_PATTERN = $(1)$(2)/include $(1)$(2)/lib/*.so $(1)$(2)/lib/*.a $(1)$(2)/lib/*.la $(1)$(2)/lib/*/*.la $(1)$(2)/lib/pkgconfig

# Exclude syncactivesync.so from bundle package because that needs to
# be packaged separately.
DEV_FILE_PATTERN += $(1)$(2)/lib/syncevolution/backends/syncactivesync.so

# It is possible to have additional .so backend files included in the
# binary archives. The additional backend files must be named in the
# EXTRA_BACKENDS env or make variable with their full file paths,
# separated by spaces. They will be renamed to <basename>-2.so. See
# also ScannedBackends in SyncSource.cpp.
ADD_EXTRA_BACKENDS = set -e; for i in $(EXTRA_BACKENDS); do cp $$i $(1)/$(BACKENDS_DIRECTORY)/`basename $$i .so`-2.so; done

# binary distribution as .tar.gz
if COND_DBUS
# when building with D-Bus, we have no choice: the service has to go into /usr
# in order to be found
distbin : $(distbin_docs) INSTALL-tar-gz all
	@ [ "$(BINSUFFIX)" ] || (echo "please invoke with e.g. 'make distbin BINSUFFIX=debian-3.1'"; exit 1 )
	@ [ "$(prefix)" = "/usr" ] || (echo "please reconfigure with --prefix=/usr"; exit 1 )
	rm -rf $(distdir)
	$(MAKE) DESTDIR=`pwd`/$(distdir) install
	$(MAKE) DESTDIR=`pwd`/$(distdir) installcheck
	rm -rf $(call DEV_FILE_PATTERN,$(distdir),/usr)
	for i in `find $(distdir) -type d | sort -r`; do rmdir $$i 2>/dev/null || true; done
	mkdir -p $(distdir)/usr/share/doc/syncevolution
	cp $(srcdir)/INSTALL-tar-gz $(distdir)/INSTALL
	cp $(filter-out all, $+) $(distdir)/usr/share/doc/syncevolution
	$(call ADD_EXTRA_BACKENDS, $(distdir))
	tar zcf $(distdir)-$(BINSUFFIX).tar.gz $(distdir)
	rm -rf $(distdir)
else
# without D-Bus, we can simply create an archive with a bin directory
# and everything works
distbin : $(distbin_docs) all
	@ [ "$(BINSUFFIX)" ] || (echo "please invoke with e.g. 'make distbin BINSUFFIX=debian-3.1'"; exit 1 ) 
	rm -rf $(distdir)
	$(MAKE) prefix=`pwd`/$(distdir) install
	@echo removing developer files and empty directories
	rm -rf $(call DEV_FILE_PATTERN,$(distdir),/)
	for i in `find $(distdir) -type d | sort -r`; do rmdir $$i 2>/dev/null || true; done
	cp $(filter-out all, $+) $(distdir)
	$(call ADD_EXTRA_BACKENDS, $(distdir))
	tar zcf $(distdir)-$(BINSUFFIX).tar.gz $(distdir)
	rm -rf $(distdir)
endif

iphone : SyncEvolution.plist

IPHONE_FILENAME = syncevolution-$(VERSION)-iphone.zip

SyncEvolution.plist : SyncEvolution.plist.in $(IPHONE_FILENAME)
	$(AM_V_GEN)sed -e 's/__FILENAME__/$(IPHONE_FILENAME)/' \
	               -e 's/__VERSION__/$(VERSION)/' \
	               -e 's/__SIZE__/$(shell ls -l $(IPHONE_FILENAME) | sed -e 's/  */ /g' | cut -d ' ' -f 5)/' \
	               $< >$@

$(IPHONE_FILENAME) : all
	rm -rf syncevolution-iphone $@
	$(MAKE) DESTDIR=`pwd`/syncevolution-iphone install
	rm -rf `ls -1 -d syncevolution-iphone/usr/share/doc/syncevolution/*/spds/sources/* | grep -v addressbook`
	perl -pi -e 's;SyncEvolution test #1;;;' \
	         -e 's;^type = text/(x-)?vcard;type = addressbook;m;' \
                 syncevolution-iphone/usr/share/doc/syncevolution/*/spds/sources/addressbook/config.txt
	cd syncevolution-iphone && zip -r ../$(IPHONE_FILENAME) .

TYPE_deb = -D
TYPE_rpm = -R
# Dependency calculation is intentionally incomplete:
# - don't force dependency on specific EDS libs via backends, their versions change too much (handled via --enable-evolution-compatibility and dynamic loading of the backends)
# - ignore client-test dependencies (because users typically don't run it)
# - ignore backend dependencies (should never prevent installing the bundle)
# - be more flexible about kdelibs5 than dpkg-shlibdeps: it is found as package
#   for libkdeui.so.5 and libkdecore.so.5 on Ubuntu Lucid, but after Debian
#   Squeeze the package was replaced by individual library packages. On such
#   distros, libkdeui5 is what we need.
# - same for kdepimlibs5 -> libakonadi-kde4
# - kdebase-runtime became kde-runtime in Debian Wheezy
REQUIRES_SED_KDE = -e 's/kdelibs5 ([^,]*),/kdelibs5 | libkdeui5,/g' -e 's/kdepimlibs5 ([^,]*),/kdepimlibs5 | libakonadi-kde4,/g' -e 's/kdebase-runtime/kdebase-runtime | kde-runtime/g'
REQUIRES_deb = --requires="'$(shell set -x; cd checkinstall/dist; LD_LIBRARY_PATH=$(distdir)/usr/lib:$(distdir)/usr/lib/syncevolution dpkg-shlibdeps -L$(srcdir)/src/shlibs.local --ignore-missing-info -O $$(for i in $$(find $(distdir) -type f -perm /u+x | grep -v -e client-test -e lib/syncevolution/backends/); do if file $$i | grep ELF >/dev/null; then echo $$i; fi; done) | sed $(REQUIRES_SED_KDE) -e 's/[^=]*=//')$(REQUIRES_deb_neon)$(REQUIRES_deb_ical)'"
if NEON_COMPATIBILITY
# --enable-neon-compatibility in src/backends/webdav:
# replace dependencies from linking with hard-coded dlopen() dependencies
REQUIRES_deb_neon = , libneon27 (>= 0.29.0) | libneon27-gnutls (>= 0.29.0)
else
REQUIRES_deb_neon =
endif
if ENABLE_ICAL
REQUIRES_deb_ical = , libical0 | libical1
endif
VERSION_deb = 1:$(STABLE_VERSION)$(VERSION)
VERSION_rpm = `echo $(VERSION) | sed -e s/-/_/g`
RELEASE = 2

# The package name: BINSUFFIX is used to distinguish binaries
# for different Evolution releases.
PKGNAME=syncevolution$(patsubst %,-%,$(BINSUFFIX))

# This is a list of packages (potentially) provided on estamos.de.
# The current package conflicts with any of them, but not itself.
PKGS = $(addprefix syncevolution-evolution-, 2.6 2.8 2.12)

# When calling checkinstall we cannot install into /tmp
# because any file created there will be excluded: that makes
# sense, because "make install" might create temporary files
# there. The current directory might be in /tmp, so use $HOME.
#
# --replaces is necessary for migrating from syncevolution-evolution-<evover>
# to syncevolution-evolution (as per http://wiki.debian.org/Renaming_a_Package)
#
# When we build shared objects, then conflict with the corresponding
# system libs. The assumption is that the system library is named
# after the lib and its major version, which holds for libsmltk and
# libsynthesis in Debian.
deb rpm : checkinstall/dist/$(distdir) checkinstall/dist/debian/control
	(echo "SyncEvolution - synchronizing personal information management data" && cat $(srcdir)/description) >description-pak
	conflicts=`ls -1 checkinstall/dist/$(distdir)/usr/lib/*.so.[0123456789] | sed -e 's;.*/;;' -e 's/\.so\.//' -e 's/$$/, /'` && \
	tmpdir=`mktemp -d $$HOME/syncevolution.XXXXXXXXXX` && \
	trap "rm -rf $$tmpdir" EXIT && \
	cd checkinstall && \
	fakeroot checkinstall </dev/null \
		$(TYPE_$@) \
		$(REQUIRES_$@) \
		--fstrans=yes \
		--strip=no \
		--pkgversion=$(VERSION_$@) \
		--pkgrelease=$(RELEASE) \
		--pkgname=$(PKGNAME) \
		--pkgarch=$(PKGARCH) \
		--provides=syncevolution \
	        --replaces="'syncevolution, `echo $(filter-out $(PKGNAME), $(PKGS)) | sed -e 's/  */ (<< 1:0.8.1-2), /g'` (<< 1:0.8.1-2)'" \
		--conflicts="'$${conflicts}syncevolution, `echo $(filter-out $(PKGNAME), $(PKGS)) | sed -e 's/  */ (<< 1:0.8.1-2), /g'` (<< 1:0.8.1-2)', libsmltk0, libsyncevolution0, libsynthesis0, syncevolution-common, syncevolution-libs" \
		--maintainer="'Patrick Ohly <patrick.ohly@gmx.de>'" \
		--pkgsource='http://syncevolution.org' \
		--pkggroup='$*' \
		--pkggroup='comm' \
		cp -r `pwd`/dist/$(distdir)/* / && \
	if [ $@ = "rpm" ]; then cp /usr/src/rpm/RPMS/*/${PKGNAME}-${VERSION}-2.*.rpm ..; else mv ${PKGNAME}_${VERSION}*.deb ..; fi

# Pseudo platform .deb packages:
# only depend on PKGNAME = syncevolution-$(BINSUFFIX) (usually syncevolution-bundle).
# Version number is only bumped when changing the packaging rules here.
PLATFORM_DEB_VERSION = $(VERSION_deb)
PLATFORM_DEB_RELEASE = 1
if ENABLE_MODULES
deb : syncevolution-kde-deb
deb : syncevolution-evolution-deb
endif

# define which files are relevant for each platform
PLATFORM_FILES_evolution = platformgnome syncecal syncebook
PLATFORM_FILES_kde = platformkde syncakonadi

# Additional parameters for syncevolution-kde/evolution.deb.
# Fake conflicts/replaces works around a bug in CheckInstall,
# which creates empty, invalid entries for those unless
# something is given. For Evolution we hard-code dependencies
# (because with EDS compatibility turned on, several
# alternatives will work at runtime) and conflict with
# EDS 3.6, because that breaks the API and does not work for us
# at the moment.
SYNCEVOLUTION_kde_DEB_ARGS = \
	--conflicts=syncevolution-foobar \
        --replaces=syncevolution-foobar
SYNCEVOLUTION_evolution_DEB_ARGS = \
	--conflicts="'evolution-data-server (>= 3.6), libebook-1.2-14 (>= 3.6), libecal-1.2.12 (>= 3.6)'" \
	--replaces=syncevolution-foobar
SYNCEVOLUTION_evolution_DEB_REQUIRES = \
	, libebook1.2-5 | libebook1.2-6 | libebook1.2-7 | libebook1.2-8 | libebook1.2-9 | libebook1.2-10 | libebook1.2-11 | libebook-1.2-11 | libebook-1.2-12 | libebook-1.2-13 \
	, libecal1.2-3 | libecal1.2-4 | libecal1.2-5 | libecal1.2-6 | libecal1.2-7 | libecal1.2-8 | libecal1.2-9 | libecal1.2-10 | libecal-1.2-10 | libecal-1.2-11

syncevolution-%-deb: checkinstall/dist/$(distdir) checkinstall/dist/debian/control
	(echo "SyncEvolution - meta package for $*" && cat $(srcdir)/description) >checkinstall/description-pak
	tmpdir=`mktemp -d $$HOME/syncevolution.XXXXXXXXXX` && \
	trap "rm -rf $$tmpdir" EXIT && \
	cd checkinstall && \
	fakeroot checkinstall </dev/null \
		$(TYPE_deb) \
		--fstrans=yes \
		--strip=no \
                --nodoc \
		--pkgversion=$(PLATFORM_DEB_VERSION) \
		--pkgrelease=$(PLATFORM_DEB_RELEASE) \
		$(SYNCEVOLUTION_$*_DEB_ARGS) \
		--pkgname=syncevolution-$* \
		--pkgarch=all \
                --requires="'$(shell set -x; cd checkinstall/dist; LD_LIBRARY_PATH=$(distdir)/usr/lib:$(distdir)/usr/lib/syncevolution dpkg-shlibdeps -L$(srcdir)/src/shlibs.local --ignore-missing-info -O $$(for i in $(patsubst %,$(distdir)/usr/lib/syncevolution/backends/%.so,$(PLATFORM_FILES_$*)); do if file $$i | grep ELF >/dev/null; then echo $$i; fi; done) | sed $(REQUIRES_SED_KDE) -e 's/[^=]*=//'), $(PKGNAME) (= $(VERSION_deb)-$(RELEASE))$(SYNCEVOLUTION_$*_DEB_REQUIRES)'" \
		--maintainer="'Patrick Ohly <patrick.ohly@gmx.de>'" \
		--pkgsource='http://syncevolution.org' \
		--pkggroup='$*' \
		sh -c 'mkdir -p touch /usr/share/doc/syncevolution-$* && echo "SyncEvolution platform pseudo-package for $*" >/usr/share/doc/syncevolution-$*/README' && \
	mv syncevolution-$**.deb ..

all_phonies += checkinstall/dist/$(distdir) clean_dist
checkinstall/dist/$(distdir): all
	rm -rf $@
	$(MAKE) install DESTDIR=`pwd`/$@
	$(MAKE) installcheck DESTDIR=`pwd`/$@
	$(call ADD_EXTRA_BACKENDS, $@)
	rm -rf $(call DEV_FILE_PATTERN,$@,/usr)
clean-local: clean_dist
clean_dist:
	rm -rf checkinstall

# required by dpkg-shlibdeps
checkinstall/dist/debian/control:
	mkdir -p ${dir $@}
	touch $@

# Build "html" inside the build dir, using source files
# from the SyncEvolution source directory and (if built)
# the installed client-api.build directory.
all_phonies += doc clean-html
doc:
	rm -rf html
	export VERSION="SyncEvolution $(VERSION)"; \
	export OUTPUT_DIRECTORY="`pwd`"; \
	export PREDEFINED="@BACKEND_DEFINES@ ENABLE_UNIT_TESTS ENABLE_INTEGRATION_TESTS"; \
	export CLIENT_LIBRARY="@FUNAMBOL_SUBDIR@/test @FUNAMBOL_SUBDIR@/include "; \
	export STRIP_FROM_PATH="$(srcdir) `dirname @FUNAMBOL_SUBDIR@`"; \
	cd $(srcdir); doxygen
clean-local: clean-html
clean-html:
	rm -rf html

all_dist_hooks += dot_dist_hook
dot_dist_hook:
	@if test -d "$(srcdir)/.git"; \
	then \
	  printf '%s' 'Creating ChangeLog...' && \
	  ( cd "$(top_srcdir)" && \
	    printf '%s\n\n' '# Generated by configure.  Do not edit.'; \
	    $(top_srcdir)/missing --run perl $(top_srcdir)/build/gen-changelog.pl ) > ChangeLog.tmp && \
	  ( mv -f ChangeLog.tmp $(top_distdir)/ChangeLog && \
	    printf '%s\n' ' done.' ) || \
	  ( rm -f ChangeLog.tmp ; \
	    printf '%s\n' ' failed.'; \
	    echo Failed to generate ChangeLog >&2 ); \
	else \
	  echo 'A git checkout is required to generate a ChangeLog.' >&2; \
	fi

if ENABLE_EVOLUTION_COMPATIBILITY
# check .so (relevant for modular builds) and main syncevolution binary
# (relevant in that case and for static builds) for dependencies on
# problematic libraries and symbols
#
# Exclude *-2.so, these are EXTRA_BACKENDS for which other rules apply.
#
# ical_strdup is an exception because it is in SyncEvolution.
all_local_installchecks += toplevel_so_check
toplevel_so_check:
	for i in `find $(DESTDIR)/$(libdir)/syncevolution $(DESTDIR)/$(libdir)/libsyncevo* $(DESTDIR)/$(libdir)/libsynthesis* -name *.so | grep -v -2.so` $(DESTDIR)/$(bindir)/syncevolution; \
	do \
	  if objdump -T -C $$i | grep -v :: | grep '\*UND\*' | sort | grep -v -w ical_strdup | grep -e ical -e " e_"; then \
	    echo "$$i should not depend on EDS, libical or libbluetooth"; \
	   exit 1; \
	  fi; \
	  if ldd $$i | grep -e libecal -e libebook -e libedata -e libical -e libbluetooth; then \
	    echo "$$i should not be linked against EDS, libical or libbluetooth"; \
	    exit 1; \
	  fi; \
	done
endif

# Check that no executable or shared object depends on symbols in
# libraries that it does not link against. Unnecessarily linking
# against libs is okay, that can be caught and fixed by
# -Wl,--as-needed. Depends on dpkg-shlibdeps, skipped if that is
# not available.
all_local_installchecks += toplevel_link_check
toplevel_link_check:
	set -x; cd $(DESTDIR) && \
	mkdir debian && \
	touch debian/control && \
        trap "rm -rf debian" EXIT && \
	files=$$(find $(DESTDIR)/$(prefix) $(DESTDIR)/$(libdir) $(DESTDIR)/$(bindir) $(DESTDIR)/$(libexecdir) -type f -perm /u+x | sort -u) && \
	files=$$(for i in $$files; do if file $$i | grep ELF >/dev/null; then echo $$i; fi; done) && \
	if ! dpkg-shlibdeps --version; then \
	    echo "dpkg-shlibdeps not found, skipping link check"; \
	elif LD_LIBRARY_PATH=usr/lib:usr/lib/syncevolution dpkg-shlibdeps \
             --ignore-missing-info -O $$files \
	     2>&1 >/dev/null | \
             grep -v $(LINK_CHECK_ALLOWED) | \
             grep -e "symbol .* found in none of the libraries" \
                  -e "contains an unresolvable reference to symbol" \
        ; then \
	    echo "linking must be fixed"; false; \
        else \
	    echo "linking is okay"; \
	fi

# Some exceptions for the link check above (= symbol may be used without linking).
LINK_CHECK_ALLOWED = -e xxxxxxxx
# SySync_ConsolePrintf is expected by libsmltk and has to be provided by caller.
LINK_CHECK_ALLOWED += -e 'SySync_ConsolePrintf.*libsmltk.so'
if NEON_COMPATIBILITY
# libneon is intentionally not linked against, to choose between
# GNUTLS and OpenSSL at runtime.
LINK_CHECK_ALLOWED += -e 'symbol ne_.*syncdav.so'
# Allow undefined references to libstdcxx. This happens when
# adding backends compiled on more recent Linux distros into
# the release archive.
LINK_CHECK_ALLOWED += -e '@GLIBCXX_[^ ]* used by'
endif

# Be strict about running 'syncevolution' only when not doing
# cross-compilation: in that case, if running 'syncevolution' fails,
# abort the build process. Otherwise proceed with the fallback below,
# which is to keep the "see --sync/source-property ?" placeholders in
# the README.
if COND_CROSS_COMPILING
RUN_SYNCEVOLUTION_CHECK=if ($$?) { return ""; } else { return $$buffer; }
else
RUN_SYNCEVOLUTION_CHECK=die if $$?; return $$buffer;
endif

# patch README.rst properties on-the-fly
README.patched.rst: README.rst src/syncevolution
	$(AM_V_GEN)perl -e '$$syncfound=0; $$sourcefound=0; $$res=0;' \
	                -e 'sub run { $$cmd = shift; $$buffer = `env LD_LIBRARY_PATH=src/syncevo/.libs:src/gdbus/.libs:src/gdbusxx/.libs:src/build-synthesis/src/.libs:$$ENV{LD_LIBRARY_PATH} $$cmd`; $(RUN_SYNCEVOLUTION_CHECK) }' \
	                -e 'while (<>) {' \
	                -e 's/^:Version: .*/:Version: $(VERSION)/;' \
	                -e 's/:Date: .*/":Date: " . `date +%Y-%m-%d`/e;' \
	                -e 'if (s;(<< see "syncevolution --sync-property ." >>\n);run("src/syncevolution --daemon=no --sync-property ?") || $$1;e) { $$syncfound=1; }' \
	                -e 'if (s;(<< see "syncevolution --source-property ." >>\n);run("src/syncevolution --daemon=no --source-property ?") || $$1;e) { $$sourcefound=1; }' \
	                -e 'print;' \
	                -e '}' \
	                -e 'die "<<sync-property>> tag not in README.rst?!" unless $$syncfound;' \
	                -e 'die "<<source-property>> tag not in README.rst?!" unless $$sourcefound;' \
	                -e 'exit $$res;' \
	                $< >$@
CLEANFILES += README.patched.rst

# produce man pages
syncevolution.1: README.patched.rst
	$(AM_V_GEN)$(RST2MAN) --exit-status=3 $< >$@
CLEANFILES += syncevolution.1

# README is the traditional name in the distribution,
# continue using it instead of README.rst.
# TODO: replace some of the RST syntax
README: README.patched.rst
	$(AM_V_GEN)cp $< $@
CLEANFILES += README

# The README.html is also used on syncevolution.org as "Usage" page,
# therefore we must use <h3> headers and lower to fit into the page.
README.html: README.patched.rst
	$(AM_V_GEN)$(RST2HTML) --initial-header-level=3 --exit-status=3 $< >$@
CLEANFILES += README.html

.PHONY: $(all_phonies) ;

installcheck-local: $(all_local_installchecks) ;

dist-hook: $(all_dist_hooks)
install-exec-hook: $(all_install_exec_hooks)
uninstall-hook: $(all_uninstall_hooks)

# Force sequential installation. This is a workaround for relinking failures
# during concurrent distcheck (a backend was relinked against not yet installed
# libsyncevolution.la).
#
# Also used to add additional backends.
install-am: all-am
	@$(MAKE) $(AM_MAKEFLAGS) install-exec-am
	@$(MAKE) $(AM_MAKEFLAGS) install-data-am
	for i in $(EXTRA_BACKENDS); do $(INSTALL) $$i $(DESTDIR)/$(BACKENDS_DIRECTORY)/`basename $$i .so`-2.so; done

# Necessary for "make distcheck": must not leave files behind.
uninstall-local:
	rm -f $(DESTDIR)/$(BACKENDS_DIRECTORY)/*-2.so

.DELETE_ON_ERROR:
