FOUND_PKGS = 0

ifneq ($(PKGS),)

 ifeq ($(shell $(PKG_CONFIG) --exists --print-errors $(PKGS) 2>/dev/null && echo ok),)
 else
  FOUND_PKGS = 1
 endif

ifeq ($(FOUND_PKGS),1)

CPPFLAGS	+= $(shell $(PKG_CONFIG) --cflags-only-I $(PKGS))
CFLAGS		+= $(shell $(PKG_CONFIG) --cflags-only-other $(PKGS))
LDFLAGS		+= $(shell $(PKG_CONFIG) --libs-only-other $(PKGS))
LIBS		+= $(shell $(PKG_CONFIG) --libs-only-L $(PKGS)) \
		   $(shell $(PKG_CONFIG) --libs-only-l $(PKGS))
endif

endif

pkg-check:
ifeq ($(FOUND_PKGS),0)
	$(error $(shell $(PKG_CONFIG) --exists --print-errors $(PKGS)))
endif
