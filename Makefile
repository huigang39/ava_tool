ifeq ($(OS),Windows_NT)
SHELL := cmd.exe
ifndef VCVARS_DONE

# Targets that only need Python / file ops — not the MSVC compiler/linker.
# When every requested goal is one of these, skip the (slow) vcvars init below.
# NOTE: `package` is NOT here — it depends on `all`, so it must initialize MSVC
# (otherwise a build after `make clean` can't find cl.exe).
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
DIR_MODULE     := module

BUILD_DIR      := build
BIN_DIR        := bin

FMT_DIRS       := src

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
                        /EHsc /MD /nologo /Z7 /O2
        CFLAGS_C     := /c /W3 /utf-8 /FS /D_CRT_SECURE_NO_WARNINGS /MD /nologo /Z7 /O2
        LDFLAGS      := /SUBSYSTEM:WINDOWS /nologo /DEBUG
        JLINK_DIR    ?= $(DIR_MODULE)/lib/win
        ifeq ($(ARCH),arm64)
            JLINK_LIB := $(JLINK_DIR)/JLink_arm64.lib
            JLINK_DLL := $(JLINK_DIR)/JLink_arm64.dll
        else
            JLINK_LIB := $(JLINK_DIR)/JLink_x64.lib
            JLINK_DLL := $(JLINK_DIR)/JLink_x64.dll
        endif
        GLFW_LIB     := $(DIR_THIRDPARTY)/GLFW/lib/win/glfw3.lib
        GLFW_DLL     := $(DIR_THIRDPARTY)/GLFW/lib/win/glfw3.dll
        FFTW_LIB     := $(DIR_MODULE)/lib/win/libfftw3f-3.lib
        FFTW_DLL     := $(DIR_MODULE)/lib/win/libfftw3f-3.dll
        MODULE_LIB   := $(DIR_MODULE)/lib/module/win/module.lib
        # libffi: obtain via  vcpkg install libffi:x64-windows-static
        # then copy the include/ and lib/x64-windows-static/ output here.
        LIBFFI_DIR   := $(DIR_THIRDPARTY)/libffi
        LIBFFI_LIB   := $(LIBFFI_DIR)/lib/win/ffi.lib
        IMGUI_NOTIFY := $(DIR_THIRDPARTY)/ImGuiNotify/win32
        INCLUDES     := /I"$(DIR_SRC)" /I"$(DIR_CORE)" /I"$(DIR_GUI)" \
                        /I"$(DIR_MODULE)" /I"$(DIR_MODULE)/inc" \
                        /I"$(DIR_THIRDPARTY)/imgui" \
                        /I"$(DIR_THIRDPARTY)/imgui/backends" \
                        /I"$(DIR_THIRDPARTY)/implot" \
                        /I"$(IMGUI_NOTIFY)/backends" \
                        /I"$(IMGUI_NOTIFY)/fonts" \
                        /I"$(DIR_THIRDPARTY)/GLFW/inc" \
                        /I"$(DIR_THIRDPARTY)/cJSON" \
                        /I"$(LIBFFI_DIR)/include"
        LDLIBS       := ws2_32.lib winmm.lib shlwapi.lib comdlg32.lib OpenGL32.lib advapi32.lib ole32.lib dbghelp.lib \
                        "$(GLFW_LIB)" "$(JLINK_LIB)" "$(FFTW_LIB)" "$(MODULE_LIB)" "$(LIBFFI_LIB)"
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
        JLINK_DIR    ?= $(DIR_MODULE)/lib/linux
        JLINK_LIB    := $(firstword $(wildcard $(JLINK_DIR)/libjlinkarm.so $(JLINK_DIR)/libjlinkarm.so.*))
        MODULE_LIB   := $(DIR_MODULE)/lib/module/linux/module.a
        IMGUI_NOTIFY := $(DIR_THIRDPARTY)/ImGuiNotify/unix
        INCLUDES     := -I$(DIR_SRC) -I$(DIR_CORE) -I$(DIR_GUI) \
                        -I$(DIR_MODULE) -I$(DIR_MODULE)/inc \
                        -I$(DIR_THIRDPARTY)/imgui \
                        -I$(DIR_THIRDPARTY)/imgui/backends \
                        -I$(DIR_THIRDPARTY)/implot \
                        -I$(IMGUI_NOTIFY)/backends \
                        -I$(IMGUI_NOTIFY)/fonts \
                        -I$(DIR_THIRDPARTY)/GLFW/inc \
                        -I$(DIR_THIRDPARTY)/cJSON
        LDFLAGS      := -Wl,-rpath,'$$ORIGIN'
        # libffi: sudo apt install libffi-dev
        LDLIBS       := "$(GLFW_LIB)" "$(JLINK_LIB)" -lfftw3f \
                        "$(MODULE_LIB)" -lGL -ldl -lpthread -lffi
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
        JLINK_DIR    ?= $(DIR_MODULE)/lib/mac
        JLINK_LIB    := $(JLINK_DIR)/libjlinkarm.dylib
        MODULE_LIB   := $(DIR_MODULE)/lib/module/mac/$(ARCH)/module.a
        IMGUI_NOTIFY := $(DIR_THIRDPARTY)/ImGuiNotify/unix
        INCLUDES     := -I$(DIR_SRC) -I$(DIR_CORE) -I$(DIR_GUI) \
                        -I$(DIR_MODULE) -I$(DIR_MODULE)/inc \
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
        # libffi: brew install libffi
        LDLIBS       := -lglfw -ljlinkarm -lfftw3f \
                        "$(MODULE_LIB)" -lpthread -lffi \
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

