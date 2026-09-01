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
SRC := src/dyn200.c src/dyn200_ports.c
MAIN_SRC := src/dyn200.c
PORTS_SRC := src/dyn200_ports.c
HDR := inc/dyn200.h src/dyn200_ports.h src/dyn200_serial_win32.h
ifeq ($(OS),Windows_NT)
CC := cl.exe
OBJ := build/win/dyn200.obj
PORTS_OBJ := build/win/dyn200_ports.obj
STATIC_OBJ := build/win/dyn200_static.obj
STATIC_PORTS_OBJ := build/win/dyn200_ports_static.obj
LIBRARY := lib/win/dyn200.dll
IMPORT_LIB := lib/win/dyn200.lib
STATIC_LIB := lib/win/dyn200_static.lib
CFLAGS := /nologo /c /std:c11 /W4 /O2 /MT /utf-8 /D_CRT_SECURE_NO_WARNINGS /Iinc
# 串口枚举用 SetupAPI（运行时加载）+ 读设备注册表键（RegQueryValueExA，在
# advapi32 里）。cl 自己驱动链接时会自动带上 advapi32，直接调 link.exe 才要写。
SYSLIBS := advapi32.lib winmm.lib
all: $(LIBRARY) $(STATIC_LIB) build/win/example_read.exe build/win/example_tables.exe build/win/example_bench.exe
$(OBJ): $(MAIN_SRC) $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) /DDYN200_BUILD $(MAIN_SRC) /Fo:$@
$(PORTS_OBJ): $(PORTS_SRC) $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) $(PORTS_SRC) /Fo:$@
$(LIBRARY): $(OBJ) $(PORTS_OBJ)
	@if not exist "lib\win" mkdir "lib\win"
	link.exe /nologo /DLL /OUT:$@ /IMPLIB:$(IMPORT_LIB) $(OBJ) $(PORTS_OBJ) $(SYSLIBS)
$(STATIC_OBJ): $(MAIN_SRC) $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) /DDYN200_STATIC $(MAIN_SRC) /Fo:$@
$(STATIC_PORTS_OBJ): $(PORTS_SRC) $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) /DDYN200_STATIC $(PORTS_SRC) /Fo:$@
$(STATIC_LIB): $(STATIC_OBJ) $(STATIC_PORTS_OBJ)
	@if not exist "lib\win" mkdir "lib\win"
	lib.exe /nologo /OUT:$@ $(STATIC_OBJ) $(STATIC_PORTS_OBJ)
build/win/example_read.obj: example/example_read.c $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) /DDYN200_STATIC $< /Fo:$@
build/win/example_read.exe: build/win/example_read.obj $(STATIC_LIB)
	link.exe /nologo /OUT:$@ $< $(STATIC_LIB) $(SYSLIBS)
build/win/test_protocol.obj: test/test_protocol.c $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) /DDYN200_STATIC $< /Fo:$@
build/win/example_tables.obj: example/example_tables.c $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) /DDYN200_STATIC $< /Fo:$@
build/win/example_tables.exe: build/win/example_tables.obj $(STATIC_LIB)
	link.exe /nologo /OUT:$@ $< $(STATIC_LIB) $(SYSLIBS)
build/win/example_bench.obj: example/example_bench.c $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) /DDYN200_STATIC $< /Fo:$@
build/win/example_bench.exe: build/win/example_bench.obj $(STATIC_LIB)
	link.exe /nologo /OUT:$@ $< $(STATIC_LIB) $(SYSLIBS)
build/win/test_protocol.exe: build/win/test_protocol.obj $(STATIC_LIB)
	link.exe /nologo /OUT:$@ $< $(STATIC_LIB) $(SYSLIBS)
build/win/test_tables.obj: test/test_tables.c $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) /DDYN200_STATIC $< /Fo:$@
build/win/test_tables.exe: build/win/test_tables.obj $(STATIC_LIB)
	link.exe /nologo /OUT:$@ $< $(STATIC_LIB) $(SYSLIBS)
