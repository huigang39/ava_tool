ifeq ($(OS),Windows_NT)
SHELL := cmd.exe
ifndef VCVARS_DONE

_GOALS    := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),static)
_MSVC_DEFAULT_GOALS := static shared all msvc-win tool test
_MSVC_ENV_GOALS := $(if $(filter msvc,$(COMPILER)),$(_GOALS),$(filter $(_MSVC_DEFAULT_GOALS),$(_GOALS)))

ifneq ($(_MSVC_ENV_GOALS),)

_CL_OK := $(shell where cl.exe 2>NUL)

ifeq ($(_CL_OK),)

_PFILES86 := $(strip $(shell powershell -NoProfile -Command "[Environment]::GetFolderPath('ProgramFilesX86')" 2>NUL))
_VSWHERE  := $(_PFILES86)\Microsoft Visual Studio\Installer\vswhere.exe
_VS       := $(strip $(shell "$(_VSWHERE)" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>NUL))
_VCVARS   := $(_VS)\VC\Auxiliary\Build\vcvarsall.bat
_MSVC_REINVOKE := 1

.PHONY: _msvc_env $(_GOALS)

_msvc_env:
	@echo [MSVC] Initializing Visual Studio environment...
	@call "$(_VCVARS)" x64 >NUL 2>&1 && $(MAKE) $(_GOALS) VCVARS_DONE=1 --no-print-directory

$(_GOALS): _msvc_env
	@:

.DEFAULT_GOAL := static

endif # _CL_OK
endif # _MSVC_ENV_GOALS
endif # VCVARS_DONE
endif # Windows_NT (MSVC init)

ifndef _MSVC_REINVOKE

PROJECT_NAME              := module
PROJECT_DIR               := $(CURDIR)

KEIL_PATH                 ?= C:/Keil_v5

# COMPILER 默认值会根据下方宿主平台自动选择 (msvc / clang / gcc)
OPTIMIZATION              := -O2

SRC_DIR                   := src
BUILD_DIR                 := build
LIB_DIR                   := lib/$(PROJECT_NAME)

INCLUDE_FILES             :=
EXCLUDE_FILES             :=
INCLUDE_PATTERNS          :=
EXCLUDE_PATTERNS          :=

space                     := $(subst ,, )
FMT_EXTENSIONS            := .c .cpp .h .hpp
FMT_DIRS                  := src inc test tool script
FMT_EXCLUDE_DIRS          :=

ifeq ($(OS),Windows_NT)
    PLATFORM              := win
    ARCH                  ?= $(if $(filter AMD64,$(PROCESSOR_ARCHITECTURE)),x86_64,$(if $(filter ARM64,$(PROCESSOR_ARCHITECTURE)),arm64,x86_64))
    COMPILER              ?= msvc
    SHELL                 := cmd.exe
    ECHO_NL               := echo.
define MKDIR
	@if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))" 2>nul
endef
define RMDIR
	@if exist "$(subst /,\,$(1))" rd /s /q "$(subst /,\,$(1))" 2>nul
endef
else
    UNAME_S               := $(shell uname -s)
    UNAME_M               := $(shell uname -m)
    ifeq ($(UNAME_S),Darwin)
        PLATFORM          ?= mac
        COMPILER          ?= clang
    else ifeq ($(UNAME_S),Linux)
        PLATFORM          ?= linux
        COMPILER          ?= gcc
    else
        PLATFORM          ?= linux
        COMPILER          ?= gcc
    endif
    ifeq ($(UNAME_M),x86_64)
        ARCH              ?= x86_64
    else ifeq ($(UNAME_M),amd64)
        ARCH              ?= x86_64
    else ifeq ($(UNAME_M),arm64)
        ARCH              ?= arm64
    else ifeq ($(UNAME_M),aarch64)
        ARCH              ?= arm64
    else
        ARCH              ?= $(UNAME_M)
    endif
    SHELL                 := /bin/bash
    ECHO_NL               := echo
define MKDIR
	@mkdir -p $(1)
endef
define RMDIR
	@rm -rf $(1)
endef
endif

AC5_COMPILER_PATH         := $(KEIL_PATH)/ARM/ARMCC/bin
AC5_COMPILER_EXE          := armcc.exe
AC5_ARCHIVER_EXE          := armar.exe
AC5_MCU_STATIC_LIB        := $(PROJECT_NAME).lib
AC5_MCU_CFLAGS            := -c $(OPTIMIZATION) -g --cpu=Cortex-M7 --apcs=interwork

