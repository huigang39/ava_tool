MODULE_DIR            := ..
LIB_DIR               := $(MODULE_DIR)/lib/module
BUILD_DIR             := build
SRC_DIR               := .
FSA_RT_LOG_DIR        := fsa_rt_log

ifeq ($(OS),Windows_NT)
    PLATFORM          := win
    EXE_SUFFIX        := .exe
    SHELL             := cmd.exe
    ECHO_NL           := echo.
define MKDIR
	@if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))" 2>nul
endef
define RMDIR
	@if exist "$(subst /,\,$(1))" rd /s /q "$(subst /,\,$(1))" 2>nul
endef
else
    UNAME_S           := $(shell uname -s)
    UNAME_M           := $(shell uname -m)
    ARCH              := $(UNAME_M)
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
CFLAGS                := -Wall -Wextra -O2 -std=gnu11
LDFLAGS               := -lpthread -lm

ifeq ($(PLATFORM),win)
    OUTPUT_STATIC_LIB := $(LIB_DIR)/win-gcc/module.a
    WINDRES           := windres
    WIN_RESOURCE      := $(BUILD_DIR)/fsa_rt_log_resource.o
    LDFLAGS           += -static -static-libgcc -s -lkernel32 -lws2_32
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
    LDFLAGS           := -static -s -lpthread -lm -luring
endif

CFLAGS                += $(INCLUDE_DIRS)

EXCLUDE_FILES         := jlink_demo.c
SRC_FILES             := $(filter-out $(EXCLUDE_FILES),$(wildcard *.c))
TARGETS               := $(SRC_FILES:.c=) fsa_rt_log
BINS                  := $(addsuffix $(EXE_SUFFIX),$(addprefix $(BUILD_DIR)/,$(TARGETS)))

FSA_RMA_SRC_FILES     := $(FSA_RT_LOG_DIR)/fsa_rma/fsa_rmaio.c

.PHONY: all clean $(TARGETS) help version package module-static

all: $(BINS)
	@$(ECHO_NL)

	@echo [tool] programs built successfully.
	@$(ECHO_NL)
	@echo [tool] make all complete.

clean:
	@$(ECHO_NL)

	@echo [tool] cleaning build artifacts...
	$(call RMDIR,$(BUILD_DIR))
	@$(ECHO_NL)
	@echo [tool] make clean complete.

help:
	@$(ECHO_NL)

	@echo [tool] usage: make [options] [target]
	@$(ECHO_NL)
	@echo targets:
	@echo   all   - compile all .c files
	@echo   package - build and package fsa_rt_log
	@echo   clean - clean all generated files
	@echo   help  - show this help message
	@$(ECHO_NL)
	@echo platform: $(PLATFORM)
	@echo found .c files: $(SRC_FILES)
	@echo fsa_rma .c files: $(if $(FSA_RMA_SRC_FILES),$(FSA_RMA_SRC_FILES),none)
	@echo targets: $(TARGETS)
	@echo library: $(OUTPUT_STATIC_LIB)

version: $(BUILD_DIR)/fsa_rt_log$(EXE_SUFFIX)
	@"$(BUILD_DIR)/fsa_rt_log$(EXE_SUFFIX)" --version

package: $(BUILD_DIR)/fsa_rt_log$(EXE_SUFFIX)
ifeq ($(PLATFORM),win)
	@echo [tool] package is currently supported on Linux and macOS only.
else
	@version=`"$(BUILD_DIR)/fsa_rt_log$(EXE_SUFFIX)" --version | awk '{print $$2}'`; \
	package_name="fsa_rt_log-$$version-$(PLATFORM)-$(ARCH)"; \
	rm -rf "$(BUILD_DIR)/$$package_name"; \
	mkdir -p "$(BUILD_DIR)/$$package_name"; \
	cp "$(BUILD_DIR)/fsa_rt_log$(EXE_SUFFIX)" "$(BUILD_DIR)/$$package_name/"; \
	cp "$(FSA_RT_LOG_DIR)/README.md" "$(BUILD_DIR)/$$package_name/"; \
	tar -czf "$(BUILD_DIR)/$$package_name.tar.gz" -C "$(BUILD_DIR)" "$$package_name"; \
	rm -rf "$(BUILD_DIR)/$$package_name"; \
	echo "[tool] package created: $(BUILD_DIR)/$$package_name.tar.gz"
endif

$(BUILD_DIR)/fsa_rt_log$(EXE_SUFFIX): $(FSA_RT_LOG_DIR)/main.c $(FSA_RT_LOG_DIR)/version.h $(FSA_RMA_SRC_FILES) $(OUTPUT_STATIC_LIB) $(WIN_RESOURCE) module-static | $(BUILD_DIR)
	@$(ECHO_NL)
	@echo [tool][CC] $<$(if $(FSA_RMA_SRC_FILES), with fsa_rma sources,)
	$(CC) $(CFLAGS) $< $(FSA_RMA_SRC_FILES) $(WIN_RESOURCE) -o "$@" "$(OUTPUT_STATIC_LIB)" $(LDFLAGS)

ifeq ($(PLATFORM),win)
$(WIN_RESOURCE): $(FSA_RT_LOG_DIR)/fsa_rt_log.rc $(FSA_RT_LOG_DIR)/version.h | $(BUILD_DIR)
	$(WINDRES) -i "$<" -o "$@"
endif

$(BUILD_DIR)/%$(EXE_SUFFIX): %.c $(if $(FSA_RMA_SRC_FILES),$(FSA_RMA_SRC_FILES),) $(OUTPUT_STATIC_LIB) module-static | $(BUILD_DIR)
	@$(ECHO_NL)
	@echo [tool][CC] $<$(if $(FSA_RMA_SRC_FILES), with fsa_rma sources,)
	$(CC) $(CFLAGS) $< $(FSA_RMA_SRC_FILES) -o "$@" "$(OUTPUT_STATIC_LIB)" $(LDFLAGS)

$(TARGETS): %: $(BUILD_DIR)/%$(EXE_SUFFIX)

$(BUILD_DIR):
	$(call MKDIR,$(BUILD_DIR))

module-static:
	@$(MAKE) -C "$(MODULE_DIR)" COMPILER=gcc PLATFORM=$(PLATFORM) static

$(OUTPUT_STATIC_LIB): module-static
