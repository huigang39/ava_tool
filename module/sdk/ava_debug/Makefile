CC      := gcc
AR      := ar
SHELL   := cmd.exe
CFLAGS  := -std=c11 -Wall -Wextra -O2 -Iinc
SRC_DIR := src
BUILD_DIR := build
LIB_DIR := lib
TARGET  := $(LIB_DIR)/libava_debug.a
EXAMPLE_TARGET := $(BUILD_DIR)/read_var.exe
SOURCES := $(SRC_DIR)/ava_sdk.c \
           $(SRC_DIR)/ava_ecat.c \
           $(SRC_DIR)/elf_parser.c
OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

.PHONY: all example clean

all: $(TARGET) $(EXAMPLE_TARGET)

example: $(EXAMPLE_TARGET)

$(TARGET): $(OBJECTS) | $(LIB_DIR)
	$(AR) rcs $@ $(OBJECTS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(EXAMPLE_TARGET): examples/read_var.c $(TARGET) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(TARGET) -o $@
$(BUILD_DIR) $(LIB_DIR):
	@if not exist "$@" mkdir "$@"

clean:
	@if exist "$(BUILD_DIR)" rd /s /q "$(BUILD_DIR)"
	@if exist "$(subst /,\,$(TARGET))" del /q "$(subst /,\,$(TARGET))"



