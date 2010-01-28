CCCOMMAND	:= $(CC) $(GENERICFLAGS) $(CCFLAGS)
LINKCOMMAND	:= $(CCLINKER) $(CFLAGS) $(CCFLAGS) $(LDFLAGS)

$(O)/obj/%.o: %.c
	$(call echo,Compile,$@)
	$(QUIET) mkdir -p $(dir $@)
	$(QUIET) $(CCCOMMAND) \
		-MF $(O)/obj/$(patsubst %,%.d,$*.c) \
		-MT $(O)/obj/$*.o -MT $(O)/obj/$*.os \
		-o $@ $*.c

$(O)/obj/%.os: %.c
	$(call echo,Compile,$@)
	$(QUIET) mkdir -p $(dir $@)
	$(QUIET) $(CCCOMMAND) $(PICFLAGS) \
		-MF $(O)/obj/$(patsubst %,%.d,$*.c) \
		-MT $(O)/obj/$*.o -MT $(O)/obj/$*.os \
		-o $@ $*.c
