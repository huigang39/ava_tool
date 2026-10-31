# Do not reuse a partial object or executable after a failed recipe.
.DELETE_ON_ERROR:

ifeq ($(OS),Windows_NT)
SHELL := cmd.exe
ifndef VCVARS_DONE

#Targets that only need Python / file ops — not the MSVC compiler / linker.
#When every requested goal is one of these, skip the(slow) vcvars init below.
#NOTE : `package` is NOT here — it depends on `all`, so it must initialize MSVC
#(otherwise a build after `make clean` can't find cl.exe).
_NO_MSVC_GOALS := icon sign clean fmt info help
_NEED_MSVC     := $(if $(MAKECMDGOALS),$(filter-out $(_NO_MSVC_GOALS),$(MAKECMDGOALS)),all)

ifneq ($(_NEED_MSVC),)

_CL_OK := $(shell where cl.exe 2>NUL)

ifeq ($(_CL_OK),)

_PFILES86     := $(strip $(shell powershell -NoProfile -Command "[Environment]::GetFolderPath('ProgramFilesX86')" 2>NUL))
_VSWHERE      := $(_PFILES86)\Microsoft Visual Studio\Installer\vswhere.exe
_VS           := $(strip $(shell "$(_VSWHERE)" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>NUL))
_VCVARS       := $(_VS)\VC\Auxiliary\Build\vcvarsall.bat
_GOALS        := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)
_MSVC_REINVOKE := 1

.PHONY: _msvc_env $(_GOALS)

_msvc_env:
	@echo [MSVC] Initializing Visual Studio environment...
	@call "$(_VCVARS)" x64 >NUL 2>&1 && $(MAKE) $(_GOALS) VCVARS_DONE=1 --no-print-directory

$(_GOALS): _msvc_env
	@:

.DEFAULT_GOAL := all

endif # _CL_OK
endif # _NEED_MSVC
endif # VCVARS_DONE
endif # Windows_NT (MSVC init)

ifndef _MSVC_REINVOKE

PROJECT_NAME   := ava_tool
PROJECT_DIR    := $(CURDIR)

DIR_SRC        := src
DIR_GUI        := src/gui
DIR_CORE       := src/core
DIR_PLATFORM   := src/platform
DIR_THIRDPARTY := thirdparty
DIR_CKIT     := ckit

BUILD_DIR      := build
BIN_DIR        := bin

FMT_DIRS       := src
FMT_PYTHON     ?= $(if $(filter win,$(PLATFORM)),python,python3)
CLANG_FORMAT_EXE ?= clang-format

# ─── Platform detection ────────────────────────────────────────────────────────

ifeq ($(OS),Windows_NT)
    PLATFORM   := win
    ARCH       ?= $(if $(filter AMD64,$(PROCESSOR_ARCHITECTURE)),x86_64,$(if $(filter ARM64,$(PROCESSOR_ARCHITECTURE)),arm64,x86_64))
    COMPILER   ?= msvc
    SHELL      := cmd.exe
    ECHO_NL    := echo.
    EXE_EXT    := .exe
define MKDIR
	@if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))" 2>nul
endef
define RMDIR
	@if exist "$(subst /,\,$(1))" rd /s /q "$(subst /,\,$(1))" 2>nul
endef
else
    UNAME_S    := $(shell uname -s)
    UNAME_M    := $(shell uname -m)
    ifeq ($(UNAME_S),Darwin)
        PLATFORM ?= mac
        COMPILER ?= clang
    else
        PLATFORM ?= linux
        COMPILER ?= gcc
    endif
    ifeq ($(UNAME_M),x86_64)
        ARCH   ?= x86_64
    else ifeq ($(UNAME_M),arm64)
        ARCH   ?= arm64
    else ifeq ($(UNAME_M),aarch64)
        ARCH   ?= arm64
    else
        ARCH   ?= $(UNAME_M)
    endif
    SHELL      := /bin/bash
    ECHO_NL    := echo
    EXE_EXT    :=
define MKDIR
	@mkdir -p $(1)
endef
define RMDIR
	@rm -rf $(1)
endef
endif

ifeq ($(ARCH),arm64)
    MAC_BREW_PREFIX ?= /opt/homebrew
else
    MAC_BREW_PREFIX ?= /usr/local
endif

# ─── Compiler settings ────────────────────────────────────────────────────────

