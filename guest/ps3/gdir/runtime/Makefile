# Blazium PS3 GDIR guest: the PS2 guest's scene runtime (GDIR VM, sys_io,
# pack_io) over the PS3's RSX, pads, audio port and hard drive, built with
# PSL1GHT (ps3dev).
#
# blazium-toolchain ships this guest compiled (guest/ps3/prebuilt/gdir.elf),
# so ps3 build needs no SDK; it only runs this Makefile for a guest with code
# of your own (--overlay, GDIR_EXTRA_FLAGS). The toolchain signs the result
# as EBOOT.BIN and writes PARAM.SFO and the .pkg itself.
#
# make   gdir.elf, and gdir.prx.elf: stripped, its PRX imports linked (what
#        the toolchain signs; scripts/ps3-guest-prebuilt.sh makes the
#        bundled one from it)

ifeq ($(strip $(PSL1GHT)),)
$(error PSL1GHT is not set (the ps3dev folder))
endif

include $(PSL1GHT)/ppu_rules

APPID		?= BLAZ00001

SOURCES		:= $(wildcard *.cpp) $(wildcard extra/*.cpp)
OFILES		:= $(SOURCES:.cpp=.o)

# the GDIR VM's tables at the PS2's sizes (and the PS2's pack budgets, which
# the scripts can read); "ps3" for OS.has_feature
DEFINES		:= -DBLAZIUM_PS3_GDIR=1 '-DGDIR_FEATURE="ps3"' -DBLAZIUM_PS2_COOK_ABI=1 \
		   -DPS2_POKE_N=2048u -DGDIR_POKE_BYTES=2048 -DGDIR_ARRAYS=128 -DGDIR_MAX_NODES=512 \
		   '-DPS3_TITLE_ID="$(APPID)"' $(GDIR_EXTRA_FLAGS)

CXXFLAGS	:= -O2 -mcpu=cell $(MACHDEP) -fno-exceptions -fno-rtti -fno-strict-aliasing -Wall -Wno-unused-function \
		   -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare -I. $(LIBPSL1GHT_INC) \
		   -I$(PORTLIBS)/include $(DEFINES)
LDFLAGS		:= $(MACHDEP) -Wl,-Map,gdir.map
LIBS		:= -lrsx -lgcm_sys -lio -lsysutil -laudio -lrt -llv2 -lm
LIBPATHS	:= $(LIBPSL1GHT_LIB) -L$(PORTLIBS)/lib

.PHONY: all clean
all: gdir.prx.elf

gdir.elf: $(OFILES)
	$(VERB) echo linking ... $@
	$(VERB) $(CXX) $^ $(LDFLAGS) $(LIBPATHS) $(LIBS) -o $@

gdir.prx.elf: gdir.elf
	$(VERB) $(STRIP) $< -o $@
	$(VERB) $(SPRX) $@
	$(VERB) echo wrote $@

clean:
	rm -f $(OFILES) gdir.elf gdir.prx.elf gdir.map
