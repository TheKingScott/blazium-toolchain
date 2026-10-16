# Blazium PS2 guest — ps2sdk EE Makefile (AFL libs only: draw/graph/packet/dma).
# Spawned by blazium-toolchain ps2 build. Do not link gsKit or audsrv.
# VU1 is enabled here when dvp-as + libpacket2 exist. CMake is CPU-GIF fallback.

EE_BIN = runtime.elf
EE_OBJS = main.o gs_draw.o pad_io.o sfx_io.o pack_io.o script_vm.o sys_io.o vu1_draw.o gdir_vm.o
EE_OBJS += $(patsubst %.cpp,%.o,$(wildcard extra/*.cpp))
EE_OBJS += $(patsubst %.c,%.o,$(wildcard extra/*.c))
EE_INCS += -I.
EE_LIBS = -ldraw -lgraph -lpacket -ldma -lpad -lfileXio -lpatches -lm
ifneq ($(wildcard $(PS2SDK)/ee/lib/libsdr.a),)
EE_LIBS += -lsdr
EE_CXXFLAGS += -DBLAZIUM_PS2_HAS_LIBSD=1
endif
EE_DVP ?= dvp-as
HAS_DVP := $(wildcard $(PS2DEV)/dvp/bin/dvp-as)$(wildcard $(PS2DEV)/dvp/bin/dvp-as.exe)
ifneq ($(wildcard $(PS2SDK)/ee/lib/libpacket2.a),)
ifneq ($(HAS_DVP),)
EE_OBJS += draw_3D.o vu1_scene.o
EE_LIBS += -lpacket2 -lmath3d
EE_CXXFLAGS += -DBLAZIUM_PS2_HAS_VU1=1
endif
endif
EE_CXXFLAGS += -fno-exceptions -fno-rtti -DBLAZIUM_PS2_COOK_ABI=1 -DBUILD_FOR_PCSX2=1

# Variables only (no rules). Must come before eeglobal so $(EE_BIN): $(EE_OBJS)
# lists cook_embed.o. Do not add a cook_embed.o recipe here — that would become
# the default goal. eeglobal already has %.o: %.S.
-include cook.mk
-include irx.mk

ifeq ($(wildcard $(PS2SDK)/samples/Makefile.pref),)
include $(PS2SDK)/samples/Makefile.pref_sample
include $(PS2SDK)/samples/Makefile.eeglobal_sample
else
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
endif

# Windows cmd mkdir is not GNU mkdir -p; objects live next to sources.
DIR_GUARD =

%.o: %.vsm
	$(EE_DVP) $< -o $@
