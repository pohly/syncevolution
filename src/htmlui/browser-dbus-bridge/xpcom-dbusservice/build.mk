NAME	= dbusservice
SOURCES	= $(wildcard xpcom-dbusservice/*.cpp)

# Only check for libxul and nspr if GECKO_SDK_PATH is not set
ifeq ($(GECKO_SDK_PATH),)
PKGS	= dbus-glib-1 libxul nspr
else
PKGS	= dbus-glib-1
endif

# These are wild guesses, but should be close
GECKO_PREFIX = $(shell pkg-config --variable=prefix mozilla-plugin)
GECKO_COMPONENT_DIR := $(shell ls -d $(GECKO_PREFIX)/lib/xulrunner-[0-9]* 2>/dev/null | head -1)/components
GECKO_SDK_PATH := $(shell pkg-config --variable=sdkdir mozilla-plugin)
XPIDL	= $(GECKO_SDK_PATH)/bin/xpidl
PLUGINDIR := $(GECKO_COMPONENT_DIR)

CFLAGS	+= -I$(GECKO_SDK_PATH)/include \
          -I$(GECKO_SDK_PATH)/include/xpcom \
          -I$(GECKO_SDK_PATH)/include/nspr \
          -I$(GECKO_SDK_PATH)/include/string \
          -I$(GECKO_SDK_PATH)/include/embedstring \
          -I$(GECKO_SDK_PATH)/include/xpconnect \
          -I$(GECKO_SDK_PATH)/include/js \
          -I$(O)/include \
          -DNO_NSPR_10_SUPPORT \
          -Wno-non-virtual-dtor

CFLAGS	+= -fno-rtti -fno-exceptions

LDFLAGS	+= -L$(GECKO_SDK_PATH)/lib
LIBS	+= -lxpcomglue_s -lnspr4 -lplds4

include build/plugin.mk

$(O)/include/IDBusService.h: xpcom-dbusservice/IDBusService.idl
	$(call echo,Generate,$@)
	@mkdir -p $(dir $@)
	@$(XPIDL) -I $(GECKO_SDK_PATH)/idl -m header -e $@ $<

$(O)/xpt/dbusservice.xpt: xpcom-dbusservice/IDBusService.idl
	$(call echo,Generate,$@)
	@mkdir -p $(dir $@)
	@$(XPIDL) -I $(GECKO_SDK_PATH)/idl -m typelib -e $@ $<

$(PIC_OBJECTS) $(OBJECTS): $(O)/include/IDBusService.h $(O)/xpt/dbusservice.xpt

install:: install-xpt

install-xpt:
ifneq ($(wildcard $(O_PLUGIN)),)
	mkdir -p $(DEST_PLUGINDIR)
	cp $(O)/xpt/dbusservice.xpt $(DEST_PLUGINDIR)/
else
	@echo XPCOM D-Bus service has not been built, not installing
endif

.PHONY: install-xpt
