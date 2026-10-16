# Guest-only ps2sdk target. Host compilers are spawned by blazium-toolchain
# (ps2 build). Prefer the Makefile when PS2SDK samples are present.
# VU1: on when dvp-as + libpacket2 exist. Otherwise CPU GIF fallback (same as Makefile).

cmake_minimum_required(VERSION 3.10)

project(blazium_ps2_runtime LANGUAGES CXX)

add_executable(runtime main.cpp gs_draw.cpp pad_io.cpp sfx_io.cpp pack_io.cpp script_vm.cpp sys_io.cpp vu1_draw.cpp gdir_vm.cpp)
file(GLOB _ps2_extra RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
	"extra/*.cpp" "extra/*.c")
if(_ps2_extra)
	target_sources(runtime PRIVATE ${_ps2_extra})
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cook_embed.S")
	enable_language(ASM)
	target_sources(runtime PRIVATE cook_embed.S)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/irx_embed.S")
	enable_language(ASM)
	target_sources(runtime PRIVATE irx_embed.S)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cook_flags.h" OR EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/irx_flags.h")
	target_include_directories(runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
target_include_directories(runtime PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"$ENV{PS2SDK}/ee/include"
	"$ENV{PS2SDK}/common/include")
target_compile_options(runtime PRIVATE -D_EE -G0 -fno-exceptions -fno-rtti)
target_compile_definitions(runtime PRIVATE BLAZIUM_PS2_COOK_ABI=1 BUILD_FOR_PCSX2=1)
target_link_libraries(runtime PRIVATE draw graph packet dma pad fileXio patches m)
if(EXISTS "$ENV{PS2SDK}/ee/lib/libsdr.a")
	target_link_libraries(runtime PRIVATE sdr)
	target_compile_definitions(runtime PRIVATE BLAZIUM_PS2_HAS_LIBSD=1)
endif()
find_program(BLAZIUM_DVP_AS NAMES dvp-as dvp-as.exe PATHS "$ENV{PS2DEV}/dvp/bin")
if(EXISTS "$ENV{PS2SDK}/ee/lib/libpacket2.a" AND BLAZIUM_DVP_AS)
	add_custom_command(
		OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/draw_3D.o"
		COMMAND "${BLAZIUM_DVP_AS}" "${CMAKE_CURRENT_SOURCE_DIR}/draw_3D.vsm" -o "${CMAKE_CURRENT_BINARY_DIR}/draw_3D.o"
		DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/draw_3D.vsm"
		VERBATIM)
	target_sources(runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/draw_3D.o")
	target_compile_definitions(runtime PRIVATE BLAZIUM_PS2_HAS_VU1=1)
	target_link_libraries(runtime PRIVATE packet2 math3d)
	message(STATUS "VU1 on (dvp-as + libpacket2)")
else()
	message(STATUS "VU1 off: CPU GIF fallback (need dvp-as + libpacket2; Makefile path also works)")
endif()
set_target_properties(runtime PROPERTIES OUTPUT_NAME "runtime" SUFFIX ".elf")
