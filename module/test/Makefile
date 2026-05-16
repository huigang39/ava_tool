MODULE_DIR            := $(CURDIR)/..
LIB_DIR               := $(MODULE_DIR)/lib/module
BUILD_DIR             := build
SRC_DIR               := .

ifeq ($(OS),Windows_NT)
    PLATFORM          := win
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
INCLUDE_DIRS          := -I"$(MODULE_DIR)"
CFLAGS                := -Wall -Wextra -g
LDFLAGS               := -lpthread -lm

ifeq ($(PLATFORM),win)
    OUTPUT_STATIC_LIB := $(LIB_DIR)/win/module.a
    LDFLAGS           += -lkernel32 -lws2_32
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
BINS                  := $(addprefix $(BUILD_DIR)/,$(TARGETS))

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

$(BUILD_DIR)/%: %.c | $(BUILD_DIR)
	@$(ECHO_NL)
	@echo [test][CC] $<
	$(CC) $(CFLAGS) $< -o "$@" $(OUTPUT_STATIC_LIB) $(LDFLAGS)

$(TARGETS): %: $(BUILD_DIR)/%

$(BUILD_DIR):
	$(call MKDIR,$(BUILD_DIR))
