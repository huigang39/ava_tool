PROJECT_NAME              := module
PROJECT_DIR               := $(CURDIR)

KEIL_PATH                 ?= C:/Keil_v5

COMPILER                  ?= gcc
OPTIMIZATION              := -O3

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
    SHELL                 := cmd.exe
    ECHO_NL               := echo.
define MKDIR
	@if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))" 2>nul
endef
define RMDIR
	@if exist "$(subst /,\,$(1))" rd /s /q "$(subst /,\,$(1))" 2>nul
endef
else
    PLATFORM              := linux
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

AC6_COMPILER_PATH         := $(KEIL_PATH)/ARM/ARMCLANG/bin
AC6_COMPILER_EXE          := armclang.exe
AC6_ARCHIVER_EXE          := armar.exe

GCC_COMPILER_EXE          := gcc
GCC_ARCHIVER_EXE          := ar

AC5_MCU_DEFINE            := -DMCU
AC5_MCU_STATIC_LIB        := $(PROJECT_NAME).lib
AC5_MCU_CFLAGS            := -c $(OPTIMIZATION) -g --cpu=Cortex-M7 --apcs=interwork

AC6_MCU_DEFINE            := -DMCU
AC6_MCU_STATIC_LIB        := $(PROJECT_NAME).lib
AC6_MCU_CFLAGS            := -c $(OPTIMIZATION) -g --target=arm-arm-none-eabi -mcpu=Cortex-M7 -mfloat-abi=hard -mfpu=fpv5-d16

GCC_MCU_DEFINE            := -DMCU
GCC_MCU_STATIC_LIB        := $(PROJECT_NAME).a
GCC_MCU_CFLAGS            := -c $(OPTIMIZATION) -g -Wall -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16

GCC_LINUX_DEFINE          := -DLINUX
GCC_LINUX_STATIC_LIB      := $(PROJECT_NAME).a
GCC_LINUX_SHARED_LIB      := $(PROJECT_NAME).so
GCC_LINUX_CFLAGS          := -c $(OPTIMIZATION) -g -Wall -Wextra -fPIC
GCC_LINUX_LDFLAGS         := -shared -fPIC
GCC_LINUX_LIB_DIRS        := -Llib/linux
GCC_LINUX_LDLIBS          := -luring

GCC_WIN_DEFINE            := -DWIN
GCC_WIN_STATIC_LIB        := $(PROJECT_NAME).a
GCC_WIN_SHARED_LIB        := $(PROJECT_NAME).dll
GCC_WIN_CFLAGS            := -c $(OPTIMIZATION) -g -Wall -Wextra -fPIC
GCC_WIN_LDFLAGS           := -shared -fPIC
GCC_WIN_LIB_DIRS          := -Llib/win
GCC_WIN_LDLIBS            := -lfftw3f-3 -lws2_32

