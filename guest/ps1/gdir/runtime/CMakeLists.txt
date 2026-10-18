# Blazium PS1 GDIR guest: the PS2 guest's scene runtime (GDIR VM, sys_io,
# pack_io) over the PS1 GPU, GTE, pads and CD. Built with PSn00bSDK by
# blazium-toolchain (ps1 build --sample gdir).

cmake_minimum_required(VERSION 3.21)

project(
	blazium_ps1_gdir
	LANGUAGES C CXX ASM
	VERSION 0.1.0
	DESCRIPTION "Blazium PS1 GDIR guest"
)

psn00bsdk_add_executable(runtime GPREL
	main.cpp gpu_draw.cpp pad_io.cpp sfx_io.cpp mc_io.cpp ps1_libc.c
	gdir_vm.cpp sys_io.cpp pack_io.cpp)
# shim/ first: stdio.h (FILE over the CD), math.h and stdlib.h additions
target_include_directories(runtime BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shim")
target_include_directories(runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(runtime PRIVATE -O2 $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
# the GDIR VM's tables sized for 2 MiB, and the PS2 budgets as the PS1's
target_compile_definitions(runtime PRIVATE
	BLAZIUM_PS1_GDIR=1
	GDIR_FEATURE="ps1"
	BLAZIUM_PS2_COOK_ABI=1
	GDIR_MAX_NODES=256
	GDIR_MAX_CONSTS=4096
	GDIR_MAX_KARR=256
	GDIR_MAX_FUNCS=512
	GDIR_MAX_MEMBERS=512
	GDIR_STACK=512
	GDIR_RING=64
	GDIR_ARRAYS=32
	GDIR_MAX_PACKS=32
	GDIR_MAX_INST=64
	GDIR_OPS_PER_FRAME=200000
	PS2_EE_LIMIT=1572864u
	PS2_GS_LIMIT=786432u
	PS2_EXTRA_EE_CAP=1048576u)
