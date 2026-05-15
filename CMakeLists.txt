cmake_minimum_required(VERSION 3.15)

project(ava_tool
        VERSION 0.0.1
        LANGUAGES CXX C OBJCXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- Platform & Architecture Detection ---
if(WIN32)
    set(OS_DIR "win")
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    set(OS_DIR "macos")
else()
    set(OS_DIR "linux")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
        set(ARCH_DIR "arm64")
    else()
        set(ARCH_DIR "x64")
    endif()
else()
    set(ARCH_DIR "x86")
endif()

message(STATUS "Build Platform: ${OS_DIR} Arch: ${ARCH_DIR}")

# --- Compiler Specific Flags ---
if(MSVC)
    add_compile_options(/W3 /utf-8 /MP /experimental:c11atomics)
    add_definitions(-DNOMINMAX -D_USE_MATH_DEFINES)
else()
    add_compile_options(-Wall -Wextra -O3)
    if(APPLE)
        add_compile_options(-fobjc-arc)
        add_definitions(-DGL_SILENCE_DEPRECATION)
    endif()
endif()

# --- Directories ---
set(DIR_SRC ${PROJECT_SOURCE_DIR}/src)
set(DIR_CORE ${DIR_SRC}/core)
set(DIR_GUI ${DIR_SRC}/gui)
set(DIR_THIRDPARTY ${PROJECT_SOURCE_DIR}/thirdparty)
set(DIR_MODULE ${PROJECT_SOURCE_DIR}/module)

# --- Module Library (Native CMake) ---
file(GLOB SRC_MODULE "${DIR_MODULE}/src/*.c")
add_library(module STATIC ${SRC_MODULE})
if(MSVC)
    set_source_files_properties(${SRC_MODULE} PROPERTIES LANGUAGE CXX)
endif()
target_include_directories(module PUBLIC ${DIR_MODULE} ${DIR_MODULE}/inc)
if(WIN32)
    target_link_libraries(module PRIVATE ws2_32)
endif()

# --- Thirdparty Sources ---
set(SRC_IMGUI
        ${DIR_THIRDPARTY}/imgui/imgui.cpp
        ${DIR_THIRDPARTY}/imgui/imgui_demo.cpp
        ${DIR_THIRDPARTY}/imgui/imgui_draw.cpp
        ${DIR_THIRDPARTY}/imgui/imgui_tables.cpp
        ${DIR_THIRDPARTY}/imgui/imgui_widgets.cpp
        ${DIR_THIRDPARTY}/imgui/backends/imgui_impl_glfw.cpp
        ${DIR_THIRDPARTY}/imgui/backends/imgui_impl_opengl3.cpp
)

set(SRC_IMPLOT
        ${DIR_THIRDPARTY}/implot/implot.cpp
        ${DIR_THIRDPARTY}/implot/implot_demo.cpp
        ${DIR_THIRDPARTY}/implot/implot_items.cpp
)

set(SRC_THIRDPARTY
        ${SRC_IMGUI}
        ${SRC_IMPLOT}
        ${DIR_THIRDPARTY}/cJSON/cJSON.c
)

# Native file dialog — one platform-specific translation unit
if(APPLE)
    set(SRC_NATIVE_DLG ${DIR_SRC}/native_dlg_mac.mm)
elseif(WIN32)
    set(SRC_NATIVE_DLG ${DIR_SRC}/native_dlg_win.cpp)
else()
    set(SRC_NATIVE_DLG ${DIR_SRC}/native_dlg_linux.cpp)
endif()

# --- Main Executable ---
add_executable(${PROJECT_NAME} WIN32
        ${DIR_SRC}/main.cpp
        ${DIR_GUI}/gui.cpp
        ${DIR_GUI}/monitor.cpp
        ${DIR_GUI}/variable.cpp
        ${DIR_CORE}/elf_parser.cpp
        ${DIR_CORE}/dwarf_parser.cpp
        ${DIR_CORE}/json_parser.cpp
        ${DIR_CORE}/bin_parser.cpp
        ${DIR_CORE}/jlink_dev.cpp
        ${SRC_NATIVE_DLG}
        ${SRC_THIRDPARTY}
)

if(WIN32)
    set(IMGUI_NOTIFY_DIR ${DIR_THIRDPARTY}/ImGuiNotify/win32)
else()
    set(IMGUI_NOTIFY_DIR ${DIR_THIRDPARTY}/ImGuiNotify/unix)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
        ${DIR_SRC}
        ${DIR_CORE}
        ${DIR_GUI}
        ${DIR_MODULE}/inc
        ${DIR_THIRDPARTY}/imgui
        ${DIR_THIRDPARTY}/imgui/backends
        ${DIR_THIRDPARTY}/implot
        ${IMGUI_NOTIFY_DIR}/backends
        ${IMGUI_NOTIFY_DIR}/fonts
        ${DIR_THIRDPARTY}/GLFW/inc
        ${DIR_THIRDPARTY}/cJSON
)