ifneq ($(COMPILER),)
    ifeq ($(COMPILER),ac5)
        ifeq ($(PLATFORM),mcu)
            CC            := $(AC5_COMPILER_PATH)/$(AC5_COMPILER_EXE)
            AR            := $(AC5_COMPILER_PATH)/$(AC5_ARCHIVER_EXE)
            CFLAGS        := $(AC5_MCU_CFLAGS) $(AC5_MCU_DEFINE)
            STATIC_LIB    := $(AC5_MCU_STATIC_LIB)
            PLATFORM_DIR  := mcu
        else
            $(error ac5 only supports mcu platform)
        endif
    else ifeq ($(COMPILER),ac6)
        ifeq ($(PLATFORM),mcu)
            CC            := $(AC6_COMPILER_PATH)/$(AC6_COMPILER_EXE)
            AR            := $(AC6_COMPILER_PATH)/$(AC6_ARCHIVER_EXE)
            CFLAGS        := $(AC6_MCU_CFLAGS) $(AC6_MCU_DEFINE)
            STATIC_LIB    := $(AC6_MCU_STATIC_LIB)
            PLATFORM_DIR  := mcu
        else
            $(error ac6 only supports mcu platform)
        endif
    else ifeq ($(COMPILER),gcc)
    	ifeq ($(PLATFORM),mcu)
            CC            := $(GCC_COMPILER_EXE)
            AR            := $(GCC_ARCHIVER_EXE)
            CFLAGS        := $(GCC_MCU_CFLAGS) $(GCC_MCU_DEFINE)
            STATIC_LIB    := $(GCC_MCU_STATIC_LIB)
            PLATFORM_DIR  := mcu
        else ifeq ($(PLATFORM),linux)
            CC            := $(GCC_COMPILER_EXE)
            AR            := $(GCC_ARCHIVER_EXE)
            CFLAGS        := $(GCC_LINUX_CFLAGS) $(GCC_LINUX_DEFINE)
            LDFLAGS       := $(GCC_LINUX_LDFLAGS) $(GCC_LINUX_LIB_DIRS)
            LDLIBS        := $(GCC_LINUX_LDLIBS)
            STATIC_LIB    := $(GCC_LINUX_STATIC_LIB)
            SHARED_LIB    := $(GCC_LINUX_SHARED_LIB)
            PLATFORM_DIR  := linux
        else ifeq ($(PLATFORM),win)
            CC            := $(GCC_COMPILER_EXE)
            AR            := $(GCC_ARCHIVER_EXE)
            CFLAGS        := $(GCC_WIN_CFLAGS) $(GCC_WIN_DEFINE)
            LDFLAGS       := $(GCC_WIN_LDFLAGS) $(GCC_WIN_LIB_DIRS)
            LDLIBS        := $(GCC_WIN_LDLIBS)
            STATIC_LIB    := $(GCC_WIN_STATIC_LIB)
            SHARED_LIB    := $(GCC_WIN_SHARED_LIB)
            PLATFORM_DIR  := win
        else
            $(error invalid platform for gcc: $(PLATFORM))
        endif
    else
        $(error invalid compiler: $(COMPILER). use ac5, ac6, or gcc)
    endif
endif

INCLUDE_DIRS              := -I$(PROJECT_DIR)/inc
CFLAGS                    += $(INCLUDE_DIRS)

OUTPUT_BUILD_DIR          := $(BUILD_DIR)/$(COMPILER)-$(PLATFORM)
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

.PHONY: all tool test clean fmt list help info ac5-mcu ac6-mcu gcc-mcu gcc-linux gcc-win

all: $(OUTPUT_STATIC_LIB) $(if $(SHARED_LIB),$(OUTPUT_SHARED_LIB),)
	@$(ECHO_NL)
	@echo "=================================================="
	@echo "static library created successfully: $(OUTPUT_STATIC_LIB)"
ifneq ($(SHARED_LIB),)
	@echo "shared library created successfully: $(OUTPUT_SHARED_LIB)"
endif
	@$(ECHO_NL)
	@echo "make all complete."

tool: $(OUTPUT_STATIC_LIB)
	@$(ECHO_NL)
	@echo "=================================================="
	@echo "building tool programs..."
	@cd tool && "$(MAKE)"
	@$(ECHO_NL)
	@echo "make tool complete."

test: $(OUTPUT_STATIC_LIB)
	@$(ECHO_NL)
	@echo "=================================================="
	@echo "building test programs..."
	@cd test && "$(MAKE)"
	@$(ECHO_NL)
	@echo "make test complete."

clean:
	@$(ECHO_NL)
	@echo "=================================================="
	@echo "cleaning build artifacts..."
	@$(MAKE) -C tool clean
	@$(MAKE) -C test clean
	$(call RMDIR,$(BUILD_DIR))
	$(call RMDIR,$(LIB_DIR))
	@$(ECHO_NL)
	@echo "make clean complete."

