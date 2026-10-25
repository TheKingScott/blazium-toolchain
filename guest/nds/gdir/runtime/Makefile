# Blazium DS GDIR guest: the PS2 guest's scene runtime (GDIR VM, sys_io,
# pack_io) over the DS's 3D engine, keys, sound channels and SD card. Built
# with BlocksDS by blazium-toolchain (nds build --sample gdir), which stages
# the export's cooked files, converted to the PS1 forms, in nitrofs/DATA/.
# The ROM is a DS-mode .nds: a DS, a DS Lite and a DSi (or 3DS) run it.

NAME		:= gdir
GAME_TITLE	?= Blazium GDIR
GAME_SUBTITLE	?= Made with Blazium
GAME_AUTHOR	?= blazium-toolchain

SOURCEDIRS	:= .
NITROFSDIR	:= nitrofs

LIBS		:= -lnds9
LIBDIRS		:= $(BLOCKSDS)/libs/libnds

# DSI=1 (blazium-toolchain dsi build): the DSi-only ROM (dsi build marks
# its header DSi-exclusive after make). The DSi's 16 MiB
# as the PS2 budgets, pack 0 stays in RAM; libnds runs the ARM9 at 133 MHz
# in DSi mode. Otherwise the DS's 4 MiB (pack 0 leaves RAM while another
# pack is the scene, as on the PS1); a DSi runs that ROM in DSi mode too.
ifeq ($(DSI),1)
TARGET_DEFINES	:= -DBLAZIUM_DSI=1 -DPS2_EE_LIMIT=12582912u -DPS2_GS_LIMIT=524288u -DPS2_EXTRA_EE_CAP=4194304u
else
TARGET_DEFINES	:= -DPACK_IO_RELOAD_PACK0=1 -DPS2_EE_LIMIT=2621440u -DPS2_GS_LIMIT=524288u -DPS2_EXTRA_EE_CAP=1048576u
endif

# the GDIR VM's tables at the PS2's sizes
DEFINES		:= -DBLAZIUM_NDS_GDIR=1 '-DGDIR_FEATURE="nds"' -DPACK_IO_HEAP_REPORT=nds_heap_report \
		   -DPS2_POKE_N=2048u -DGDIR_POKE_BYTES=2048 -DBLAZIUM_PS2_COOK_ABI=1 -DGDIR_ARRAYS=128 \
		   -DGDIR_MAX_NODES=512 $(TARGET_DEFINES) $(GDIR_EXTRA_FLAGS)

# the shared runtime is warning-clean on its own compilers, not every GCC
CXXFLAGS	+= -Wno-error -Wno-unused-function -Wno-unused-variable

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile


# the VM's loop in ARM code: faster than Thumb on the ARM9 where it matters
build/gdir/./gdir_vm.cpp.o: CXXFLAGS += -marm
