O		?= debug
# V

# CONFIG

# CROSS_COMPILE
CCACHE		?= $(if $(shell which ccache),ccache,)

ifeq ($(origin CC),default)
CC		:= $(CCACHE) $(CROSS_COMPILE)gcc
endif

ifeq ($(origin CXX),default)
CXX		:= $(CCACHE) $(CROSS_COMPILE)g++
endif

ifeq ($(origin AR),default)
AR		:= $(CCACHE) $(CROSS_COMPILE)ar
endif

CCLINKER	?= $(CC)
CXXLINKER	?= $(CXX)

PKG_CONFIG	?= pkg-config
# PKG_CONFIG_PATH

# CPPFLAGS
CFLAGS		?= -g -Wall
# CCFLAGS
# CXXFLAGS
# LDFLAGS
# LIBS

CXXPATTERNS	?= %.cc %.cp %.cxx %.cpp %.CPP %.c++ %.C

PREFIX		?= /usr/local
BINDIR		?= $(PREFIX)/bin
SBINDIR		?= $(PREFIX)/sbin
LIBDIR		?= $(PREFIX)/lib
PLUGINDIR	?= $(LIBDIR)
DATADIR		?= $(PREFIX)/share
SYSCONFDIR	?= /etc

# DESTDIR

# LD_LIBRARY_PATH

include build/common.mk

ifneq ($(wildcard $(O)/config.mk),)
 include $(O)/config.mk
else
 -include $(O).mk
endif

export O V
export CONFIG
export CC CXX CCLINKER CXXLINKER AR
export PKG_CONFIG PKG_CONFIG_PATH
export CPPFLAGS CFLAGS CCFLAGS CXXFLAGS LDFLAGS LIBS
export CXXPATTERNS
export PREFIX BINDIR SBINDIR LIBDIR PLUGINDIR DATADIR SYSCONFDIR
export DESTDIR
export TEST_LIBRARY_PATH := $(LD_LIBRARY_PATH)

DIST		?= $(BINARIES) $(LIBRARIES) $(PLUGINS)
DO_DIST		:= $(filter-out $(NODIST),$(DIST))

LIBRARY_TARGETS	:= $(foreach L,$(LIBRARIES),$(L)-shared $(L)-static)
PLUGIN_TARGETS	:= $(foreach L,$(PLUGINS),$(L)-plugin)
TARGETS		:= $(BINARIES) $(TESTS) $(LIBRARY_TARGETS) $(PLUGIN_TARGETS)
CHECK_TARGETS	:= $(foreach T,$(TESTS),check-$(T))
INSTALL_TARGETS	:= $(foreach I,$(DO_DIST),install-$(I))

build:
check: $(CHECK_TARGETS)
all: build check
install: $(INSTALL_TARGETS)

clean:
	$(QUIET) test "$(PWD)" && readlink -f "$(O)" | grep -q "^$(PWD)/."
	$(call echo,Remove,$(O))
	$(QUIET) if [ -d "$(O)" ]; then rm -r "$(O)"; fi

makefile	= $(firstword $(wildcard $(1).mk) $(1)/build.mk)
librarymakefile	= $(call makefile,$(patsubst %-static,%,$(patsubst %-shared,%,$(1))))
librarytarget	= build-$(call lastword,$(subst -, ,$(1)))
pluginmakefile	= $(call makefile,$(patsubst %-plugin,%,$(1)))
plugintarget	= build-$(call lastword,$(subst -, ,$(1)))
testmakefile	= $(call makefile,$(patsubst check-%,%,$(1)))
distmakefile	= $(call makefile,$(patsubst install-%,%,$(1)))

$(BINARIES) $(TESTS):
	$(QUIET) $(MAKE) --no-print-directory -f $(call makefile,$@) build

$(PLUGIN_TARGETS):
	$(QUIET) $(MAKE) --no-print-directory -f $(call pluginmakefile,$@) $(call plugintarget,$@)

$(LIBRARY_TARGETS):
	$(QUIET) $(MAKE) --no-print-directory -f $(call librarymakefile,$@) $(call librarytarget,$@)

$(CHECK_TARGETS): $(TESTS)
	$(QUIET) $(MAKE) --no-print-directory -f $(call testmakefile,$@) check

$(INSTALL_TARGETS): $(DO_DIST)
	$(QUIET) $(MAKE) --no-print-directory -f $(call distmakefile,$@) install

.PHONY: build install all check clean
.PHONY: $(TARGETS) $(CHECK_TARGETS) $(INSTALL_TARGETS) $(LIBRARIES) $(PLUGINS)
