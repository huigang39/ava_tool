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

set(DIR_MODULE ${PROJECT_SOURCE_DIR}/module)
set(DIR_THIRDPARTY ${PROJECT_SOURCE_DIR}/thirdparty)
set(DIR_GUI ${PROJECT_SOURCE_DIR}/gui)

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

set(SRC
        ${SRC_IMGUI}
        ${SRC_IMPLOT}
        ${DIR_THIRDPARTY}/ImGuiFileDialog/ImGuiFileDialog.cpp
        ${DIR_THIRDPARTY}/cJSON/cJSON.c
        ${DIR_GUI}/gui.cpp
        ${DIR_GUI}/monitor.cpp
        ${DIR_GUI}/parser.cpp
        ${DIR_GUI}/elf_parser.cpp
        ${DIR_GUI}/dwarf_parser.cpp
        ${DIR_GUI}/jlink_dev.cpp
)

set(MODULE_MAKE_TARGET all)

if (UNIX)
    set(MODULE_LIB_DIR ${DIR_MODULE}/lib/module/linux)
elseif (WIN32)
    set(MODULE_LIB_DIR ${DIR_MODULE}/lib/module/win)
endif ()
set(MODULE_LIB ${MODULE_LIB_DIR}/module.a)

find_program(MAKE_PROGRAM make mingw32-make)
if (NOT MAKE_PROGRAM)
    message(FATAL_ERROR "make not found. please install make (MinGW/MSYS2 on Windows)")
endif ()

add_custom_target(module_build
        COMMAND ${MAKE_PROGRAM} ${MODULE_MAKE_TARGET}
        WORKING_DIRECTORY ${DIR_MODULE}
        COMMENT "building module static library using Makefile"
        BYPRODUCTS ${MODULE_LIB}
)

add_executable(${PROJECT_NAME}
        main.cpp
        ${SRC}
)

add_dependencies(${PROJECT_NAME} module_build)

target_include_directories(${PROJECT_NAME} PRIVATE
        ${DIR_MODULE}
        ${DIR_MODULE}/inc

        ${DIR_THIRDPARTY}/imgui
        ${DIR_THIRDPARTY}/imgui/backends
        ${DIR_THIRDPARTY}/implot
        ${DIR_THIRDPARTY}/ImGuiFileDialog
        ${DIR_THIRDPARTY}/ImGuiNotify/win32/backends
        ${DIR_THIRDPARTY}/ImGuiNotify/win32/fonts
        ${DIR_THIRDPARTY}/GLFW/inc
        ${DIR_THIRDPARTY}/GLFW/inc/GLFW
        ${DIR_THIRDPARTY}/cJSON

        ${DIR_GUI}
)

target_link_directories(${PROJECT_NAME} PRIVATE
        ${MODULE_LIB_DIR}
)

if (UNIX)
    set(GLFW3_LIB ${DIR_THIRDPARTY}/GLFW/lib/linux/libglfw3.so)
    set(JLINK_LIB ${DIR_MODULE}/lib/linux/libjlinkarm.so.8)
elseif (WIN32)
    set(GLFW3_LIB ${DIR_THIRDPARTY}/GLFW/lib/win/glfw3.dll)
    set(JLINK_LIB ${DIR_MODULE}/lib/win/JLink_x64.dll)
    set(FFTW_LIB ${DIR_MODULE}/lib/win/libfftw3f-3.dll)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GLFW3_LIB}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${JLINK_LIB}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFTW_LIB}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            COMMENT "copying DLL files to output directory"
    )
endif ()

target_link_libraries(${PROJECT_NAME} PRIVATE
        OpenGL::GL
        ${GLFW3_LIB}
        ${JLINK_LIB}
        ${FFTW_LIB}
        ${MODULE_LIB}
        $<$<PLATFORM_ID:Windows>:winmm>
)

# set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
# set(CPACK_PACKAGE_VERSION ${AVATOOL_VERSION})
# set(CPACK_GENERATOR "NSIS")
# set(CPACK_PACKAGE_FILE_NAME ${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME})

# install(TARGETS ${PROJECT_NAME} DESTINATION bin)

# include(CPack)
