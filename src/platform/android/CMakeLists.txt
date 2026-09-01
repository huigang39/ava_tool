cmake_minimum_required(VERSION 3.22)
project(ava_android LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
add_library(ava_core SHARED
    native/bridge.cpp
    native/peer_bridge.cpp
    ../../core/remote_device.cpp
    ../../core/elf_parser.cpp
    ../../core/dwarf_parser.cpp
    ../../../thirdparty/cJSON/cJSON.c)
target_include_directories(ava_core PRIVATE native/compat ../.. ../../../thirdparty/cJSON)
if(MSVC)
    target_link_libraries(ava_core PRIVATE ws2_32 bcrypt)
    target_compile_options(ava_core PRIVATE /W3 /utf-8 /EHsc)
    # Match desktop compatibility with hosts/JVMs bundling an older VC runtime.
    target_compile_definitions(ava_core PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
else()
    target_compile_options(ava_core PRIVATE -Wall -Wextra)
endif()
# Windows short paths can turn clang++.exe into CLANG_~1.EXE; retain C++ driver mode.
if(ANDROID)
    target_link_options(ava_core PRIVATE "--driver-mode=g++" "-Wl,-z,max-page-size=16384")
else()
    # Host JNI tests execute the same shared ELF/DWARF parser without an Android device.
    find_package(JNI REQUIRED)
    target_include_directories(ava_core PRIVATE ${JNI_INCLUDE_DIRS})
endif()
