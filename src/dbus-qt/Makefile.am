include $(top_srcdir)/m4-repo/autotroll.mk

libsyncevolution_qt_dbus_includedir = $(includedir)/syncevolution-qt-dbus

lib_LTLIBRARIES = libsyncevolution-qt-dbus.la

libsyncevolution_qt_dbus_headers = dbustypes.h

libsyncevolution_qt_dbus_include_HEADERS = \
  $(libsyncevolution_qt_dbus_headers) \
  $(built_headers) \
  $(NULL)

libsyncevolution_qt_dbus_la_SOURCES = \
	$(BUILT_SOURCES) \
        $(libsyncevolution_qt_dbus_headers) \
	dbustypes.cpp

AM_CPPFLAGS = $(SYNCEVOLUTION_CFLAGS) -I$(top_srcdir)/test $(BACKEND_CPPFLAGS)

EXTRA_DIST = configure-sub.in

built_headers = \
  syncevo-server-full.h \
  syncevo-session-full.h \
  syncevo-connection-full.h \
  $(NULL)

BUILT_SOURCES = \
  syncevo-server-full.cpp \
  syncevo-session-full.cpp \
  syncevo-connection-full.cpp \
  $(built_headers) \
  $(NULL)

MOSTLYCLEANFILES = $(lib_LTLIBRARIES)

MAINTAINERCLEANFILES = Makefile.in

CLEANFILES = $(BUILT_SOURCES)

libsyncevolution_qt_dbus_la_LIBADD = $(QT_DBUS_LIBS) $(QT_LIBS)
libsyncevolution_qt_dbus_la_CPPFLAGS = $(AM_CPPFLAGS) $(QT_CPPFLAGS)
# Allow Qt to set some compile flags, but not the ones normally set via configure.
# In particular -W is not compatible with the SyncEvolution header files (we have
# unused parameters in some inline functions).
libsyncevolution_qt_dbus_la_CXXFLAGS = $(SYNCEVOLUTION_CXXFLAGS) $(filter-out -O2 -g -W -Wall, $(QT_CXXFLAGS))

DISTCLEANFILES = syncevolution-qt-dbus.pc
EXTRA_DIST = syncevolution-qt-dbus.pc.in
pkgconfig_DATA = syncevolution-qt-dbus.pc
pkgconfigdir = $(libdir)/pkgconfig

syncevo-server-full.cpp: ../dbus/interfaces/syncevo-server-full.xml
	if test -f $@; then touch $@; else qdbusxml2cpp -p $* -i dbustypes.h $< ; fi
syncevo-session-full.cpp: ../dbus/interfaces/syncevo-session-full.xml
	if test -f $@; then touch $@; else qdbusxml2cpp -p $* -i dbustypes.h $< ; fi
syncevo-connection-full.cpp: ../dbus/interfaces/syncevo-connection-full.xml
	if test -f $@; then touch $@; else qdbusxml2cpp -p $* -i dbustypes.h $< ; fi
syncevo-server-full.h: ../dbus/interfaces/syncevo-server-full.xml
	if test -f $@; then touch $@; else qdbusxml2cpp -p $* -i dbustypes.h $< ; fi
syncevo-session-full.h: ../dbus/interfaces/syncevo-session-full.xml
	if test -f $@; then touch $@; else qdbusxml2cpp -p $* -i dbustypes.h $< ; fi
syncevo-connection-full.h: ../dbus/interfaces/syncevo-connection-full.xml
	if test -f $@; then touch $@; else qdbusxml2cpp -p $* -i dbustypes.h $< ; fi