AC6_COMPILER_PATH         := $(KEIL_PATH)/ARM/ARMCLANG/bin
AC6_COMPILER_EXE          := armclang.exe
AC6_ARCHIVER_EXE          := armar.exe
AC6_MCU_STATIC_LIB        := $(PROJECT_NAME).lib
AC6_MCU_CFLAGS            := -c $(OPTIMIZATION) -g --target=arm-arm-none-eabi -mcpu=Cortex-M7 -mfloat-abi=hard -mfpu=fpv5-d16

GCC_COMPILER_EXE          := gcc
GCC_ARCHIVER_EXE          := ar
GCC_MCU_STATIC_LIB        := $(PROJECT_NAME).a
GCC_MCU_CFLAGS            := -c $(OPTIMIZATION) -g -Wall -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16

GCC_LINUX_STATIC_LIB      := $(PROJECT_NAME).a
GCC_LINUX_SHARED_LIB      := $(PROJECT_NAME).so
GCC_LINUX_CFLAGS          := -c $(OPTIMIZATION) -g -Wall -Wextra -fPIC
GCC_LINUX_LDFLAGS         := -shared -fPIC
GCC_LINUX_LIB_DIRS        := -Llib/linux
GCC_LINUX_LDLIBS          := -lfftw3f -luring -lpthread -lm

# MSVC (cl.exe / lib.exe / link.exe)，仅 Windows 可用
# 需在 "x64 Native Tools Command Prompt" 中运行以加载 vcvarsall.bat
MSVC_COMPILER_EXE         := cl.exe
MSVC_ARCHIVER_EXE         := lib.exe
MSVC_LINKER_EXE           := link.exe
MSVC_OPTIMIZATION         := /O2
MSVC_WIN_STATIC_LIB       := $(PROJECT_NAME).lib
MSVC_WIN_SHARED_LIB       := $(PROJECT_NAME).dll
MSVC_WIN_CFLAGS           := /c $(MSVC_OPTIMIZATION) /Z7 /W3 /nologo /TC /MD /std:clatest /utf-8 /experimental:c11atomics /FS
MSVC_WIN_LDFLAGS          := /DLL /nologo /DEBUG
MSVC_WIN_LIB_DIRS         := /LIBPATH:lib/win
MSVC_WIN_LDLIBS           := libfftw3f-3.lib ws2_32.lib

# macOS 默认使用 Apple Clang，Homebrew 安装前缀按架构区分
ifeq ($(ARCH),arm64)
    MAC_BREW_PREFIX       ?= /opt/homebrew
else
    MAC_BREW_PREFIX       ?= /usr/local
endif
CLANG_MAC_STATIC_LIB      := $(PROJECT_NAME).a
CLANG_MAC_SHARED_LIB      := $(PROJECT_NAME).dylib
CLANG_MAC_CFLAGS          := -c $(OPTIMIZATION) -g -Wall -Wextra -fPIC -I$(MAC_BREW_PREFIX)/include
CLANG_MAC_LDFLAGS         := -dynamiclib -fPIC -L$(MAC_BREW_PREFIX)/lib
CLANG_MAC_LIB_DIRS        := -L$(MAC_BREW_PREFIX)/lib
CLANG_MAC_LDLIBS          := -lfftw3f -lpthread -lm

