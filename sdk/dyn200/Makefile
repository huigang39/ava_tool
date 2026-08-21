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
PROJECT := dyn200
SRC := src/dyn200.c
HDR := inc/dyn200.h
ifeq ($(OS),Windows_NT)
CC := cl.exe
OBJ := build/win/dyn200.obj
STATIC_OBJ := build/win/dyn200_static.obj
LIBRARY := lib/win/dyn200.dll
IMPORT_LIB := lib/win/dyn200.lib
STATIC_LIB := lib/win/dyn200_static.lib
CFLAGS := /nologo /c /std:c11 /W4 /O2 /MT /utf-8 /D_CRT_SECURE_NO_WARNINGS /Iinc
all: $(LIBRARY) $(STATIC_LIB) build/win/example_read.exe
$(OBJ): $(SRC) $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) /DDYN200_BUILD $(SRC) /Fo:$@
$(LIBRARY): $(OBJ)
	@if not exist "lib\win" mkdir "lib\win"
	link.exe /nologo /DLL /OUT:$@ /IMPLIB:$(IMPORT_LIB) $(OBJ)
$(STATIC_OBJ): $(SRC) $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) /DDYN200_STATIC $(SRC) /Fo:$@
$(STATIC_LIB): $(STATIC_OBJ)
	@if not exist "lib\win" mkdir "lib\win"
	lib.exe /nologo /OUT:$@ $(STATIC_OBJ)
build/win/example_read.obj: example/example_read.c $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) /DDYN200_STATIC $< /Fo:$@
build/win/example_read.exe: build/win/example_read.obj $(STATIC_LIB)
	link.exe /nologo /OUT:$@ $< $(STATIC_LIB)
build/win/test_protocol.obj: test/test_protocol.c $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) /DDYN200_STATIC $< /Fo:$@
build/win/test_protocol.exe: build/win/test_protocol.obj $(STATIC_LIB)
	link.exe /nologo /OUT:$@ $< $(STATIC_LIB)
test: build/win/test_protocol.exe
	build\win\test_protocol.exe
clean:
	@if exist build\win rd /s /q build\win
	@if exist lib\win rd /s /q lib\win
else
UNAME_S := $(shell uname -s)
PLATFORM := $(if $(filter Darwin,$(UNAME_S)),mac,linux)
CC := $(if $(filter Darwin,$(UNAME_S)),clang,gcc)
EXT := $(if $(filter Darwin,$(UNAME_S)),.dylib,.so)
SHARED := $(if $(filter Darwin,$(UNAME_S)),-dynamiclib,-shared)
LIBRARY := lib/$(PLATFORM)/libdyn200$(EXT)
STATIC_LIB := lib/$(PLATFORM)/libdyn200.a
RPATH := $(if $(filter Darwin,$(UNAME_S)),@loader_path/../../lib/mac,$$$$ORIGIN/../../lib/linux)
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -fPIC -Iinc
all: $(LIBRARY) $(STATIC_LIB) build/$(PLATFORM)/example_read
$(LIBRARY): $(SRC) $(HDR)
	@mkdir -p lib/$(PLATFORM)
	$(CC) $(CFLAGS) $(SHARED) $(SRC) -lpthread -o $@
$(STATIC_LIB): $(SRC) $(HDR)
	@mkdir -p lib/$(PLATFORM) build/$(PLATFORM)
	$(CC) $(CFLAGS) -DDYN200_STATIC -c $(SRC) -o build/$(PLATFORM)/dyn200_static.o
	ar rcs $@ build/$(PLATFORM)/dyn200_static.o
build/$(PLATFORM)/example_read: example/example_read.c $(STATIC_LIB)
	@mkdir -p build/$(PLATFORM)
	$(CC) $(CFLAGS) -DDYN200_STATIC $< $(STATIC_LIB) -lpthread -o $@
build/$(PLATFORM)/test_protocol: test/test_protocol.c $(STATIC_LIB)
	@mkdir -p build/$(PLATFORM)
	$(CC) $(CFLAGS) -DDYN200_STATIC $< $(STATIC_LIB) -lpthread -o $@
test: build/$(PLATFORM)/test_protocol
	./$<
clean:
	rm -rf build/$(PLATFORM) lib/$(PLATFORM)
endif
.PHONY: all test clean info
info:
	@echo project=$(PROJECT)
endif
