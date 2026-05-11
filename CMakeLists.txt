cmake_minimum_required(VERSION 3.15)

project(ava_tool
        VERSION 0.0.1
        LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(OpenGL REQUIRED)

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
        ${DIR_THIRDPARTY}/ImGuiFileDialog/ImGuiFileDialog.cpp
        ${DIR_THIRDPARTY}/cJSON/cJSON.c
)

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
        ${SRC_THIRDPARTY}
)

target_include_directories(${PROJECT_NAME} PRIVATE
        ${DIR_SRC}
        ${DIR_CORE}
        ${DIR_GUI}
        ${DIR_MODULE}/inc
        ${DIR_THIRDPARTY}/imgui
        ${DIR_THIRDPARTY}/imgui/backends
        ${DIR_THIRDPARTY}/implot
        ${DIR_THIRDPARTY}/ImGuiFileDialog
        ${DIR_THIRDPARTY}/ImGuiNotify/win32/backends
        ${DIR_THIRDPARTY}/ImGuiNotify/win32/fonts
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
    set(FFTW_LIB ${DIR_MODULE}/lib/win/libfftw3f-3.lib)
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
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 winmm)
    if(MSVC)
        # MSVC specific options
    else()
        target_link_options(${PROJECT_NAME} PRIVATE -static-libgcc -static-libstdc++)
    endif()
elseif(APPLE)
    # macOS paths (Assuming shared libs are in macos subfolder if provided)
    set(GLFW3_LIB ${DIR_THIRDPARTY}/GLFW/lib/macos/libglfw.3.dylib)
    set(JLINK_LIB ${DIR_MODULE}/lib/macos/libjlinkarm.dylib)
    set(FFTW_LIB ${DIR_MODULE}/lib/macos/libfftw3f.dylib)
    find_package(OpenGL REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE "-framework Cocoa" "-framework IOKit" "-framework CoreVideo")
else()
    # Linux paths
    set(GLFW3_LIB ${DIR_THIRDPARTY}/GLFW/lib/linux/libglfw3.so)
    set(JLINK_LIB ${DIR_MODULE}/lib/linux/libjlinkarm.so.8)
    set(FFTW_LIB fftw3f) # Use system fftw if not provided
    find_package(OpenGL REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
        module
        ${GLFW3_LIB}
        ${JLINK_LIB}
        ${FFTW_LIB}
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
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GLFW3_LIB}" "${JLINK_LIB}" "${FFTW_LIB}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            COMMENT "copying dynamic libraries to output directory"
    )
endif()

# set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
# set(CPACK_PACKAGE_VERSION ${AVATOOL_VERSION})
# set(CPACK_GENERATOR "NSIS")
# set(CPACK_PACKAGE_FILE_NAME ${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME})

# install(TARGETS ${PROJECT_NAME} DESTINATION bin)

# include(CPack)