ifneq ($(COMPILER),)
    ifeq ($(COMPILER),ac5)
        ifeq ($(PLATFORM),mcu)
            CC            := $(AC5_COMPILER_PATH)/$(AC5_COMPILER_EXE)
            AR            := $(AC5_COMPILER_PATH)/$(AC5_ARCHIVER_EXE)
            CFLAGS        := $(AC5_MCU_CFLAGS)
            STATIC_LIB    := $(AC5_MCU_STATIC_LIB)
            PLATFORM_DIR  := mcu
        else
            $(error ac5 仅支持 mcu 平台)
        endif
    else ifeq ($(COMPILER),ac6)
        ifeq ($(PLATFORM),mcu)
            CC            := $(AC6_COMPILER_PATH)/$(AC6_COMPILER_EXE)
            AR            := $(AC6_COMPILER_PATH)/$(AC6_ARCHIVER_EXE)
            CFLAGS        := $(AC6_MCU_CFLAGS)
            STATIC_LIB    := $(AC6_MCU_STATIC_LIB)
            PLATFORM_DIR  := mcu
        else
            $(error ac6 仅支持 mcu 平台)
        endif
    else ifeq ($(COMPILER),gcc)
        ifeq ($(PLATFORM),mcu)
            CC            := $(GCC_COMPILER_EXE)
            AR            := $(GCC_ARCHIVER_EXE)
            CFLAGS        := $(GCC_MCU_CFLAGS)
            STATIC_LIB    := $(GCC_MCU_STATIC_LIB)
            PLATFORM_DIR  := mcu
        else ifeq ($(PLATFORM),linux)
            CC            := $(GCC_COMPILER_EXE)
            AR            := $(GCC_ARCHIVER_EXE)
            CFLAGS        := $(GCC_LINUX_CFLAGS)
            LDFLAGS       := $(GCC_LINUX_LDFLAGS) $(GCC_LINUX_LIB_DIRS)
            LDLIBS        := $(GCC_LINUX_LDLIBS)
            STATIC_LIB    := $(GCC_LINUX_STATIC_LIB)
            SHARED_LIB    := $(GCC_LINUX_SHARED_LIB)
            PLATFORM_DIR  := linux
        else
            $(error gcc 不支持该平台: $(PLATFORM))
        endif
    else ifeq ($(COMPILER),clang)
        ifeq ($(PLATFORM),mac)
            CC            := clang
            AR            := ar
            CFLAGS        := $(CLANG_MAC_CFLAGS)
            LDFLAGS       := $(CLANG_MAC_LDFLAGS)
            LDLIBS        := $(CLANG_MAC_LDLIBS)
            STATIC_LIB    := $(CLANG_MAC_STATIC_LIB)
            SHARED_LIB    := $(CLANG_MAC_SHARED_LIB)
            PLATFORM_DIR  := mac/$(ARCH)
        else ifeq ($(PLATFORM),linux)
            CC            := clang
            AR            := ar
            CFLAGS        := $(GCC_LINUX_CFLAGS)
            LDFLAGS       := $(GCC_LINUX_LDFLAGS) $(GCC_LINUX_LIB_DIRS)
            LDLIBS        := $(GCC_LINUX_LDLIBS)
            STATIC_LIB    := $(GCC_LINUX_STATIC_LIB)
            SHARED_LIB    := $(GCC_LINUX_SHARED_LIB)
            PLATFORM_DIR  := linux
        else
            $(error clang 仅支持 mac 或 linux 平台)
        endif
    else ifeq ($(COMPILER),msvc)
        ifeq ($(PLATFORM),win)
            CC            := $(MSVC_COMPILER_EXE)
            AR            := $(MSVC_ARCHIVER_EXE)
            LD            := $(MSVC_LINKER_EXE)
            CFLAGS        := $(MSVC_WIN_CFLAGS)
            LDFLAGS       := $(MSVC_WIN_LDFLAGS) $(MSVC_WIN_LIB_DIRS)
            LDLIBS        := $(MSVC_WIN_LDLIBS)
            STATIC_LIB    := $(MSVC_WIN_STATIC_LIB)
            SHARED_LIB    := $(MSVC_WIN_SHARED_LIB)
            PLATFORM_DIR  := win
        else
            $(error msvc 仅支持 win 平台)
        endif
    else
        $(error 无效的编译器: $(COMPILER), 可选 msvc, clang, gcc, ac5, ac6)
    endif
endif

ifeq ($(COMPILER),msvc)
    INCLUDE_DIRS          := /I"$(PROJECT_DIR)/inc"
else
    INCLUDE_DIRS          := -I"$(PROJECT_DIR)/inc"
endif
CFLAGS                    += $(INCLUDE_DIRS)

OUTPUT_BUILD_DIR          := $(BUILD_DIR)/$(COMPILER)-$(PLATFORM)-$(ARCH)
OUTPUT_LIB_DIR            := $(LIB_DIR)/$(PLATFORM_DIR)
OUTPUT_STATIC_LIB         := $(OUTPUT_LIB_DIR)/$(STATIC_LIB)
OUTPUT_SHARED_LIB         := $(OUTPUT_LIB_DIR)/$(SHARED_LIB)

