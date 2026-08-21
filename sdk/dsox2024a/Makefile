ifeq ($(OS),Windows_NT)
SHELL := cmd.exe
ifndef VCVARS_DONE
_NEED_MSVC := $(if $(MAKECMDGOALS),$(filter-out clean info,$(MAKECMDGOALS)),all)
ifneq ($(_NEED_MSVC),)
ifeq ($(shell where cl.exe 2>NUL),)
_PFILES86 := $(strip $(shell powershell -NoProfile -Command "[Environment]::GetFolderPath('ProgramFilesX86')" 2>NUL))
_VSWHERE := $(_PFILES86)\Microsoft Visual Studio\Installer\vswhere.exe
_VS := $(strip $(shell "$(_VSWHERE)" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>NUL))
_VCVARS := $(_VS)\VC\Auxiliary\Build\vcvarsall.bat
_GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)
_MSVC_REINVOKE := 1
.PHONY: _msvc_env $(_GOALS)
_msvc_env:
	@call "$(_VCVARS)" x64 >NUL 2>&1 && $(MAKE) $(_GOALS) VCVARS_DONE=1 --no-print-directory
$(_GOALS): _msvc_env
	@:
.DEFAULT_GOAL := all
endif
endif
endif
endif

ifndef _MSVC_REINVOKE
PROJECT := dsox2024a
SRC := src/dsox2024a.c
HDR := inc/dsox2024a.h
EXAMPLE_SRCS := $(wildcard example/*.c)
EXAMPLE_NAMES := $(basename $(notdir $(EXAMPLE_SRCS)))

ifeq ($(OS),Windows_NT)
PLATFORM := win
CC := cl.exe
OBJ := build/$(PLATFORM)/$(PROJECT).obj
LIBRARY := lib/$(PLATFORM)/$(PROJECT).dll
IMPORT_LIB := lib/$(PLATFORM)/$(PROJECT).lib
EXAMPLES := $(addprefix build/$(PLATFORM)/,$(addsuffix .exe,$(EXAMPLE_NAMES)))
CFLAGS := /nologo /c /std:c11 /W4 /wd4152 /wd4701 /O2 /MD /utf-8 /D_CRT_SECURE_NO_WARNINGS /Iinc
SDK_CFLAGS := $(CFLAGS) /DDSOX2024A_BUILD

all: $(LIBRARY) $(EXAMPLES)

$(OBJ): $(SRC) $(HDR)
	@if not exist "build\$(PLATFORM)" mkdir "build\$(PLATFORM)"
	$(CC) $(SDK_CFLAGS) $(SRC) /Fo:$@

$(LIBRARY): $(OBJ)
	@if not exist "lib\$(PLATFORM)" mkdir "lib\$(PLATFORM)"
	link.exe /nologo /DLL /OUT:$@ /IMPLIB:$(IMPORT_LIB) $(OBJ) ws2_32.lib

build/$(PLATFORM)/%.obj: example/%.c $(HDR)
	@if not exist "build\$(PLATFORM)" mkdir "build\$(PLATFORM)"
	$(CC) $(CFLAGS) $< /Fo:$@

build/$(PLATFORM)/%.exe: build/$(PLATFORM)/%.obj $(LIBRARY)
	link.exe /nologo /OUT:$@ $< $(IMPORT_LIB)
	@copy /Y "lib\\$(PLATFORM)\\$(PROJECT).dll" "build\\$(PLATFORM)\\$(PROJECT).dll" >NUL

clean:
	@if exist build\win rd /s /q build\win
	@if exist lib\win rd /s /q lib\win
else
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
PLATFORM := mac
CC := clang
SHARED_FLAGS := -dynamiclib
LIBRARY := lib/$(PLATFORM)/lib$(PROJECT).dylib
RPATH := @loader_path/../../lib/$(PLATFORM)
LDLIBS := -lpthread -lm
else
PLATFORM := linux
CC := gcc
SHARED_FLAGS := -shared
LIBRARY := lib/$(PLATFORM)/lib$(PROJECT).so
RPATH := $$$$ORIGIN/../../lib/$(PLATFORM)
LDLIBS := -lpthread -ldl -lm
endif
OBJ := build/$(PLATFORM)/$(PROJECT).o
LIBRARY := lib/$(PLATFORM)/lib$(PROJECT)$(if $(filter Darwin,$(UNAME_S)),.dylib,.so)
EXAMPLES := $(addprefix build/$(PLATFORM)/,$(EXAMPLE_NAMES))
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -fPIC -Iinc

all: $(LIBRARY) $(EXAMPLES)

$(OBJ): $(SRC) $(HDR)
	@mkdir -p build/$(PLATFORM)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBRARY): $(OBJ)
	@mkdir -p lib/$(PLATFORM)
	$(CC) $(SHARED_FLAGS) $< $(LDLIBS) -o $@

build/$(PLATFORM)/%.o: example/%.c $(HDR)
	@mkdir -p build/$(PLATFORM)
	$(CC) $(CFLAGS) -c $< -o $@

build/$(PLATFORM)/%: build/$(PLATFORM)/%.o $(LIBRARY)
	$(CC) $< -Llib/$(PLATFORM) -l$(PROJECT) -Wl,-rpath,$(RPATH) -o $@

clean:
	rm -rf build/$(PLATFORM) lib/$(PLATFORM)
endif

.PHONY: all example clean info
example: $(EXAMPLES)
	@echo built examples: $(EXAMPLES)

info:
	@echo platform=$(PLATFORM) compiler=$(CC)
	@echo examples=$(EXAMPLE_SRCS)
endif


