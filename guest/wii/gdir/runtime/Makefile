# Blazium Wii GDIR guest: the PS2 guest's scene runtime (GDIR VM, sys_io,
# pack_io) over the Wii's GX, Wii Remotes and GameCube controllers, audio
# interface and SD card, built with devkitPPC, libogc and libfat.
#
# blazium-toolchain ships this guest compiled (guest/wii/prebuilt:
# boot.dol), so wii build needs no SDK; it only runs this Makefile for a
# guest with code of your own (--overlay, GDIR_EXTRA_FLAGS).
#
# make   gdir.elf, and boot.dol from it (elf2dol)

ifeq ($(strip $(DEVKITPPC)),)
$(error DEVKITPPC is not set (devkitPro's devkitPPC folder))
endif
DEVKITPRO	?= $(abspath $(DEVKITPPC)/..)
LIBOGC		?= $(DEVKITPRO)/libogc
PORTLIBS	?= $(DEVKITPRO)/portlibs/wii

CXX		:= $(DEVKITPPC)/bin/powerpc-eabi-g++
ELF2DOL		?= elf2dol

SOURCES		:= $(wildcard *.cpp) $(wildcard extra/*.cpp)
OFILES		:= $(SOURCES:.cpp=.o)

MACHDEP		:= -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float

# the GDIR VM's tables at the PS2's sizes, and the PS2's memory budget (the
# Wii's 88 MB leave it room); "wii" for OS.has_feature
DEFINES		:= -DBLAZIUM_WII_GDIR=1 '-DGDIR_FEATURE="wii"' -DBLAZIUM_PS2_COOK_ABI=1 \
		   -DPS2_POKE_N=2048u -DGDIR_POKE_BYTES=2048 -DGDIR_ARRAYS=128 -DGDIR_MAX_NODES=512 \
		   -DPS2_EE_LIMIT=12582912u -DPS2_GS_LIMIT=4194304u -DPS2_EXTRA_EE_CAP=4194304u \
		   -DPACK_IO_HEAP_REPORT=wii_heap_report $(GDIR_EXTRA_FLAGS)

CXXFLAGS	:= -O2 $(MACHDEP) -fno-exceptions -fno-rtti -fno-strict-aliasing -Wall -Wno-unused-function \
		   -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare -I. -I$(LIBOGC)/include \
		   -I$(PORTLIBS)/include $(DEFINES)
LDFLAGS		:= $(MACHDEP) -Wl,-Map,gdir.map -L$(LIBOGC)/lib/wii -L$(PORTLIBS)/lib
LIBS		:= -lfat -lwiiuse -lbte -logc -lm

.PHONY: all clean
all: boot.dol

gdir.elf: $(OFILES)
	@echo linking ... $@
	@$(CXX) $^ $(LDFLAGS) $(LIBS) -o $@

boot.dol: gdir.elf
	@$(ELF2DOL) $< $@
	@echo wrote $@

%.o: %.cpp
	@echo $<
	@$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OFILES) gdir.elf boot.dol gdir.map