ifeq ($(OS),Windows_NT)
    FMT_DIRS_PS           := $(foreach dir,$(FMT_DIRS),'$(dir)')
    FMT_DIRS_PS           := $(subst $(space),'',$(space),$(FMT_DIRS_PS))
    FMT_EXCLUDE_PATTERN   := $(subst $(space),|,$(FMT_EXCLUDE_DIRS))
    FMT_FILES             := $(shell powershell -NoProfile -Command "$$files=@(); $$dirs=@($(FMT_DIRS_PS)); $$excludes='$(FMT_EXCLUDE_PATTERN)'; foreach ($$dir in $$dirs) { if (Test-Path $$dir) { $$files += Get-ChildItem -Path $$dir -Include *.c,*.cpp,*.h,*.hpp -Recurse -File | Where-Object { if ($$excludes) { $$_.FullName -notmatch $$excludes } else { $$true } } | ForEach-Object { $$_.FullName } } }; $$files -join ' '" 2>nul)
    ifeq ($(FMT_FILES),)
        FMT_FILES         := $(foreach dir,$(FMT_DIRS),$(shell cmd /c "for /r $(dir) %%f in (*.c *.cpp *.h *.hpp) do @echo %%~ff" 2>nul))
    endif
else
    FMT_FILES             := $(shell find $(FMT_DIRS) -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) $(foreach dir,$(FMT_EXCLUDE_DIRS),! -path "*/$(dir)/*") 2>/dev/null)
endif

fmt:
	@$(ECHO_NL)
	@echo "=================================================="
	@echo "formatting code files..."
ifeq ($(PLATFORM),win)
	@where clang-format >nul 2>&1 || (echo [ERROR] clang-format not found. please install clang-format. && exit 1)
	@powershell -NoProfile -Command "if (Get-Command dos2unix -ErrorAction SilentlyContinue) { Write-Host 'converting line endings to LF...'; $$files='$(FMT_FILES)'.Split(' '); foreach ($$file in $$files) { if ($$file -and (Test-Path $$file)) { dos2unix \"$$file\" 2>$$null } } }"
	@echo "found $(words $(FMT_FILES)) file(s) to format"
	@$(ECHO_NL)
	@powershell -NoProfile -Command "$$files='$(FMT_FILES)'.Split(' '); foreach ($$file in $$files) { if ($$file -and (Test-Path $$file)) { Write-Host \"formatting: $$file\"; clang-format -i -style=file \"$$file\" 2>$$null; if ($$LASTEXITCODE -ne 0) { clang-format -i \"$$file\" 2>$$null } } }"
	@$(ECHO_NL)
else
	@command -v clang-format >/dev/null 2>&1 || (echo [ERROR] clang-format not found. please install clang-format. && exit 1)
	@if command -v dos2unix >/dev/null 2>&1; then                                 \
		echo "converting line endings to LF...";                                  \
		for file in $(FMT_FILES); do dos2unix "$$file" 2>/dev/null || true; done; \
	fi
	@echo "found $(words $(FMT_FILES)) file(s) to format"
	@$(ECHO_NL)
	@for file in $(FMT_FILES); do                                                                     \
		if [ -n "$$file" ] && [ -f "$$file" ]; then                                                   \
			echo "formatting: $$file";                                                                \
			clang-format -i -style=file "$$file" 2>/dev/null || clang-format -i "$$file" 2>/dev/null; \
		fi;                                                                                           \
	done
	@$(ECHO_NL)
endif
	@echo "make fmt complete."

list:
	@$(ECHO_NL)
	@echo "=================================================="
	@echo "available source files:"
ifeq ($(PLATFORM),win)
	@powershell -NoProfile -Command "$$idx=0; Get-ChildItem -Path '$(SRC_DIR)' -Filter *.c -Recurse -File | Sort-Object FullName | ForEach-Object { $$idx++; Write-Host ('  {0,2}. {1}' -f $$idx, $$_.FullName) }" 2>nul || \
	 cmd /c "setlocal enabledelayedexpansion & set /a idx=0 & for /r $(SRC_DIR) %%f in (*.c) do @set /a idx+=1 & echo   !idx!. %%~ff"
else
	@find $(SRC_DIR) -type f -name "*.c" 2>/dev/null | sort | awk '{printf "  %2d. %s\n", NR, $$0}'
endif

