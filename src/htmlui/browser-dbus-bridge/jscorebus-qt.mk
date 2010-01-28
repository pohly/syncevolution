NAME := jscorebus-qt
VERSION := 1.1
SOURCES := $(wildcard jscorebus/*.c)
CFLAGS := -ggdb -DDEBUG
PKGS := dbus-1 dbus-glib-1 QtWebKit

INCDIR := $(DESTDIR)/$(PREFIX)/include/$(NAME)
PCDIR := $(DESTDIR)/$(PREFIX)/lib/pkgconfig

include build/library.mk

install::
ifneq ($(wildcard $(O_LIBRARY)),)
	install -d $(INCDIR) $(PCDIR)
	install -m 0644 jscorebus/jscorebus.h $(INCDIR)
	install -m 0644 jscorebus/jscorebus.pc.in $(PCDIR)/$(NAME).pc
	sed -i s,@PREFIX@,$(PREFIX), $(PCDIR)/$(NAME).pc
	sed -i s,@VERSION@,$(VERSION), $(PCDIR)/$(NAME).pc
	sed -i 's,@REQUIRES@,$(PKGS),' $(PCDIR)/$(NAME).pc
	sed -i s,@NAME@,$(NAME), $(PCDIR)/$(NAME).pc
else
	@echo JSCoreBus for QtWebKit has not been built, not installing
endif