build/win/test_stream.obj: test/test_stream.c $(MAIN_SRC) $(HDR)
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) /DDYN200_STATIC $< /Fo:$@
build/win/test_stream.exe: build/win/test_stream.obj $(STATIC_PORTS_OBJ)
	link.exe /nologo /OUT:$@ $^ $(SYSLIBS)
build/win/test_serial_win32.obj: test/test_serial_win32.c src/dyn200_serial_win32.h
	@if not exist "build\win" mkdir "build\win"
	$(CC) $(CFLAGS) $< /Fo:$@
build/win/test_serial_win32.exe: build/win/test_serial_win32.obj
	link.exe /nologo /OUT:$@ $<
test: build/win/test_protocol.exe build/win/test_tables.exe build/win/test_stream.exe build/win/test_serial_win32.exe
	build\win\test_protocol.exe
	build\win\test_tables.exe
	build\win\test_stream.exe
	build\win\test_serial_win32.exe
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
all: $(LIBRARY) $(STATIC_LIB) build/$(PLATFORM)/example_read build/$(PLATFORM)/example_tables \
	build/$(PLATFORM)/example_bench
$(LIBRARY): $(SRC) $(HDR)
	@mkdir -p lib/$(PLATFORM)
	$(CC) $(CFLAGS) $(SHARED) $(SRC) -lpthread -o $@
$(STATIC_LIB): $(SRC) $(HDR)
	@mkdir -p lib/$(PLATFORM) build/$(PLATFORM)
	$(CC) $(CFLAGS) -DDYN200_STATIC -c $(MAIN_SRC) -o build/$(PLATFORM)/dyn200_static.o
	$(CC) $(CFLAGS) -DDYN200_STATIC -c $(PORTS_SRC) -o build/$(PLATFORM)/dyn200_ports_static.o
	ar rcs $@ build/$(PLATFORM)/dyn200_static.o build/$(PLATFORM)/dyn200_ports_static.o
build/$(PLATFORM)/example_read: example/example_read.c $(STATIC_LIB)
	@mkdir -p build/$(PLATFORM)
	$(CC) $(CFLAGS) -DDYN200_STATIC $< $(STATIC_LIB) -lpthread -o $@
build/$(PLATFORM)/test_protocol: test/test_protocol.c $(STATIC_LIB)
	@mkdir -p build/$(PLATFORM)
	$(CC) $(CFLAGS) -DDYN200_STATIC $< $(STATIC_LIB) -lpthread -o $@
build/$(PLATFORM)/example_tables: example/example_tables.c $(STATIC_LIB)
	@mkdir -p build/$(PLATFORM)
	$(CC) $(CFLAGS) -DDYN200_STATIC $< $(STATIC_LIB) -lpthread -o $@
build/$(PLATFORM)/example_bench: example/example_bench.c $(STATIC_LIB)
	@mkdir -p build/$(PLATFORM)
	$(CC) $(CFLAGS) -DDYN200_STATIC $< $(STATIC_LIB) -lpthread -o $@
build/$(PLATFORM)/test_tables: test/test_tables.c $(STATIC_LIB)
	@mkdir -p build/$(PLATFORM)
	$(CC) $(CFLAGS) -DDYN200_STATIC $< $(STATIC_LIB) -lpthread -o $@
build/$(PLATFORM)/test_stream: test/test_stream.c $(MAIN_SRC) $(PORTS_SRC) $(HDR)
	@mkdir -p build/$(PLATFORM)
	$(CC) $(CFLAGS) -DDYN200_STATIC $< $(PORTS_SRC) -lpthread -o $@
test: build/$(PLATFORM)/test_protocol build/$(PLATFORM)/test_tables build/$(PLATFORM)/test_stream
	./build/$(PLATFORM)/test_protocol
	./build/$(PLATFORM)/test_tables
	./build/$(PLATFORM)/test_stream
clean:
	rm -rf build/$(PLATFORM) lib/$(PLATFORM)
endif
.PHONY: all test clean info
info:
	@echo project=$(PROJECT)
endif