ifeq ($(COMPILER),msvc)
    ifeq ($(PLATFORM),win)
        CXX          := cl.exe
        CC           := cl.exe
        LD           := link.exe
        CXXFLAGS     := /c /W3 /utf-8 /FS /experimental:c11atomics /std:c++20 \
                        /DNOMINMAX /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS \
                        /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR \
                        /EHsc /MD /nologo /Z7 /O2
        CFLAGS_C     := /c /W3 /utf-8 /FS /D_CRT_SECURE_NO_WARNINGS /MD /nologo /Z7 /O2
        LDFLAGS      := /SUBSYSTEM:WINDOWS /nologo /DEBUG /NODEFAULTLIB:LIBCMT
        JLINK_DIR    ?= $(DIR_CKIT)/lib/jlink
        ifeq ($(ARCH),arm64)
            JLINK_LIB := $(JLINK_DIR)/JLink_arm64.lib
            JLINK_DLL := $(JLINK_DIR)/JLink_arm64.dll
        else
            JLINK_LIB := $(JLINK_DIR)/JLink_x64.lib
            JLINK_DLL := $(JLINK_DIR)/JLink_x64.dll
        endif
        GLFW_LIB     := $(DIR_THIRDPARTY)/GLFW/lib/win/glfw3.lib
        GLFW_DLL     := $(DIR_THIRDPARTY)/GLFW/lib/win/glfw3.dll
        FFTW_LIB     := $(DIR_CKIT)/lib/fftw/libfftw3f-3.lib
        FFTW_DLL     := $(DIR_CKIT)/lib/fftw/libfftw3f-3.dll
        CKIT_LIB   := $(DIR_CKIT)/lib/ckit/win/ckit.lib
#libffi : obtain via vcpkg install libffi : x64 - windows - static
#then copy the include / and lib / x64 - windows - static / output here.
        LIBFFI_DIR   := $(DIR_THIRDPARTY)/libffi
        LIBFFI_LIB   := $(LIBFFI_DIR)/lib/win/ffi.lib
        IMGUI_NOTIFY := $(DIR_THIRDPARTY)/ImGuiNotify/win32
        INCLUDES     := /I"$(DIR_SRC)" /I"$(DIR_CORE)" /I"$(DIR_GUI)" \
                        /I"$(DIR_CKIT)" /I"$(DIR_CKIT)/include" \
                        /I"$(DIR_THIRDPARTY)/imgui" \
                        /I"$(DIR_THIRDPARTY)/imgui/backends" \
                        /I"$(DIR_THIRDPARTY)/implot" \
                        /I"$(IMGUI_NOTIFY)/backends" \
                        /I"$(IMGUI_NOTIFY)/fonts" \
                        /I"$(DIR_THIRDPARTY)/GLFW/inc" \
                        /I"$(DIR_THIRDPARTY)/cJSON" \
                        /I"$(LIBFFI_DIR)/include"
        LDLIBS       := bcrypt.lib ws2_32.lib iphlpapi.lib winmm.lib shlwapi.lib comdlg32.lib OpenGL32.lib advapi32.lib ole32.lib dbghelp.lib \
                        mfplat.lib mfreadwrite.lib mfuuid.lib \
                        "$(GLFW_LIB)" "$(FFTW_LIB)" "$(CKIT_LIB)" "$(LIBFFI_LIB)"
        PLATFORM_DIR := win
    else
        $(error msvc 仅支持 win 平台)
    endif

else ifeq ($(COMPILER),gcc)
    ifeq ($(PLATFORM),linux)
        CXX          := g++
        CC           := gcc
        CXXFLAGS     := -c -Wall -Wextra -O3 -std=c++20
        CFLAGS_C     := -c -Wall -Wextra -O3 -std=c11
        GLFW_LIB     := $(DIR_THIRDPARTY)/GLFW/lib/linux/libglfw3.so
        JLINK_DIR    ?= $(DIR_CKIT)/lib/jlink
        JLINK_LIB    := $(firstword $(wildcard $(JLINK_DIR)/libjlinkarm.so $(JLINK_DIR)/libjlinkarm.so.*))
        CKIT_LIB   := $(DIR_CKIT)/lib/ckit/linux/ckit.a
        IMGUI_NOTIFY := $(DIR_THIRDPARTY)/ImGuiNotify/unix
        INCLUDES     := -I$(DIR_SRC) -I$(DIR_CORE) -I$(DIR_GUI) \
                        -I$(DIR_CKIT) -I$(DIR_CKIT)/include \
                        -I$(DIR_THIRDPARTY)/imgui \
                        -I$(DIR_THIRDPARTY)/imgui/backends \
                        -I$(DIR_THIRDPARTY)/implot \
                        -I$(IMGUI_NOTIFY)/backends \
                        -I$(IMGUI_NOTIFY)/fonts \
                        -I$(DIR_THIRDPARTY)/GLFW/inc \
                        -I$(DIR_THIRDPARTY)/cJSON
        LDFLAGS      := -Wl,-rpath,'$$ORIGIN'
