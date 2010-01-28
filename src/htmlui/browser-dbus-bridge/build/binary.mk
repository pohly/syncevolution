include build/common.mk
include build/target.mk

O_NAME		:= $(O)/bin/$(NAME)

build: $(O_NAME)

$(O_NAME): $(OBJECTS) $(DEPENDS)
	$(call echo,Link,$@)
	$(QUIET) mkdir -p $(dir $@)
	$(QUIET) $(LINKCOMMAND) -o $@ $(OBJECTS) $(LIBS)

install::
	mkdir -p $(DEST_BINDIR)
	install $(O_NAME) $(DEST_BINDIR)/

.PHONY: build