info:
	@$(ECHO_NL)
	@echo "=================================================="
	@echo "build configuration:"
	@echo "PLATFORM: $(PLATFORM)"
	@echo "CC: $(CC)"
	@echo "AR: $(AR)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "SRC_FILES: $(words $(SRC_FILES))"
	@echo "OUTPUT_STATIC_LIB: $(OUTPUT_STATIC_LIB)"
	@echo "OUTPUT_SHARED_LIB: $(OUTPUT_SHARED_LIB)"

help:
	@$(ECHO_NL)
	@echo "=================================================="
	@echo "usage: make [options] [target]"
	@$(ECHO_NL)
	@echo "targets:"
	@echo "  all   - build static and shared libraries (default)"
	@echo "  tool  - build the tool program"
	@echo "  test  - build the test program"
	@echo "  clean - remove build artifacts"
	@echo "  fmt   - format code files using clang-format"
	@echo "  list  - list all available source files"
	@echo "  info  - show build configuration"
	@echo "  help  - show this help message"
	@$(ECHO_NL)
	@echo "options:"
	@echo "  INCLUDE_FILES=<files> - include only these files (space-separated)"
	@echo "  EXCLUDE_FILES=<files> - exclude these files (space-separated)"
	@$(ECHO_NL)
	@echo "examples:"
	@echo "  make           - build with default settings (gcc/linux)"
	@echo "  make ac5-mcu   - build for MCU with ac5"
	@echo "  make ac6-mcu   - build for MCU with ac6"
	@echo "  make gcc-mcu   - build static library for MCU with gcc"
	@echo "  make gcc-linux - build libraries for Linux with gcc"
	@echo "  make gcc-win   - build libraries for Windows with gcc"

ac5-mcu:
	@$(MAKE) COMPILER=ac5 PLATFORM=mcu all

ac6-mcu:
	@$(MAKE) COMPILER=ac6 PLATFORM=mcu all

gcc-mcu:
	@$(MAKE) COMPILER=gcc PLATFORM=mcu all

gcc-linux:
	@$(MAKE) COMPILER=gcc PLATFORM=linux all

gcc-win:
	@$(MAKE) COMPILER=gcc PLATFORM=win all

$(OUTPUT_STATIC_LIB): $(OBJ_FILES) | $(OUTPUT_LIB_DIR)
	@$(ECHO_NL)
	@echo "=================================================="
	@echo "creating static library: $(STATIC_LIB)"
ifeq ($(COMPILER),gcc)
	$(AR) rcs "$@" $(OBJ_FILES)
else
	$(AR) -r "$@" $(OBJ_FILES)
endif

$(OUTPUT_SHARED_LIB): $(OBJ_FILES) | $(OUTPUT_LIB_DIR)
	@$(ECHO_NL)
	@echo "=================================================="
	@echo "creating shared library: $(SHARED_LIB)"
ifeq ($(COMPILER),gcc)
ifeq ($(PLATFORM),linux)
	$(CC) $(LDFLAGS) -o "$@" $(OBJ_FILES) $(LDLIBS)
else ifeq ($(PLATFORM),win)
	$(CC) $(LDFLAGS) -Wl,--out-implib,"$(OUTPUT_LIB_DIR)/$(PROJECT_NAME).lib" -o "$@" $(OBJ_FILES) $(LDLIBS)
else
	$(error shared library not supported for platform: $(PLATFORM))
endif
else
	$(error shared library only supported with gcc compiler)
endif

$(OUTPUT_BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(OUTPUT_BUILD_DIR)
	@$(ECHO_NL)
	@echo "[COMPILE] $<"
	$(call MKDIR,$(dir $@))
ifeq ($(COMPILER),gcc)
	$(CC) $(CFLAGS) -o "$@" "$<"
else
	$(CC) $(CFLAGS) -o"$@" "$<"
endif

$(OUTPUT_BUILD_DIR):
	$(call MKDIR,$(OUTPUT_BUILD_DIR))

$(OUTPUT_LIB_DIR):
	$(call MKDIR,$(OUTPUT_LIB_DIR))