#libffi : sudo apt install libffi - dev
        LDLIBS       := "$(GLFW_LIB)" "$(JLINK_LIB)" -lfftw3f \
                        "$(CKIT_LIB)" -lGL -ldl -lpthread -lffi
        PLATFORM_DIR := linux
    else
        $(error gcc 仅支持 linux 平台)
    endif

else ifeq ($(COMPILER),clang)
    ifeq ($(PLATFORM),mac)
        CXX          := clang++
        CC           := clang
        OBJCXX       := clang++
        CXXFLAGS     := -c -Wall -Wextra -O3 -std=c++20 \
                        -DGL_SILENCE_DEPRECATION \
                        -I$(MAC_BREW_PREFIX)/include
        CFLAGS_C     := -c -Wall -Wextra -O3 -std=c11 \
                        -I$(MAC_BREW_PREFIX)/include
        OBJCXXFLAGS  := -c -Wall -Wextra -O3 -std=c++20 -fobjc-arc \
                        -DGL_SILENCE_DEPRECATION \
                        -I$(MAC_BREW_PREFIX)/include
        JLINK_DIR    ?= $(DIR_CKIT)/lib/jlink
        JLINK_LIB    := $(JLINK_DIR)/libjlinkarm.dylib
        CKIT_LIB   := $(DIR_CKIT)/lib/ckit/mac/$(ARCH)/ckit.a
        IMGUI_NOTIFY := $(DIR_THIRDPARTY)/ImGuiNotify/unix
        INCLUDES     := -I$(DIR_SRC) -I$(DIR_CORE) -I$(DIR_GUI) \
                        -I$(DIR_CKIT) -I$(DIR_CKIT)/include \
                        -I$(DIR_THIRDPARTY)/imgui \
                        -I$(DIR_THIRDPARTY)/imgui/backends \
                        -I$(DIR_THIRDPARTY)/implot \
                        -I$(IMGUI_NOTIFY)/backends \
                        -I$(IMGUI_NOTIFY)/fonts \
                        -I$(DIR_THIRDPARTY)/GLFW/inc \
                        -I$(DIR_THIRDPARTY)/cJSON \
                        -I$(MAC_BREW_PREFIX)/include
        LDFLAGS      := -L$(MAC_BREW_PREFIX)/lib \
                        -L$(JLINK_DIR) \
                        -Wl,-rpath,$(JLINK_DIR)
#libffi : brew install libffi
        LDLIBS       := -lglfw -ljlinkarm -lfftw3f \
                        "$(CKIT_LIB)" -lpthread -lffi \
                        -framework Cocoa -framework IOKit \
                        -framework CoreVideo -framework OpenGL
        PLATFORM_DIR := mac/$(ARCH)
    else
        $(error clang 仅支持 mac 平台)
    endif

else
    $(error 无效的编译器: $(COMPILER), 可选 msvc / gcc / clang)
endif

# ─── Output paths ─────────────────────────────────────────────────────────────

# MuJoCo rendering is optional; the DLL is loaded only when its dock is used.
MUJOCO_DIR ?= C:/mujoco
ifeq ($(COMPILER),msvc)
    ifneq ($(wildcard $(MUJOCO_DIR)/include/mujoco/mujoco.h),)
        INCLUDES += /I"$(MUJOCO_DIR)/include" /DAVA_HAS_MUJOCO
    endif
endif

OUTPUT_BUILD_DIR := $(BUILD_DIR)/$(COMPILER)-$(PLATFORM)-$(ARCH)
OUTPUT_BIN_DIR   := $(BIN_DIR)/$(PLATFORM_DIR)
OUTPUT_EXE       := $(OUTPUT_BIN_DIR)/$(PROJECT_NAME)$(EXE_EXT)

#Standalone auto - update helper(Windows / MSVC only).
ifeq ($(COMPILER),msvc)
    UPDATER_EXE := $(OUTPUT_BIN_DIR)/updater$(EXE_EXT)
#Embed the app icon when assets / icon.ico                       exists(generate it with `make icon`).
#Guarded by wildcard so the build still works before the icon is created.
    ifneq ($(wildcard assets/icon.ico),)
        APP_RES := $(OUTPUT_BUILD_DIR)/app.res
    endif
endif