OUTPUT_BUILD_DIR := $(BUILD_DIR)/$(COMPILER)-$(PLATFORM)-$(ARCH)
OUTPUT_BIN_DIR   := $(BIN_DIR)/$(PLATFORM_DIR)
OUTPUT_EXE       := $(OUTPUT_BIN_DIR)/$(PROJECT_NAME)$(EXE_EXT)

# Standalone auto-update helper (Windows/MSVC only).
ifeq ($(COMPILER),msvc)
    UPDATER_EXE := $(OUTPUT_BIN_DIR)/updater$(EXE_EXT)
    # Embed the app icon when assets/icon.ico exists (generate it with `make icon`).
    # Guarded by wildcard so the build still works before the icon is created.
    ifneq ($(wildcard assets/icon.ico),)
        APP_RES := $(OUTPUT_BUILD_DIR)/app.res
    endif
endif

# ─── Source files ─────────────────────────────────────────────────────────────

ifeq ($(PLATFORM),mac)
    SRC_NATIVE := $(DIR_PLATFORM)/native_dlg_mac.mm
else ifeq ($(PLATFORM),linux)
    SRC_NATIVE := $(DIR_PLATFORM)/native_dlg_linux.cpp
else
    SRC_NATIVE := $(DIR_PLATFORM)/native_dlg_win.cpp
endif

SRC_CXX := \
    $(DIR_SRC)/main.cpp \
    $(DIR_GUI)/gui.cpp \
    $(DIR_GUI)/monitor.cpp \
    $(DIR_GUI)/variable.cpp \
    $(DIR_GUI)/bode.cpp \
    $(DIR_GUI)/assembly_viewer.cpp \
    $(DIR_GUI)/sequence_editor.cpp \
    $(DIR_GUI)/tutorial_guide.cpp \
    $(DIR_GUI)/sdk_panel.cpp \
    $(DIR_CORE)/elf_parser.cpp \
    $(DIR_CORE)/dwarf_parser.cpp \
    $(DIR_CORE)/json_parser.cpp \
    $(DIR_CORE)/bin_parser.cpp \
    $(DIR_CORE)/jlink_port.cpp \
    $(DIR_CORE)/sampler.cpp \
    $(DIR_CORE)/http_win.cpp \
    $(DIR_CORE)/updater.cpp \
    $(DIR_CORE)/c_header_parser.cpp \
    $(DIR_CORE)/sdk_loader.cpp \
    $(DIR_CORE)/export_enum.cpp \
    $(filter %.cpp,$(SRC_NATIVE)) \
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

SRC_C   := $(DIR_THIRDPARTY)/cJSON/cJSON.c
SRC_MM  := $(filter %.mm,$(SRC_NATIVE))

OBJ_CXX := $(patsubst %.cpp,$(OUTPUT_BUILD_DIR)/%.o,$(SRC_CXX))
OBJ_C   := $(patsubst %.c,$(OUTPUT_BUILD_DIR)/%.o,$(SRC_C))
OBJ_MM  := $(patsubst %.mm,$(OUTPUT_BUILD_DIR)/%.o,$(SRC_MM))
OBJ_ALL := $(OBJ_CXX) $(OBJ_C) $(OBJ_MM)

