cmake_minimum_required(VERSION 3.28)
project(fsa_soem_master LANGUAGES C)

if(NOT WIN32)
  message(FATAL_ERROR "fsa_soem_master currently supports Windows only")
endif()

set(SOEM_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SOEM)

add_executable(fsa_soem_master src/main.c)
target_compile_features(fsa_soem_master PRIVATE c_std_11)
target_include_directories(fsa_soem_master PRIVATE ../../sdk/ava_debug/inc)
target_link_libraries(fsa_soem_master PRIVATE soem winmm)

if(MSVC)
  target_compile_options(fsa_soem_master PRIVATE /W4 /utf-8)
else()
  target_compile_options(fsa_soem_master PRIVATE -Wall -Wextra -Wpedantic)
endif()