# --- External Libraries Mapping ---
if(WIN32)
    set(GLFW3_LIB ${DIR_THIRDPARTY}/GLFW/lib/win/glfw3.lib)
    if(ARCH_DIR STREQUAL "arm64")
        set(JLINK_LIB ${DIR_MODULE}/lib/win/JLink_arm64.lib)
    else()
        set(JLINK_LIB ${DIR_MODULE}/lib/win/JLink_x64.lib)
    endif()
    set(FFTW3F_LIB ${DIR_MODULE}/lib/win/libfftw3f-3.lib)
    if(ARCH_DIR STREQUAL "arm64")
        set(DLL_LIST
            ${DIR_THIRDPARTY}/GLFW/lib/win/glfw3.dll
            ${DIR_MODULE}/lib/win/JLink_arm64.dll
            ${DIR_MODULE}/lib/win/libfftw3f-3.dll
        )
    else()
        set(DLL_LIST
            ${DIR_THIRDPARTY}/GLFW/lib/win/glfw3.dll
            ${DIR_MODULE}/lib/win/JLink_x64.dll
            ${DIR_MODULE}/lib/win/libfftw3f-3.dll
        )
    endif()
    find_package(OpenGL REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 winmm shlwapi)
    if(MSVC)
        # MSVC specific options
    else()
        target_link_options(${PROJECT_NAME} PRIVATE -static-libgcc -static-libstdc++)
    endif()
elseif(APPLE)
    # macOS: 使用 Homebrew 安装的 GLFW / FFTW, 以及 SEGGER 安装的 JLink dylib
    find_package(glfw3 REQUIRED)
    set(GLFW3_LIB glfw)

    find_path(FFTW3F_INCLUDE_DIR fftw3.h
        HINTS /opt/homebrew/include /usr/local/include)
    find_library(FFTW3F_LIB NAMES fftw3f
        HINTS /opt/homebrew/lib /usr/local/lib REQUIRED)

    find_library(JLINK_LIB NAMES jlinkarm
        HINTS /Applications/SEGGER/JLink REQUIRED)

    target_include_directories(${PROJECT_NAME} PRIVATE ${FFTW3F_INCLUDE_DIR})
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "-framework Cocoa" "-framework IOKit" "-framework CoreVideo" "-framework OpenGL")
else()
    # Linux paths
    set(GLFW3_LIB ${DIR_THIRDPARTY}/GLFW/lib/linux/libglfw3.so)
    set(JLINK_LIB ${DIR_MODULE}/lib/linux/libjlinkarm.so.8)
    set(FFTW3F_LIB fftw3f) # Use system fftw if not provided
    find_package(OpenGL REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
        module
        ${GLFW3_LIB}
        ${JLINK_LIB}
        ${FFTW3F_LIB}
        $<$<NOT:$<PLATFORM_ID:Darwin>>:OpenGL::GL>
)

# --- Post-Build: Copy Shared Libraries ---
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${DLL_LIST}
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            COMMENT "copying dynamic libraries to output directory"
    )
elseif(APPLE)
    # macOS: 让可执行文件能够找到 SEGGER JLink dylib (brew 装的 glfw/fftw 已在标准 rpath 下)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_RPATH   "/Applications/SEGGER/JLink"
        INSTALL_RPATH "/Applications/SEGGER/JLink"
    )
    # SEGGER 的 dylib install_name 为相对路径 (Output/Release/<arch>/...), dyld 无法解析,
    # 这里改写为已存在 rpath 中的绝对路径名,确保运行时可加载。
    if(ARCH_DIR STREQUAL "arm64")
        set(JLINK_INSTALL_NAME "Output/Release/arm64/libjlinkarm.9.42.0.dylib")
    else()
        set(JLINK_INSTALL_NAME "Output/Release/x64/libjlinkarm.9.42.0.dylib")
    endif()
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND install_name_tool -change "${JLINK_INSTALL_NAME}"
                "/Applications/SEGGER/JLink/libjlinkarm.dylib"
                "$<TARGET_FILE:${PROJECT_NAME}>"
        COMMENT "Rewriting JLink dylib install name to absolute path")
endif()

# set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
# set(CPACK_PACKAGE_VERSION ${AVATOOL_VERSION})
# set(CPACK_GENERATOR "NSIS")
# set(CPACK_PACKAGE_FILE_NAME ${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME})

# install(TARGETS ${PROJECT_NAME} DESTINATION bin)

# include(CPack)
