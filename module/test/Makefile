ifeq ($(OS),Windows_NT)
SHELL := cmd.exe
ifndef VCVARS_DONE

_CL_OK := $(shell where cl.exe 2>NUL)

ifeq ($(_CL_OK),)

_PFILES86 := $(strip $(shell powershell -NoProfile -Command "[Environment]::GetFolderPath('ProgramFilesX86')" 2>NUL))
_VSWHERE  := $(_PFILES86)\Microsoft Visual Studio\Installer\vswhere.exe
_VS       := $(strip $(shell "$(_VSWHERE)" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>NUL))
_VCVARS   := $(_VS)\VC\Auxiliary\Build\vcvarsall.bat
_GOALS    := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)
_MSVC_REINVOKE := 1

.PHONY: _msvc_env $(_GOALS)

_msvc_env:
	@echo [MSVC] Initializing Visual Studio environment...
	@call "$(_VCVARS)" x64 >NUL 2>&1 && $(MAKE) $(_GOALS) VCVARS_DONE=1 --no-print-directory

$(_GOALS): _msvc_env
	@:

.DEFAULT_GOAL := all

endif # _CL_OK
endif # VCVARS_DONE
endif # Windows_NT (MSVC init)

ifndef _MSVC_REINVOKE

MODULE_DIR            := $(CURDIR)/..
LIB_DIR               := $(MODULE_DIR)/lib/module
BUILD_DIR             := build
SRC_DIR               := .
EXE_EXT               :=

ifeq ($(OS),Windows_NT)
    PLATFORM          := win
    SHELL             := cmd.exe
    ECHO_NL           := echo.
    EXE_EXT           := .exe
define MKDIR
	@if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))" 2>nul
endef
define RMDIR
	@if exist "$(subst /,\,$(1))" rd /s /q "$(subst /,\,$(1))" 2>nul
endef
else
    UNAME_S           := $(shell uname -s)
    UNAME_M           := $(shell uname -m)
    ifeq ($(UNAME_S),Darwin)
        PLATFORM      := mac
    else
        PLATFORM      := linux
    endif
    SHELL             := /bin/bash
    ECHO_NL           := echo
define MKDIR
	@mkdir -p $(1)
endef
define RMDIR
	@rm -rf $(1)
endef
endif

CC                    := gcc
INCLUDE_DIRS          := -I"$(MODULE_DIR)" -I"$(MODULE_DIR)/inc"
CFLAGS                := -Wall -Wextra -g
LDFLAGS               := -lpthread -lm

ifeq ($(PLATFORM),win)
    CC                := cl.exe
    INCLUDE_DIRS      := /I"$(MODULE_DIR)" /I"$(MODULE_DIR)/inc"
    CFLAGS            := /W3 /nologo /TC /MD /std:clatest /utf-8 /experimental:c11atomics /FS
    OUTPUT_STATIC_LIB := $(LIB_DIR)/win/module.lib
    LDFLAGS           := kernel32.lib ws2_32.lib
else ifeq ($(PLATFORM),mac)
    ARCH              := $(UNAME_M)
    ifeq ($(ARCH),arm64)
        BREW_PREFIX   ?= /opt/homebrew
    else
        BREW_PREFIX   ?= /usr/local
    endif
    CC                := clang
    OUTPUT_STATIC_LIB := $(LIB_DIR)/mac/$(ARCH)/module.a
    INCLUDE_DIRS      += -I$(BREW_PREFIX)/include
    LDFLAGS           += -L$(BREW_PREFIX)/lib -lfftw3f
    CFLAGS            += -fsanitize=address -fno-omit-frame-pointer -mllvm -asan-globals=0
    LDFLAGS           += -fsanitize=address
else
    OUTPUT_STATIC_LIB := $(LIB_DIR)/linux/module.a
    LDFLAGS           += -luring
    CFLAGS            += -fsanitize=address -fno-omit-frame-pointer
    LDFLAGS           += -fsanitize=address
endif

CFLAGS                += $(INCLUDE_DIRS)

SRC_FILES             := $(wildcard *.c)
TARGETS               := $(SRC_FILES:.c=)
BINS                  := $(addprefix $(BUILD_DIR)/,$(addsuffix $(EXE_EXT),$(TARGETS)))
AVA_ECAT_SRC_FILES     := ../sdk/ava_debug/src/ava_ecat.c
AVA_SDK_SRC_FILES     := ../sdk/ava_debug/src/ava_sdk.c ../sdk/ava_debug/src/elf_parser.c $(AVA_ECAT_SRC_FILES)

.PHONY: all clean $(TARGETS) help

all: $(BINS)
	@$(ECHO_NL)

	@echo [test] programs built successfully.
	@$(ECHO_NL)
	@echo [test] make all complete.

clean:
	@$(ECHO_NL)

	@echo [test] cleaning build artifacts...
	$(call RMDIR,$(BUILD_DIR))
	@$(ECHO_NL)
	@echo [test] make clean complete.

help:
	@$(ECHO_NL)

	@echo [test] usage: make [options] [target]
	@$(ECHO_NL)
	@echo targets:
	@echo   all   - compile all .c files
	@echo   clean - clean all generated files
	@echo   help  - show this help message
	@$(ECHO_NL)
	@echo platform: $(PLATFORM)
	@echo found .c files: $(SRC_FILES)
	@echo targets: $(TARGETS)
	@echo library: $(OUTPUT_STATIC_LIB)

$(BUILD_DIR)/%$(EXE_EXT): %.c | $(BUILD_DIR)
	@$(ECHO_NL)
	@echo [test][CC] $<
ifeq ($(PLATFORM),win)
	$(CC) $(CFLAGS) "$<" /Fo"$(BUILD_DIR)\\" /Fe"$@" /link "$(OUTPUT_STATIC_LIB)" $(LDFLAGS)
else
	$(CC) $(CFLAGS) $< -o "$@" "$(OUTPUT_STATIC_LIB)" $(LDFLAGS)
endif

$(BUILD_DIR)/pdo_pid_param_test$(EXE_EXT): pdo_pid_param_test.c $(AVA_ECAT_SRC_FILES) | $(BUILD_DIR)
	@$(ECHO_NL)
	@echo [test][CC] $<
ifeq ($(PLATFORM),win)
	$(CC) $(CFLAGS) "$<" "$(AVA_ECAT_SRC_FILES)" /Fo"$(BUILD_DIR)\\" /Fe"$@" /link $(LDFLAGS)
else
	$(CC) $(CFLAGS) $< $(AVA_ECAT_SRC_FILES) -o "$@" $(LDFLAGS)
endif

$(BUILD_DIR)/ava_sdk_test$(EXE_EXT): ava_sdk_test.c $(AVA_SDK_SRC_FILES) | $(BUILD_DIR)
	@$(ECHO_NL)
	@echo [test][CC] $<
ifeq ($(PLATFORM),win)
	$(CC) $(CFLAGS) "$<" $(AVA_SDK_SRC_FILES) /Fo"$(BUILD_DIR)\\" /Fe"$@" /link $(LDFLAGS)
else
	$(CC) $(CFLAGS) $< $(AVA_SDK_SRC_FILES) -o "$@" $(LDFLAGS)
endif
$(TARGETS): %: $(BUILD_DIR)/%$(EXE_EXT)

$(BUILD_DIR):
	$(call MKDIR,$(BUILD_DIR))

endif # _MSVC_REINVOKE
