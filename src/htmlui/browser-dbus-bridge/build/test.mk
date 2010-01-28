include build/binary.mk

check: build
	$(call echo,Check,$(O_NAME))
	$(QUIET) LD_LIBRARY_PATH=$(O)/lib:$(TEST_LIBRARY_PATH) $(O_NAME)

.PHONY: check
