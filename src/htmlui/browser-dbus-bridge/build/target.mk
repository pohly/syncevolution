$(if $(NAME),,$(error NAME not specified for target))
$(if $(SOURCES),,$(error SOURCES not specified for target))

-include build/pkgconfig.mk

CONFIGFLAGS	:= $(if $(CONFIG),-include $(CONFIG),)
GENERICFLAGS	:= $(CPPFLAGS) $(CONFIGFLAGS) $(CFLAGS) -c -MD
PICFLAGS	:= -DPIC -fPIC

-include build/c.mk
-include build/c++.mk

OBJECTS		:= $(patsubst %,$(O)/obj/%.o,$(basename $(SOURCES)))
PIC_OBJECTS	:= $(patsubst %,$(O)/obj/%.os,$(basename $(SOURCES)))

-include $(SOURCES:%=$(O)/obj/%.d)

$(OBJECTS) $(PIC_OBJECTS): pkg-check $(CONFIG)
