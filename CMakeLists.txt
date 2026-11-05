cmake_minimum_required(VERSION 3.20)
project(fnv_recomp C)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Generated from the player's own FalloutNV.exe by tools/recomp.py (never committed).
file(GLOB GEN_SOURCES ${CMAKE_SOURCE_DIR}/gen/funcs_*.c)
add_library(fnv_gen OBJECT ${GEN_SOURCES} gen/table.c gen/image.c)
# -O1 keeps compile time reasonable for ~370 MB of generated C; locals still get registers.
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1;-g0;-w;-fno-strict-aliasing;-fwrapv")
target_include_directories(fnv_gen PRIVATE rt)

file(GLOB RT_SOURCES ${CMAKE_SOURCE_DIR}/rt/*.c)
add_executable(fnv ${RT_SOURCES} $<TARGET_OBJECTS:fnv_gen>)
target_include_directories(fnv PRIVATE rt)
target_compile_options(fnv PRIVATE -Wall -Wno-unused-function -fno-strict-aliasing -fwrapv)
target_link_libraries(fnv PRIVATE m pthread)
target_link_options(fnv PRIVATE -rdynamic)