# ─── Header dependencies ────────────────────────────────────────────────────────
# This build does no per-translation-unit dependency scanning. Without it, editing
# a header leaves dependent objects stale — and if that header changes a class's
# size/layout (e.g. adding a member), some TUs see the new layout while others keep
# the old one, corrupting memory at runtime (a make_shared<T> in one TU under-
# allocates for code in another TU). To prevent that, conservatively rebuild every
# first-party object whenever ANY project header changes. Thirdparty objects don't
# include these headers, so they are intentionally left out and never over-rebuild.
PROJECT_HEADERS := $(wildcard \
    $(DIR_SRC)/*.hpp        $(DIR_SRC)/*.h \
    $(DIR_GUI)/*.hpp        $(DIR_GUI)/*.h \
    $(DIR_CORE)/*.hpp       $(DIR_CORE)/*.h \
    $(DIR_PLATFORM)/*.hpp   $(DIR_PLATFORM)/*.h \
    $(DIR_MODULE)/*.h       $(DIR_MODULE)/inc/*.h)

OBJ_FIRST_PARTY := $(filter $(OUTPUT_BUILD_DIR)/$(DIR_SRC)/%,$(OBJ_ALL))
$(OBJ_FIRST_PARTY): $(PROJECT_HEADERS)

# ─── Targets ──────────────────────────────────────────────────────────────────

.PHONY: all clean fmt info help msvc-win gcc-linux clang-mac package icon sign

.DEFAULT_GOAL := all

all: $(OUTPUT_EXE) $(UPDATER_EXE)
	@$(ECHO_NL)
	@echo make all complete: $(OUTPUT_EXE)
	@$(ECHO_NL)

$(OUTPUT_EXE): $(OBJ_ALL) $(MODULE_LIB) $(APP_RES) | $(OUTPUT_BIN_DIR)
	@$(ECHO_NL)
	@echo [LINK] $@
ifeq ($(COMPILER),msvc)
	$(LD) $(LDFLAGS) /OUT:"$@" $(OBJ_ALL) $(APP_RES) $(LDLIBS)
	@echo copying runtime DLLs...
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
# Compile the icon resource (.rc -> .res) for embedding into the exe.
$(OUTPUT_BUILD_DIR)/app.res: assets/app.rc assets/icon.ico
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
ifeq ($(ISCC),iscc)
PACKAGE_RUN = if exist "$(ISCC_PF86)" ( "$(ISCC_PF86)" "installer\ava_tool.iss" ) else if exist "$(ISCC_PF)" ( "$(ISCC_PF)" "installer\ava_tool.iss" ) else ( where iscc >nul 2>nul && iscc "installer\ava_tool.iss" || ( echo. & echo ERROR: Inno Setup ISCC.exe not found. & echo   Install it from https://jrsoftware.org/isdl.php & echo   then re-run: make package    ^(or: make package ISCC=C:\path\to\ISCC.exe^) & exit 1 ) )
else
PACKAGE_RUN = "$(ISCC)" "installer\ava_tool.iss"
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
	@$(SIGN_CMD) bin\win\ava_tool.exe bin\win\updater.exe

package: all
	@$(ECHO_NL)
	@echo [PACKAGE] running Inno Setup...
	@$(SIGN_CMD) bin\win\ava_tool.exe bin\win\updater.exe
	@$(PACKAGE_RUN)
	@$(SIGN_CMD) dist\ava_tool_setup_*.exe
	@echo package complete: see dist\ava_tool_setup_*.exe
endif

$(MODULE_LIB):
	@$(ECHO_NL)
	@echo building module library...
	@$(MAKE) -C $(DIR_MODULE) static --no-print-directory

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
	@$(MAKE) -C $(DIR_MODULE) clean --no-print-directory -s
	$(call RMDIR,$(BUILD_DIR))
	$(call RMDIR,$(BIN_DIR))
	@$(ECHO_NL)
	@echo make clean complete.

# ─── Code formatting ──────────────────────────────────────────────────────────

ifeq ($(PLATFORM),win)
FMT_FILES := $(shell powershell -NoProfile -Command \
    "Get-ChildItem -Path '$(FMT_DIRS)' -Include *.c,*.cpp,*.h,*.hpp -Recurse -File | \
     ForEach-Object { $$_.FullName }" 2>nul)
else
FMT_FILES := $(shell find $(FMT_DIRS) -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) 2>/dev/null)
endif

fmt:
	@$(ECHO_NL)
	@echo formatting code files...
ifeq ($(PLATFORM),win)
	@where clang-format >nul 2>&1 || (echo [ERROR] clang-format not found && exit 1)
	@powershell -NoProfile -Command \
	    "$$files='$(FMT_FILES)'.Split(' '); \
	     foreach ($$f in $$files) { \
	         if ($$f -and (Test-Path $$f)) { \
	             Write-Host ('  formatting: ' + $$f); \
	             clang-format -i -style=file $$f \
	         } \
	     }"
else
	@command -v clang-format >/dev/null 2>&1 || (echo "[ERROR] clang-format not found" && exit 1)
	@for f in $(FMT_FILES); do \
		echo "  formatting: $$f"; \
		clang-format -i -style=file "$$f" 2>/dev/null || clang-format -i "$$f"; \
	done
endif
	@$(ECHO_NL)
	@echo make fmt complete.

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
	@echo   fmt         - format source files with clang-format
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
