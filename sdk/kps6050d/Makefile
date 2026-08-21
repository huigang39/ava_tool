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
PROJECT := kps6050d
SRC := src/kps6050d.c
HDR := inc/kps6050d.h
EXAMPLE_SRCS := $(wildcard example/*.c)
FSA_EXAMPLE_SRC := example/example_fsa_scope_cycle.c
REGULAR_SRCS := $(filter-out $(FSA_EXAMPLE_SRC),$(EXAMPLE_SRCS))

ifeq ($(OS),Windows_NT)
CC := cl.exe
OBJ := build/win/$(PROJECT).obj
LIBRARY := lib/win/$(PROJECT).dll
IMPORT_LIB := lib/win/$(PROJECT).lib
DSOX_LIB := ../dsox2024a/lib/win/dsox2024a.lib
FSAV3_LIB := ../fsav3/lib/win/fsav3.lib
REGULAR_EXES := $(patsubst example/%.c,build/win/%.exe,$(REGULAR_SRCS))
FSA_EXE := build/win/example_fsa_scope_cycle.exe
FSA_OBJ := build/win/fsav3_c.obj
CFLAGS := /nologo /c /std:c11 /W4 /O2 /MD /utf-8 /D_CRT_SECURE_NO_WARNINGS /Iinc /I../dsox2024a/inc /I../fsav3

ifneq ($(wildcard $(FSAV3_LIB)),)
FSA_TARGET := $(FSA_EXE)
else
FSA_TARGET :=
endif

all: $(LIBRARY) $(REGULAR_EXES) $(FSA_TARGET)
	@echo built examples: $(REGULAR_EXES) $(FSA_TARGET)
ifeq ($(wildcard $(FSAV3_LIB)),)
	@echo skipped example_fsa_scope_cycle: missing $(FSAV3_LIB)
endif

$(OBJ): $(SRC) $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) /DKPS6050D_BUILD $(SRC) /Fo:$@

$(LIBRARY): $(OBJ)
	@if not exist "lib\win" mkdir "lib\win"
	link.exe /nologo /DLL /OUT:$@ /IMPLIB:$(IMPORT_LIB) $(OBJ)

build/win/%.obj: example/%.c $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) $< /Fo:$@

build/win/%.exe: build/win/%.obj $(LIBRARY)
	link.exe /nologo /OUT:$@ $< $(IMPORT_LIB) $(if $(wildcard $(DSOX_LIB)),$(DSOX_LIB),)
	@copy /Y "lib\win\$(PROJECT).dll" "build\win\$(PROJECT).dll" >NUL

ifneq ($(wildcard $(FSAV3_LIB)),)
$(FSA_OBJ): ../fsav3/fsav3_c.cpp ../fsav3/fsav3_c.h ../fsav3/fsav3.h
	cl.exe /nologo /c /std:c++20 /W4 /O2 /MD /EHsc /utf-8 /I../fsav3 ../fsav3/fsav3_c.cpp /Fo:$@

$(FSA_EXE): build/win/example_fsa_scope_cycle.obj $(FSA_OBJ) $(LIBRARY)
	link.exe /nologo /OUT:$@ build/win/example_fsa_scope_cycle.obj $(FSA_OBJ) $(IMPORT_LIB) $(DSOX_LIB) $(FSAV3_LIB)
	@copy /Y "lib\win\$(PROJECT).dll" "build\win\$(PROJECT).dll" >NUL
	@copy /Y "..\dsox2024a\lib\win\dsox2024a.dll" "build\win\dsox2024a.dll" >NUL
	@copy /Y "..\fsav3\lib\win\fsav3.dll" "build\win\fsav3.dll" >NUL
endif

example: $(REGULAR_EXES) $(FSA_TARGET)
	@echo built examples: $(REGULAR_EXES) $(FSA_TARGET)
ifeq ($(wildcard $(FSAV3_LIB)),)
	@echo skipped example_fsa_scope_cycle: missing $(FSAV3_LIB)
endif

clean:
	@if exist build\win rd /s /q build\win
	@if exist lib\win rd /s /q lib\win
else
CC := gcc
LIBRARY := lib/linux/lib$(PROJECT).so
EXAMPLES := $(patsubst example/%.c,build/linux/%,$(REGULAR_SRCS))
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -fPIC -Iinc

all: $(LIBRARY) $(EXAMPLES)
	@echo built examples: $(EXAMPLES)

$(LIBRARY): $(SRC) $(HDR)
	@mkdir -p lib/linux
	$(CC) $(CFLAGS) -shared $(SRC) -lpthread -lm -o $@

build/linux/%: example/%.c $(LIBRARY)
	@mkdir -p build/linux
	$(CC) $(CFLAGS) $< -Llib/linux -l$(PROJECT) -Wl,-rpath,'$$ORIGIN/../../lib/linux' -o $@

example: $(EXAMPLES)
	@echo built examples: $(EXAMPLES)

clean:
	rm -rf build lib
endif

.PHONY: all example clean info
info:
	@echo examples=$(EXAMPLE_SRCS)
endif