ALL_SRC_FILES             := $(foreach dir,$(SRC_DIR),$(wildcard $(dir)/*.c))

ifeq ($(ALL_SRC_FILES),)
    ifeq ($(OS),Windows_NT)
        ALL_SRC_FILES     := $(shell powershell -NoProfile -Command "Get-ChildItem -Path '$(SRC_DIR)' -Filter *.c -Recurse -File | ForEach-Object { $$_.FullName }" 2>nul)
        ifeq ($(ALL_SRC_FILES),)
            ALL_SRC_FILES := $(shell cmd /c "for /r $(SRC_DIR) %%f in (*.c) do @echo %%~ff" 2>nul)
        endif
    else
        ALL_SRC_FILES     := $(shell find $(SRC_DIR) -type f -name "*.c" 2>/dev/null)
    endif
endif

ifneq ($(INCLUDE_FILES),)
    SRC_FILES             := $(filter $(addprefix %/,$(INCLUDE_FILES)),$(ALL_SRC_FILES)) $(filter $(INCLUDE_FILES),$(ALL_SRC_FILES))
else
    SRC_FILES             := $(ALL_SRC_FILES)
    ifneq ($(EXCLUDE_FILES),)
        SRC_FILES         := $(filter-out $(addprefix %/,$(EXCLUDE_FILES)),$(SRC_FILES)) $(filter-out $(EXCLUDE_FILES),$(SRC_FILES))
    endif
    ifneq ($(EXCLUDE_PATTERNS),)
        $(foreach pattern,$(EXCLUDE_PATTERNS),                                                   \
            $(eval EXCLUDE_MATCH := $(foreach dir,$(SRC_SUBDIRS),$(wildcard $(dir)/$(pattern)))) \
            $(eval SRC_FILES     := $(filter-out $(EXCLUDE_MATCH),$(SRC_FILES))))
    endif
    ifneq ($(INCLUDE_PATTERNS),)
        SRC_FILES         := $(foreach pattern,$(INCLUDE_PATTERNS),$(foreach dir,$(SRC_SUBDIRS),$(wildcard $(dir)/$(pattern))))
    endif
endif

OBJ_FILES                 := $(SRC_FILES:$(SRC_DIR)/%.c=$(OUTPUT_BUILD_DIR)/%.o)

.PHONY: all static shared tool test clean fmt list help info ac5-mcu ac6-mcu gcc-mcu gcc-linux clang-mac clang-linux msvc-win

.DEFAULT_GOAL := static

static: $(OUTPUT_STATIC_LIB)
	@$(ECHO_NL)

	@echo [module] static library created successfully: $(OUTPUT_STATIC_LIB)
	@$(ECHO_NL)
	@echo [module] make static complete.

shared:
ifeq ($(SHARED_LIB),)
	@$(ECHO_NL)

	@echo [module] 当前平台/编译器组合不支持动态库 (COMPILER=$(COMPILER), PLATFORM=$(PLATFORM))
	@$(ECHO_NL)
else
	@$(MAKE) $(OUTPUT_SHARED_LIB)
	@$(ECHO_NL)

	@echo [module] shared library created successfully: $(OUTPUT_SHARED_LIB)
	@$(ECHO_NL)
	@echo [module] make shared complete.
endif

all: $(OUTPUT_STATIC_LIB) $(if $(SHARED_LIB),$(OUTPUT_SHARED_LIB),)
	@$(ECHO_NL)

	@echo [module] static library created successfully: $(OUTPUT_STATIC_LIB)
ifneq ($(SHARED_LIB),)
	@echo [module] shared library created successfully: $(OUTPUT_SHARED_LIB)
endif
	@$(ECHO_NL)
	@echo [module] make all complete.

tool: $(OUTPUT_STATIC_LIB)
	@$(ECHO_NL)

	@echo [module] building tool programs...
	@cd tool && "$(MAKE)"
	@$(ECHO_NL)
	@echo [module] make tool complete.

test: $(OUTPUT_STATIC_LIB)
	@$(ECHO_NL)

	@echo [module] building test programs...
	@cd test && "$(MAKE)"
	@$(ECHO_NL)
	@echo [module] make test complete.

clean:
	@$(ECHO_NL)

	@echo [module] cleaning build artifacts...
	@$(MAKE) -C tool clean --no-print-directory -s
	@$(MAKE) -C test clean --no-print-directory -s
	$(call RMDIR,$(BUILD_DIR))
	$(call RMDIR,$(LIB_DIR))
	@$(ECHO_NL)
	@echo [module] make clean complete.

ifeq ($(OS),Windows_NT)
    FMT_EXCLUDE_PATTERN   := $(subst $(space),|,$(FMT_EXCLUDE_DIRS))
    FMT_PS                := $$dirs='$(strip $(FMT_DIRS))' -split '\s+'; $$excludes='$(FMT_EXCLUDE_PATTERN)'; $$files=@(); foreach ($$dir in $$dirs) { if ($$dir -and (Test-Path $$dir)) { $$files += Get-ChildItem -Path $$dir -Include *.c,*.cpp,*.h,*.hpp -Recurse -File | Where-Object { if ($$excludes) { $$_.FullName -notmatch $$excludes } else { $$true } } } }
else
    FMT_FILES             := $(shell find $(FMT_DIRS) -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) $(foreach dir,$(FMT_EXCLUDE_DIRS),! -path "*/$(dir)/*") 2>/dev/null)
endif

fmt:
	@$(ECHO_NL)

	@echo [module] formatting code files...
ifeq ($(PLATFORM),win)
	@where clang-format >nul 2>&1 || (echo [ERROR] clang-format not found. please install clang-format. && exit 1)
	@powershell -NoProfile -Command "$(FMT_PS); if (Get-Command dos2unix -ErrorAction SilentlyContinue) { Write-Host 'converting line endings to LF...'; foreach ($$file in $$files) { dos2unix $$file.FullName 2>$$null } }"
	@powershell -NoProfile -Command "$(FMT_PS); Write-Host ('found {0} file(s) to format' -f $$files.Count)"
	@$(ECHO_NL)
	@powershell -NoProfile -Command "$(FMT_PS); foreach ($$file in $$files) { Write-Host \"formatting: $$($$file.FullName)\"; clang-format -i -style=file $$file.FullName 2>$$null; if ($$LASTEXITCODE -ne 0) { clang-format -i $$file.FullName 2>$$null } }"
	@$(ECHO_NL)
else
	@command -v clang-format >/dev/null 2>&1 || (echo [ERROR] clang-format not found. please install clang-format. && exit 1)
	@if command -v dos2unix >/dev/null 2>&1; then                                 \
		echo "converting line endings to LF...";                                  \
		for file in $(FMT_FILES); do dos2unix "$$file" 2>/dev/null || true; done; \
	fi
	@echo found $(words $(FMT_FILES)) file(s) to format
	@$(ECHO_NL)
	@for file in $(FMT_FILES); do                                                                     \
		if [ -n "$$file" ] && [ -f "$$file" ]; then                                                   \
			echo "formatting: $$file";                                                                \
			clang-format -i -style=file "$$file" 2>/dev/null || clang-format -i "$$file" 2>/dev/null; \
		fi;                                                                                           \
	done
	@$(ECHO_NL)
endif
	@echo [module] make fmt complete.

list:
	@$(ECHO_NL)

	@echo [module] available source files:
ifeq ($(PLATFORM),win)
	@powershell -NoProfile -Command "$$idx=0; Get-ChildItem -Path '$(SRC_DIR)' -Filter *.c -Recurse -File | Sort-Object FullName | ForEach-Object { $$idx++; Write-Host ('  {0,2}. {1}' -f $$idx, $$_.FullName) }" 2>nul || \
	 cmd /c "setlocal enabledelayedexpansion & set /a idx=0 & for /r $(SRC_DIR) %%f in (*.c) do @set /a idx+=1 & echo   !idx!. %%~ff"
else
	@find $(SRC_DIR) -type f -name "*.c" 2>/dev/null | sort | awk '{printf "  %2d. %s\n", NR, $$0}'
endif

info:
	@$(ECHO_NL)

	@echo [module] build configuration:
	@echo   PLATFORM: $(PLATFORM)
	@echo   ARCH: $(ARCH)
	@echo   CC: $(CC)
	@echo   AR: $(AR)
	@echo   CFLAGS: $(CFLAGS)
	@echo   SRC_FILES: $(words $(SRC_FILES))
	@echo   OUTPUT_STATIC_LIB: $(OUTPUT_STATIC_LIB)
	@echo   OUTPUT_SHARED_LIB: $(OUTPUT_SHARED_LIB)

help:
	@$(ECHO_NL)

	@echo usage: make [options] [target]
	@$(ECHO_NL)
	@echo targets:
	@echo   static - build static library only (default, no external deps)
	@echo   shared - build shared library only (requires external deps like fftw3f)
	@echo   all    - build both static and shared libraries
	@echo   tool   - build the tool program
	@echo   test   - build the test program
	@echo   clean  - remove build artifacts
	@echo   fmt    - format code files using clang-format
	@echo   list   - list all available source files
	@echo   info   - show build configuration
	@echo   help   - show this help message
	@$(ECHO_NL)
	@echo options:
	@echo   INCLUDE_FILES=^<files^> - include only these files (space-separated)
	@echo   EXCLUDE_FILES=^<files^> - exclude these files (space-separated)
	@$(ECHO_NL)
	@echo examples:
	@echo   make           - build static lib only with host defaults (win=msvc, mac=clang, linux=gcc)
	@echo   make shared    - build shared lib only (requires fftw3f etc.)
	@echo   make all       - build both static and shared libs
	@echo   make ac5-mcu   - cross-compile for MCU with Arm Compiler 5 (armcc)
	@echo   make ac6-mcu   - cross-compile for MCU with Arm Compiler 6 (armclang)
	@echo   make gcc-mcu   - cross-compile for MCU with arm-none-eabi-gcc
	@echo   make msvc-win  - build for Windows with MSVC (run from x64 Native Tools Cmd)
	@echo   make clang-mac - build for macOS with Apple Clang (auto x86_64/arm64)
	@echo   make gcc-linux - build for Linux with gcc

ac5-mcu:
	@$(MAKE) COMPILER=ac5 PLATFORM=mcu static

ac6-mcu:
	@$(MAKE) COMPILER=ac6 PLATFORM=mcu static

gcc-mcu:
	@$(MAKE) COMPILER=gcc PLATFORM=mcu static

gcc-linux:
	@$(MAKE) COMPILER=gcc PLATFORM=linux static

clang-mac:
	@$(MAKE) COMPILER=clang PLATFORM=mac static

clang-linux:
	@$(MAKE) COMPILER=clang PLATFORM=linux static

msvc-win:
	@$(MAKE) COMPILER=msvc PLATFORM=win static

$(OUTPUT_STATIC_LIB): $(OBJ_FILES) | $(OUTPUT_LIB_DIR)
	@$(ECHO_NL)

	@echo [module] creating static library: $(STATIC_LIB)
ifeq ($(COMPILER),msvc)
	$(AR) /nologo /OUT:"$@" $(OBJ_FILES)
else ifeq ($(COMPILER),gcc)
	$(AR) rcs "$@" $(OBJ_FILES)
else ifeq ($(COMPILER),clang)
	$(AR) rcs "$@" $(OBJ_FILES)
else
	$(AR) -r "$@" $(OBJ_FILES)
endif

$(OUTPUT_SHARED_LIB): $(OBJ_FILES) | $(OUTPUT_LIB_DIR)
	@$(ECHO_NL)

	@echo [module] creating shared library: $(SHARED_LIB)
ifeq ($(COMPILER),msvc)
	$(LD) $(LDFLAGS) /OUT:"$@" /IMPLIB:"$(OUTPUT_LIB_DIR)/$(PROJECT_NAME).lib" $(OBJ_FILES) $(LDLIBS)
else ifeq ($(COMPILER),clang)
ifeq ($(PLATFORM),mac)
	$(CC) $(LDFLAGS) -o "$@" $(OBJ_FILES) $(LDLIBS)
else ifeq ($(PLATFORM),linux)
	$(CC) $(LDFLAGS) -o "$@" $(OBJ_FILES) $(LDLIBS)
else
	$(error clang+$(PLATFORM) 不支持动态库)
endif
else ifeq ($(COMPILER),gcc)
ifeq ($(PLATFORM),linux)
	$(CC) $(LDFLAGS) -o "$@" $(OBJ_FILES) $(LDLIBS)
else
	$(error 该平台不支持动态库: $(PLATFORM))
endif
else
	$(error 该编译器不支持动态库: $(COMPILER))
endif

$(OUTPUT_BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(OUTPUT_BUILD_DIR)
	@$(ECHO_NL)
	@echo [module][CC] $<
	$(call MKDIR,$(dir $@))
ifeq ($(COMPILER),msvc)
	$(CC) $(CFLAGS) /Fo"$@" "$<"
else ifeq ($(COMPILER),gcc)
	$(CC) $(CFLAGS) -o "$@" "$<"
else ifeq ($(COMPILER),clang)
	$(CC) $(CFLAGS) -o "$@" "$<"
else
	$(CC) $(CFLAGS) -o"$@" "$<"
endif

$(OUTPUT_BUILD_DIR):
	$(call MKDIR,$(OUTPUT_BUILD_DIR))

$(OUTPUT_LIB_DIR):
	$(call MKDIR,$(OUTPUT_LIB_DIR))

endif # _MSVC_REINVOKE
