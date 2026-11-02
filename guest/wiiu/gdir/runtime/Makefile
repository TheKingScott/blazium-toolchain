# Blazium Wii U GDIR guest: the PS2 guest's scene runtime (GDIR VM, sys_io,
# pack_io) over the Wii U's GX2, GamePad and Wii Remotes, AX and SD card,
# built with devkitPPC and wut.
#
# blazium-toolchain ships this guest compiled (guest/wiiu/prebuilt/gdir.rpx),
# so wiiu build needs no SDK; it only runs this Makefile for a guest with
# code of your own (--overlay, GDIR_EXTRA_FLAGS). The toolchain writes the
# .wuhb (the RPX, its icon and name, the game's DATA) itself.
#
# make   gdir.elf, and gdir.rpx from it (elf2rpl)

ifeq ($(strip $(DEVKITPPC)),)
$(error DEVKITPPC is not set (devkitPro's devkitPPC folder))
endif
DEVKITPRO	?= $(abspath $(DEVKITPPC)/..)
WUT_ROOT	?= $(DEVKITPRO)/wut

CXX		:= $(DEVKITPPC)/bin/powerpc-eabi-g++
ELF2RPL		?= elf2rpl

SOURCES		:= $(wildcard *.cpp) $(wildcard extra/*.cpp)
OFILES		:= $(SOURCES:.cpp=.o)

MACHDEP		:= -DESPRESSO -mcpu=750 -meabi -mhard-float

# the GDIR VM's tables at the PS2's sizes (and the PS2's pack budgets, which
# the scripts can read); "wiiu" for OS.has_feature
DEFINES		:= -DBLAZIUM_WIIU_GDIR=1 '-DGDIR_FEATURE="wiiu"' -DBLAZIUM_PS2_COOK_ABI=1 \
		   -DPS2_POKE_N=2048u -DGDIR_POKE_BYTES=2048 -DGDIR_ARRAYS=128 -DGDIR_MAX_NODES=512 \
		   -D__WIIU__ -D__WUT__ $(GDIR_EXTRA_FLAGS)

# -fno-tree-sra -fno-ipa-sra: GCC otherwise splits the VM's values (a type
# byte and a union of int, float and pointer) into scalars of the union's
# float member, and copies an int through lfs/stfs; the Wii U keeps its bits,
# but Cemu's recompiler flushes the denormal floats such small ints make to
# zero (a script's argument 1 arrived as 0)
CXXFLAGS	:= -O2 $(MACHDEP) -fno-tree-sra -fno-ipa-sra -fno-exceptions -fno-rtti -fno-strict-aliasing -fno-builtin-printf -Wall \
		   -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare \
		   -Wno-format -I. -I$(WUT_ROOT)/include $(DEFINES)
LDFLAGS		:= $(MACHDEP) -specs=$(WUT_ROOT)/share/wut.specs -Wl,--wrap=printf -Wl,-Map,gdir.map
LIBS		:= -L$(WUT_ROOT)/lib -lwut -lm

.PHONY: all clean
all: gdir.rpx

gdir.elf: $(OFILES)
	@echo linking ... $@
	@$(CXX) $^ $(LDFLAGS) $(LIBS) -o $@

gdir.rpx: gdir.elf
	@$(ELF2RPL) $< $@
	@echo wrote $@

%.o: %.cpp
	@echo $<
	@$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OFILES) gdir.elf gdir.rpx gdir.map
