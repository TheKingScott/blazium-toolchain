# Blazium Switch GDIR guest: the PS2 guest's scene runtime (GDIR VM, sys_io,
# pack_io) over the Switch's GPU (deko3d), controllers, audout and SD card,
# built with devkitA64 and libnx.
#
# blazium-toolchain ships this guest compiled (guest/switch/prebuilt/gdir.nro),
# so switch build needs no SDK; it only runs this Makefile for a guest with
# code of your own (--overlay, GDIR_EXTRA_FLAGS). The toolchain adds the
# .nro's assets (the icon, the names, the RomFS with the game's DATA) itself.
#
# make   gdir.elf, and gdir.nro from it (elf2nro, with no assets)

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set (devkitPro's folder, with devkitA64 and libnx))
endif
DEVKITA64	?= $(DEVKITPRO)/devkitA64
LIBNX		?= $(DEVKITPRO)/libnx

CXX		:= $(DEVKITA64)/bin/aarch64-none-elf-g++
ELF2NRO		?= $(DEVKITPRO)/tools/bin/elf2nro

SOURCES		:= $(wildcard *.cpp) $(wildcard extra/*.cpp)
OFILES		:= $(SOURCES:.cpp=.o)

ARCH		:= -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# the GDIR VM's tables at the PS2's sizes (and the PS2's pack budgets, which
# the scripts can read); "switch" for OS.has_feature
DEFINES		:= -DBLAZIUM_SWITCH_GDIR=1 '-DGDIR_FEATURE="switch"' -DBLAZIUM_PS2_COOK_ABI=1 \
		   -DPS2_POKE_N=2048u -DGDIR_POKE_BYTES=2048 -DGDIR_ARRAYS=128 -DGDIR_MAX_NODES=512 \
		   -D__SWITCH__ $(GDIR_EXTRA_FLAGS)

CXXFLAGS	:= -O2 $(ARCH) -ffunction-sections -fno-exceptions -fno-rtti -fno-strict-aliasing -fno-builtin-printf -Wall \
		   -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare \
		   -Wno-format -I. -I$(LIBNX)/include $(DEFINES)
LDFLAGS		:= -specs=$(LIBNX)/switch.specs $(ARCH) -Wl,--wrap=printf -Wl,--wrap=diagAbortWithResult -Wl,-Map,gdir.map
LIBS		:= -L$(LIBNX)/lib -ldeko3d -lnx -lm

.PHONY: all clean
all: gdir.nro

gdir.elf: $(OFILES)
	@echo linking ... $@
	@$(CXX) $^ $(LDFLAGS) $(LIBS) -o $@

gdir.nro: gdir.elf
	@$(ELF2NRO) $< $@
	@echo wrote $@

%.o: %.cpp
	@echo $<
	@$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OFILES) gdir.elf gdir.nro gdir.map