# ─── Source files ─────────────────────────────────────────────────────────────

ifeq ($(PLATFORM),mac)
    SRC_NATIVE := $(DIR_PLATFORM)/mac/native_dlg.mm
else ifeq ($(PLATFORM),linux)
    SRC_NATIVE := $(DIR_PLATFORM)/linux/native_dlg.cpp
else
    SRC_NATIVE := $(DIR_PLATFORM)/win/native_dlg.cpp
endif

SRC_CXX := \
    $(DIR_SRC)/main.cpp \
    $(DIR_GUI)/gui.cpp \
    $(DIR_GUI)/workspace.cpp \
    $(DIR_GUI)/capture_controls.cpp \
    $(DIR_SRC)/platform/app_capture.cpp \
    $(DIR_GUI)/monitor.cpp \
    $(DIR_GUI)/mujoco_view.cpp \
    $(DIR_GUI)/inverter_simulator.cpp \
    $(DIR_CORE)/spice_runner.cpp \
    $(DIR_CORE)/safety.cpp \
    $(DIR_GUI)/variable_flash_array.cpp \
    $(DIR_GUI)/variable.cpp \
    $(DIR_GUI)/variable_diagnostics.cpp \
    $(DIR_GUI)/bode.cpp \
    $(DIR_GUI)/bkp_sram_debugger.cpp \
    $(DIR_GUI)/audio_input.cpp \
    $(DIR_GUI)/assembly_viewer.cpp \
    $(DIR_GUI)/sequence_editor.cpp \
    $(DIR_GUI)/device_manager.cpp \
    $(DIR_GUI)/remote_files.cpp \
    $(DIR_GUI)/flash_tool_read.cpp \
    $(DIR_GUI)/flash_tool.cpp \
    $(DIR_GUI)/flash_tool_session.cpp \
    $(DIR_GUI)/midi_tool.cpp \
    $(DIR_GUI)/motor_control.cpp \
    $(DIR_GUI)/fault_decoder.cpp \
    $(DIR_GUI)/fault_decoder_group.cpp \
    $(DIR_GUI)/dso_scope.cpp \
    $(DIR_CORE)/dso_controller.cpp \
    $(DIR_GUI)/motor_position_dial.cpp \
    $(DIR_GUI)/motor_dashboard.cpp \
    $(DIR_GUI)/tutorial_guide.cpp \
    $(DIR_GUI)/sdk_panel.cpp \
    $(DIR_CORE)/elf_parser.cpp \
    $(DIR_CORE)/dwarf_parser.cpp \
    $(DIR_CORE)/json_parser.cpp \
    $(DIR_CORE)/bin_parser.cpp \
    $(DIR_CORE)/jlink_port.cpp \
    $(DIR_CORE)/sampler.cpp \
    $(DIR_CORE)/flash_array_session.cpp \
    $(DIR_CORE)/flash_array.cpp \
    $(DIR_CORE)/remote_files.cpp \
    $(DIR_CORE)/remote_device.cpp \
    $(DIR_GUI)/remote_device_ui.cpp \
    $(DIR_CORE)/http_win.cpp \
    $(DIR_CORE)/local_file_server.cpp \
    $(DIR_CORE)/updater.cpp \
    $(DIR_CORE)/c_header_parser.cpp \
    $(DIR_CORE)/sdk_loader.cpp \
    $(DIR_CORE)/motor_controller.cpp \
    $(DIR_CORE)/motor_jlink.cpp \
    $(DIR_CORE)/motor_jlink_port.cpp \
    $(DIR_GUI)/motor_jlink_ui.cpp \
    $(DIR_CORE)/export_enum.cpp \
    $(filter %.cpp,$(SRC_NATIVE)) \
    $(DIR_PLATFORM)/system_tray.cpp \
    $(DIR_THIRDPARTY)/imgui/imgui.cpp \
    $(DIR_THIRDPARTY)/imgui/imgui_demo.cpp \
    $(DIR_THIRDPARTY)/imgui/imgui_draw.cpp \
    $(DIR_THIRDPARTY)/imgui/imgui_tables.cpp \
    $(DIR_THIRDPARTY)/imgui/imgui_widgets.cpp \
    $(DIR_THIRDPARTY)/imgui/backends/imgui_impl_glfw.cpp \
    $(DIR_THIRDPARTY)/imgui/backends/imgui_impl_opengl3.cpp \
    $(DIR_THIRDPARTY)/implot/implot.cpp \
    $(DIR_THIRDPARTY)/implot/implot_demo.cpp \
    $(DIR_THIRDPARTY)/implot/implot_items.cpp

