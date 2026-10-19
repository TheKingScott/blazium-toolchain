# Blazium N64 GDIR guest: the PS2 guest's scene runtime (GDIR VM, sys_io,
# pack_io) over Tiny3D, the N64's pads, AI and EEPROM. Built with libdragon
# (preview) by blazium-toolchain (n64 build --sample gdir), which stages the
# export's cooked files, converted to the PS1 forms, in filesystem/DATA/.

BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(T3D_INST)/t3d.mk

src = main.cpp t3d_draw.cpp pad_io.cpp sfx_io.cpp mc_io.cpp gdir_vm.cpp sys_io.cpp pack_io.cpp

# the shared runtime is warning-clean on its own compilers, not every GCC
N64_CXXFLAGS += -O2 -fno-exceptions -fno-rtti -Wno-error
# the GDIR VM's tables at the PS2's sizes (more arrays: four-player races
# keep more alive), the PS2 budgets as the N64's RAM
N64_CXXFLAGS += -DBLAZIUM_N64_GDIR=1 -DPACK_IO_RELOAD_PACK0=1 '-DGDIR_FEATURE="n64"' -DPACK_IO_HEAP_REPORT=n64_heap_report \
	-DPS2_POKE_N=2048u -DGDIR_POKE_BYTES=2048 -DBLAZIUM_PS2_COOK_ABI=1 -DGDIR_ARRAYS=128 \
	-DPS2_EE_LIMIT=5242880u -DPS2_GS_LIMIT=4194304u -DPS2_EXTRA_EE_CAP=2097152u $(GDIR_EXTRA_FLAGS)

N64_ROM_TITLE = "Blazium GDIR"
N64_ROM_SAVETYPE = eeprom4k
N64_MKDFS_ROOT = filesystem

all: runtime.z64

$(BUILD_DIR)/runtime.dfs: $(wildcard filesystem/DATA/*)
$(BUILD_DIR)/runtime.elf: $(src:%.cpp=$(BUILD_DIR)/%.o)
runtime.z64: $(BUILD_DIR)/runtime.dfs

clean:
	rm -rf $(BUILD_DIR) runtime.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
