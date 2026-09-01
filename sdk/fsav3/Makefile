ifeq ($(OS),Windows_NT)
SHELL := cmd.exe
ifndef VCVARS_DONE
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

ifndef _MSVC_REINVOKE
.PHONY: all clean

BUILD_DIR := build/win
LIB_DIR := lib/win
OBJ := $(BUILD_DIR)/fsav3_c.obj
DLL := $(LIB_DIR)/fsav3_c.dll
VENDOR_LIB := $(LIB_DIR)/fsav3.lib

all: $(DLL)
	@echo built $(DLL)

$(OBJ): fsav3_c.cpp fsav3_c.h fsav3.h
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	cl.exe /nologo /c /std:c++20 /W4 /O2 /MD /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS fsav3_c.cpp /Fo:$@

$(DLL): $(OBJ) $(VENDOR_LIB)
	@if not exist "$(LIB_DIR)" mkdir "$(LIB_DIR)"
	link.exe /nologo /DLL /NOIMPLIB /OUT:$@ \
		/EXPORT:fsav3_c_read_error3 /EXPORT:fsav3_read_param /EXPORT:fsav3_read_rt /EXPORT:fsav3_write_param \
		/EXPORT:fsav3_write_rt \
		$(OBJ) $(VENDOR_LIB)
	@if exist "$(LIB_DIR)\fsav3_c.exp" del /q "$(LIB_DIR)\fsav3_c.exp"

clean:
	@if exist "$(BUILD_DIR)" rd /s /q "$(BUILD_DIR)"
	@if exist "$(DLL)" del /q "$(DLL)"
	@if exist "$(LIB_DIR)\fsav3_c.exp" del /q "$(LIB_DIR)\fsav3_c.exp"
endif
