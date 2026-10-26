# Blazium GameCube GDIR guest: the PS2 guest's scene runtime (GDIR VM,
# sys_io, pack_io) over the GameCube's GX, pads, audio interface, Memory
# Card and disc drive, built with devkitPPC and libogc.
#
# blazium-toolchain ships this guest compiled (guest/gamecube/prebuilt:
# gdir.dol and the disc's apploader), so gamecube build needs no SDK; it
# only runs this Makefile for a guest with code of your own (--overlay,
# GDIR_EXTRA_FLAGS), and writes the disc image itself.
#
# make   gdir.elf, and gdir.dol from it (elf2dol)

ifeq ($(strip $(DEVKITPPC)),)
$(error DEVKITPPC is not set (devkitPro's devkitPPC folder))
endif
DEVKITPRO	?= $(abspath $(DEVKITPPC)/..)
LIBOGC		?= $(DEVKITPRO)/libogc

CXX		:= $(DEVKITPPC)/bin/powerpc-eabi-g++
ELF2DOL		?= elf2dol

SOURCES		:= $(wildcard *.cpp) $(wildcard extra/*.cpp)
OFILES		:= $(SOURCES:.cpp=.o)

MACHDEP		:= -DGEKKO -mogc -mcpu=750 -meabi -mhard-float

# the GDIR VM's tables at the PS2's sizes; the GameCube's 24 MB as the DSi's
# 16 (pack 0 stays in RAM); "gamecube" for OS.has_feature
DEFINES		:= -DBLAZIUM_GC_GDIR=1 '-DGDIR_FEATURE="gamecube"' -DBLAZIUM_PS2_COOK_ABI=1 \
		   -DPS2_POKE_N=2048u -DGDIR_POKE_BYTES=2048 -DGDIR_ARRAYS=128 -DGDIR_MAX_NODES=512 \
		   -DPS2_EE_LIMIT=12582912u -DPS2_GS_LIMIT=4194304u -DPS2_EXTRA_EE_CAP=4194304u \
		   -DPACK_IO_HEAP_REPORT=gc_heap_report $(GDIR_EXTRA_FLAGS)

CXXFLAGS	:= -O2 $(MACHDEP) -fno-exceptions -fno-rtti -fno-strict-aliasing -Wall -Wno-unused-function \
		   -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare -I. -I$(LIBOGC)/include $(DEFINES)
LDFLAGS		:= $(MACHDEP) -Wl,-Map,gdir.map -L$(LIBOGC)/lib/cube
LIBS		:= -logc -lm

.PHONY: all clean
all: gdir.dol

gdir.elf: $(OFILES)
	@echo linking ... $@
	@$(CXX) $^ $(LDFLAGS) $(LIBS) -o $@

gdir.dol: gdir.elf
	@$(ELF2DOL) $< $@
	@echo wrote $@

%.o: %.cpp
	@echo $<
	@$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OFILES) gdir.elf gdir.dol gdir.map