SRC_C   := $(DIR_THIRDPARTY)/cJSON/cJSON.c $(DIR_CORE)/dso_sdk.c

ifeq ($(COMPILER),msvc)
    INCLUDES += /I"sdk/dsox2024a/inc"
else
    INCLUDES += -Isdk/dsox2024a/inc
endif

$(OUTPUT_BUILD_DIR)/src/core/dso_sdk.o: sdk/dsox2024a/src/dsox2024a.c sdk/dsox2024a/inc/dsox2024a.h
SRC_MM  := $(filter %.mm,$(SRC_NATIVE))

OBJ_CXX := $(patsubst %.cpp,$(OUTPUT_BUILD_DIR)/%.o,$(SRC_CXX))
OBJ_C   := $(patsubst %.c,$(OUTPUT_BUILD_DIR)/%.o,$(SRC_C))
OBJ_MM  := $(patsubst %.mm,$(OUTPUT_BUILD_DIR)/%.o,$(SRC_MM))
OBJ_ALL := $(OBJ_CXX) $(OBJ_C) $(OBJ_MM)

# ─── Header dependencies ────────────────────────────────────────────────────────
#This build does no per - translation - unit dependency scanning.Without it, editing
#a header leaves dependent objects stale — and if that header changes a class's
#size / layout(e.g.adding a member), some TUs see the new layout while others keep
#the old one, corrupting memory at runtime(a make_shared<T> in one TU under -
#allocates for code in another TU).To prevent that, conservatively rebuild every
#first - party object whenever ANY project header changes.Thirdparty objects don't
#include these headers, so they are intentionally left out and never over - rebuild.
library_headers = $(foreach entry,$(wildcard $(1)/*),$(call library_headers,$(entry))) $(wildcard $(1)/*.h $(1)/*.hpp)
PROJECT_HEADERS := $(call library_headers,$(DIR_CKIT)/include) $(wildcard \
    $(DIR_SRC)/*.hpp        $(DIR_SRC)/*.h \
    $(DIR_GUI)/*.hpp        $(DIR_GUI)/*.h \
    $(DIR_CORE)/*.hpp       $(DIR_CORE)/*.h \
    $(DIR_PLATFORM)/*.hpp   $(DIR_PLATFORM)/*.h \
    $(DIR_CKIT)/*.h       $(DIR_CKIT)/include/*.h $(DIR_CKIT)/include/*.hpp \
    sdk/ava_eth/include/ava/*.h \
    $(IMGUI_NOTIFY)/backends/*.hpp)

OBJ_FIRST_PARTY := $(filter $(OUTPUT_BUILD_DIR)/$(DIR_SRC)/%,$(OBJ_ALL))
$(OBJ_FIRST_PARTY): $(PROJECT_HEADERS)

# ─── Targets ──────────────────────────────────────────────────────────────────

.PHONY: all clean fmt info help msvc-win gcc-linux clang-mac package icon sign FORCE_LIBRARIES

.DEFAULT_GOAL := all

all: $(OUTPUT_EXE) $(UPDATER_EXE)
	@$(ECHO_NL)
	@echo make all complete: $(OUTPUT_EXE)
	@$(ECHO_NL)

all: $(OUTPUT_BIN_DIR)/config/STM32H7_LiveAttach.JLinkScript
$(OUTPUT_BIN_DIR)/config/%: config/%
	$(call MKDIR,$(dir $@))
ifeq ($(OS),Windows_NT)
	@copy /Y "$(subst /,\,$<)" "$(subst /,\,$@)" >nul
else
	@cp "$<" "$@"
endif

$(OUTPUT_EXE): $(OBJ_ALL) $(CKIT_LIB) $(APP_RES) assets/mujoco_single_joint.xml assets/mujoco_actuator.xml $(if $(filter msvc,$(COMPILER)),assets/app.manifest) | $(OUTPUT_BIN_DIR)
	@$(ECHO_NL)
	@echo [LINK] $@
ifeq ($(COMPILER),msvc)
	$(LD) $(LDFLAGS) /MANIFEST:EMBED /MANIFESTINPUT:"assets/app.manifest" /MANIFESTUAC:"level='asInvoker' uiAccess='false'" /OUT:"$@" $(OBJ_ALL) $(APP_RES) $(LDLIBS)
	@echo copying runtime DLLs...
	@if exist "thirdparty\ngspice\Spice64\bin\ngspice_con.exe" xcopy /E /I /Y "thirdparty\ngspice\Spice64" "$(subst /,\,$(OUTPUT_BIN_DIR))\Spice64" >nul
	@if not exist "$(subst /,\,$(OUTPUT_BIN_DIR))\SpiceModels" mkdir "$(subst /,\,$(OUTPUT_BIN_DIR))\SpiceModels"
	@if exist "assets\spice\infineon_optimos_100v\OptiMOS5\OptiMOS5_100V_Halfbridges_LTSpice.lib" copy /Y "assets\spice\infineon_optimos_100v\OptiMOS5\OptiMOS5_100V_Halfbridges_LTSpice.lib" "$(subst /,\,$(OUTPUT_BIN_DIR))\SpiceModels\" >nul
	@if exist "assets\mujoco_single_joint.xml" copy /Y "assets\mujoco_single_joint.xml" "$(subst /,\,$(OUTPUT_BIN_DIR))\mujoco_single_joint.xml" >nul
	@if exist "assets\mujoco_actuator.xml" copy /Y "assets\mujoco_actuator.xml" "$(subst /,\,$(OUTPUT_BIN_DIR))\mujoco_actuator.xml" >nul
	@if exist "$(subst /,\,$(MUJOCO_DIR))\bin\mujoco.dll" copy /Y "$(subst /,\,$(MUJOCO_DIR))\bin\mujoco.dll" "$(subst /,\,$(OUTPUT_BIN_DIR))\mujoco.dll" >nul
	@if exist "$(subst /,\,$(MUJOCO_DIR))\LICENSE" copy /Y "$(subst /,\,$(MUJOCO_DIR))\LICENSE" "$(subst /,\,$(OUTPUT_BIN_DIR))\MuJoCo_LICENSE.txt" >nul
	@if exist "$(subst /,\,$(MUJOCO_DIR))\THIRD_PARTY_NOTICES.txt" copy /Y "$(subst /,\,$(MUJOCO_DIR))\THIRD_PARTY_NOTICES.txt" "$(subst /,\,$(OUTPUT_BIN_DIR))\MuJoCo_THIRD_PARTY_NOTICES.txt" >nul
	@copy /Y "$(subst /,\,$(GLFW_DLL))" "$(subst /,\,$(OUTPUT_BIN_DIR))\$(notdir $(GLFW_DLL))" >nul
	@copy /Y "$(subst /,\,$(JLINK_DLL))" "$(subst /,\,$(OUTPUT_BIN_DIR))\$(notdir $(JLINK_DLL))" >nul
	@copy /Y "$(subst /,\,$(FFTW_DLL))" "$(subst /,\,$(OUTPUT_BIN_DIR))\$(notdir $(FFTW_DLL))" >nul
else
	$(CXX) $(LDFLAGS) -o "$@" $(OBJ_ALL) $(LDLIBS)
ifeq ($(PLATFORM),mac)
	@JLINK_EMBEDDED=$$(otool -L "$@" | awk '/libjlinkarm/{print $$1}'); \
	 if [ -n "$$JLINK_EMBEDDED" ]; then \
	     install_name_tool -change "$$JLINK_EMBEDDED" "@rpath/libjlinkarm.dylib" "$@"; \
	 fi
endif
endif

ifeq ($(COMPILER),msvc)
# Ship matched SDK libraries/headers beside the app, including FSA v3's dependency.
# Individual output targets keep SDK-only updates incremental without relinking.
define SDK_PRESET_COPY
all: $(OUTPUT_BIN_DIR)/sdk/$(1)
$(OUTPUT_BIN_DIR)/sdk/$(1): $(2)
	$$(call MKDIR,$$(dir $$@))
	@copy /Y "$$(subst /,\,$$<)" "$$(subst /,\,$$@)" >nul
endef
$(eval $(call SDK_PRESET_COPY,dyn200/dyn200.dll,sdk/dyn200/lib/win/dyn200.dll))
$(eval $(call SDK_PRESET_COPY,dyn200/dyn200.h,sdk/dyn200/inc/dyn200.h))
$(eval $(call SDK_PRESET_COPY,kps6050d/kps6050d.dll,sdk/kps6050d/lib/win/kps6050d.dll))
$(eval $(call SDK_PRESET_COPY,kps6050d/kps6050d.h,sdk/kps6050d/inc/kps6050d.h))
$(eval $(call SDK_PRESET_COPY,dsox2024a/dsox2024a.dll,sdk/dsox2024a/lib/win/dsox2024a.dll))
$(eval $(call SDK_PRESET_COPY,dsox2024a/dsox2024a.h,sdk/dsox2024a/inc/dsox2024a.h))
$(eval $(call SDK_PRESET_COPY,fsav3/fsav3_c.dll,sdk/fsav3/lib/win/fsav3_c.dll))
$(eval $(call SDK_PRESET_COPY,fsav3/fsav3.dll,sdk/fsav3/lib/win/fsav3.dll))
$(eval $(call SDK_PRESET_COPY,fsav3/fsav3_c.h,sdk/fsav3/fsav3_c.h))

# Embed the icon and motor runtime in the executable.
$(OUTPUT_BUILD_DIR)/app.res: assets/app.rc assets/icon.ico sdk/ava_eth/bin/win/ava_eth_sdk.dll sdk/ava_eth/bin/win/libwinpthread-1.dll
	@$(ECHO_NL)
	@echo [RC] $<
	$(call MKDIR,$(dir $@))
	rc.exe /nologo /fo "$@" assets\app.rc

# Generate assets/icon.ico from assets/icon.png (no external tools needed).
icon:
	@echo [ICON] generating assets/icon.ico from assets/icon.png
	python tools\make_icon.py
	@echo icon ready: assets/icon.ico  ^(rebuild with: make^)

# updater.exe: tiny standalone helper that downloads + runs the new installer.
# Shares http_win.o with the main app; winhttp/shell32/user32 are pulled in via
# #pragma comment(lib) inside the sources.
UPDATER_OBJ := $(OUTPUT_BUILD_DIR)/$(DIR_SRC)/updater_main.o $(OUTPUT_BUILD_DIR)/$(DIR_CORE)/http_win.o

$(UPDATER_EXE): $(UPDATER_OBJ) | $(OUTPUT_BIN_DIR)
	@$(ECHO_NL)
	@echo [LINK] $@
	$(LD) $(LDFLAGS) /OUT:"$@" $(UPDATER_OBJ) winhttp.lib shell32.lib user32.lib

# Build the installer with Inno Setup (ISCC). When ISCC is the default, auto-detect
# the compiler at its standard install path; override with: make package ISCC="C:\path\ISCC.exe"
ISCC      ?= iscc
ISCC_PF86 := C:\Program Files (x86)\Inno Setup 6\ISCC.exe
ISCC_PF   := C:\Program Files\Inno Setup 6\ISCC.exe
ISCC_USER := $(LOCALAPPDATA)\Programs\Inno Setup 6\ISCC.exe
ifeq ($(ISCC),iscc)
PACKAGE_RUN = if exist "$(ISCC_PF86)" ( "$(ISCC_PF86)" /DMyAppBinDir="$(abspath $(OUTPUT_BIN_DIR))" "installer\ava_tool.iss" ) else if exist "$(ISCC_PF)" ( "$(ISCC_PF)" /DMyAppBinDir="$(abspath $(OUTPUT_BIN_DIR))" "installer\ava_tool.iss" ) else if exist "$(ISCC_USER)" ( "$(ISCC_USER)" /DMyAppBinDir="$(abspath $(OUTPUT_BIN_DIR))" "installer\ava_tool.iss" ) else ( where iscc >nul 2>nul && iscc /DMyAppBinDir="$(abspath $(OUTPUT_BIN_DIR))" "installer\ava_tool.iss" || ( echo. & echo ERROR: Inno Setup ISCC.exe not found. & echo   Install it from https://jrsoftware.org/isdl.php & echo   then re-run: make package    ^(or: make package ISCC=C:\path\to\ISCC.exe^) & exit 1 ) )
else
PACKAGE_RUN = "$(ISCC)" /DMyAppBinDir="$(abspath $(OUTPUT_BIN_DIR))" "installer\ava_tool.iss"
endif
# Optional Authenticode signing to remove the "Unknown Publisher" warning.
# Provide a certificate and signing is applied automatically (no-op without one):
#   make package AVA_SIGN_PFX="C:\cert.pfx" AVA_SIGN_PASS=secret     (OV .pfx)
#   make package AVA_SIGN_SHA1=<thumbprint>                          (store / EV token)
export AVA_SIGN_PFX
export AVA_SIGN_PASS
export AVA_SIGN_SHA1
export AVA_SIGN_TS
SIGN_CMD = python tools\sign.py

# Sign the app binaries in place (run after a build).
sign:
	@$(SIGN_CMD) "$(OUTPUT_BIN_DIR)/ava_tool.exe" "$(OUTPUT_BIN_DIR)/updater.exe"

package: all
	@$(ECHO_NL)
	@echo [PACKAGE] running Inno Setup...
	@$(SIGN_CMD) "$(OUTPUT_BIN_DIR)/ava_tool.exe" "$(OUTPUT_BIN_DIR)/updater.exe"
	@$(PACKAGE_RUN)
	@$(SIGN_CMD) dist\ava_tool_setup_*.exe
	@echo package complete: see dist\ava_tool_setup_*.exe
endif

# The module archive is produced by its own incremental build. Run that check as
# an order-only prerequisite: a changed archive relinks the app, while an
# unchanged archive does not force a needless relink on every invocation.
FORCE_LIBRARIES:
	@$(ECHO_NL)
	@echo building ckit library...
	@$(MAKE) -C $(DIR_CKIT) static PROJECT_NAME=ckit --no-print-directory

$(CKIT_LIB): | FORCE_LIBRARIES

$(OUTPUT_BUILD_DIR)/%.o: %.cpp
	@$(ECHO_NL)
	@echo [CXX] $<
	$(call MKDIR,$(dir $@))
ifeq ($(COMPILER),msvc)
	$(CXX) $(CXXFLAGS) $(INCLUDES) /Fo"$@" "$<"
else
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o "$@" "$<"
endif

$(OUTPUT_BUILD_DIR)/%.o: %.c
	@$(ECHO_NL)
	@echo [CC]  $<
	$(call MKDIR,$(dir $@))
ifeq ($(COMPILER),msvc)
	$(CC) $(CFLAGS_C) $(INCLUDES) /Fo"$@" "$<"
else
	$(CC) $(CFLAGS_C) $(INCLUDES) -o "$@" "$<"
endif

$(OUTPUT_BUILD_DIR)/%.o: %.mm
	@$(ECHO_NL)
	@echo [MM]  $<
	$(call MKDIR,$(dir $@))
	$(OBJCXX) $(OBJCXXFLAGS) $(INCLUDES) -o "$@" "$<"

$(OUTPUT_BIN_DIR):
	$(call MKDIR,$(OUTPUT_BIN_DIR))

clean:
	@$(ECHO_NL)
	@echo cleaning build artifacts...
	@$(MAKE) -C $(DIR_CKIT) clean --no-print-directory -s
	$(call RMDIR,$(BUILD_DIR))
	$(call RMDIR,$(BIN_DIR))
	@$(ECHO_NL)
	@echo make clean complete.

# ─── Code formatting ──────────────────────────────────────────────────────────

fmt:
	@$(FMT_PYTHON) tools/format_code.py --clang-format "$(CLANG_FORMAT_EXE)" $(foreach dir,$(FMT_DIRS),"$(dir)")

# ─── Info / Help ──────────────────────────────────────────────────────────────

info:
	@$(ECHO_NL)
	@echo build configuration:
	@echo   PROJECT:  $(PROJECT_NAME)
	@echo   PLATFORM: $(PLATFORM)
	@echo   ARCH:     $(ARCH)
	@echo   COMPILER: $(COMPILER)
	@echo   CXX:      $(CXX)
	@echo   CC:       $(CC)
	@echo   OUTPUT:   $(OUTPUT_EXE)
	@echo   OBJ_CNT:  $(words $(OBJ_ALL)) files
	@$(ECHO_NL)

help:
	@$(ECHO_NL)
	@echo usage: make [target] [OPTION=value]
	@$(ECHO_NL)
	@echo targets:
	@echo   all         - build executable (default)
	@echo   clean       - remove all build artifacts
	@echo   fmt         - list formatting changes and apply after confirmation
	@echo   info        - show build configuration
	@echo   help        - show this help
	@$(ECHO_NL)
	@echo   msvc-win    - build for Windows with MSVC
	@echo   gcc-linux   - build for Linux with gcc
	@echo   clang-mac   - build for macOS with Apple Clang
	@$(ECHO_NL)
	@echo options:
	@echo   ARCH=arm64  - force arm64 architecture
	@$(ECHO_NL)
	@echo examples:
	@echo   make             - auto-detect platform and build
	@echo   make -j8         - parallel build with 8 jobs
	@echo   make msvc-win    - build for Windows with MSVC
	@echo   make clang-mac   - build for macOS with Apple Clang
	@echo   make ARCH=arm64  - force arm64 target

msvc-win:
	@$(MAKE) COMPILER=msvc PLATFORM=win all

gcc-linux:
	@$(MAKE) COMPILER=gcc PLATFORM=linux all

clang-mac:
	@$(MAKE) COMPILER=clang PLATFORM=mac all

endif # _MSVC_REINVOKE
