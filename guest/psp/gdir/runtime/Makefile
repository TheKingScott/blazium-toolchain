# Blazium PSP GDIR guest: the PS2 guest's scene runtime (GDIR VM, sys_io,
# pack_io) over the PSP's GE, controls, audio and Memory Stick, built with
# pspdev's toolchain and PSPSDK as a user-mode PRX.
#
# blazium-toolchain ships this guest compiled (guest/psp/prebuilt:
# gdir.prx), so psp build needs no SDK; it only runs this Makefile for a
# guest with code of your own (--overlay, GDIR_EXTRA_FLAGS), and packs the
# EBOOT.PBP itself.
#
# make   gdir.elf, and gdir.prx from it (psp-fixup-imports, psp-prxgen)

ifeq ($(strip $(PSPDEV)),)
$(error PSPDEV is not set (pspdev's folder, /usr/local/pspdev))
endif
PSPSDK		:= $(PSPDEV)/psp/sdk
BIN		:= $(PSPDEV)/bin

CXX		:= $(BIN)/psp-g++

SOURCES		:= $(wildcard *.cpp) $(wildcard extra/*.cpp)
OFILES		:= $(SOURCES:.cpp=.o)

# the GDIR VM's tables at the PS2's sizes, and the PS2's memory budget;
# "psp" for OS.has_feature
DEFINES		:= -DBLAZIUM_PSP_GDIR=1 '-DGDIR_FEATURE="psp"' -DBLAZIUM_PS2_COOK_ABI=1 -D_PSP_FW_VERSION=600 \
		   -DPS2_POKE_N=2048u -DGDIR_POKE_BYTES=2048 -DGDIR_ARRAYS=128 -DGDIR_MAX_NODES=512 \
		   -DPS2_EE_LIMIT=12582912u -DPS2_GS_LIMIT=4194304u -DPS2_EXTRA_EE_CAP=4194304u \
		   -DPACK_IO_HEAP_REPORT=psp_heap_report $(GDIR_EXTRA_FLAGS)

CXXFLAGS	:= -O2 -G0 -fno-exceptions -fno-rtti -fno-strict-aliasing -Wall -Wno-unused-function \
		   -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare -I. -I$(PSPSDK)/include $(DEFINES)
LDFLAGS		:= -L$(PSPSDK)/lib -specs=$(PSPSDK)/lib/prxspecs -Wl,-q,-T$(PSPSDK)/lib/linkfile.prx -Wl,-zmax-page-size=128
LIBS		:= -lpspgu -lpspaudio -lpsppower -lpsprtc -lpspdisplay -lpspge -lpspctrl -lm

.PHONY: all clean
all: gdir.prx

gdir.elf: $(OFILES)
	@echo linking ... $@
	@$(CXX) $(LDFLAGS) $^ $(PSPSDK)/lib/prxexports.o $(LIBS) -o $@
	@$(BIN)/psp-fixup-imports $@

gdir.prx: gdir.elf
	@$(BIN)/psp-prxgen $< $@
	@echo wrote $@

%.o: %.cpp
	@echo $<
	@$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OFILES) gdir.elf gdir.prx
