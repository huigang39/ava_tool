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
OBJ := $(BUILD_DIR)/neofsa_c.obj
DLL := $(LIB_DIR)/neofsa_c.dll

# 使用公开 ava_eth C ABI，运行时动态加载 DLL。
INCLUDES := /I ../ava_eth/include
HEADERS := neofsa_c.h ../ava_eth/include/ava/sdk.h ../ava_eth/include/ava/controller.h ../ava_eth/include/ava/types.h

all: $(DLL)
	@echo built $(DLL)

$(OBJ): neofsa_c.cpp $(HEADERS)
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	cl.exe /nologo /c /std:c++20 /W4 /O2 /MD /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS $(INCLUDES) neofsa_c.cpp /Fo:$@

$(DLL): $(OBJ)
	@if not exist "$(LIB_DIR)" mkdir "$(LIB_DIR)"
	link.exe /nologo /DLL /NOIMPLIB /OUT:$@ \
		/EXPORT:neofsa_read_param /EXPORT:neofsa_write_param \
		/EXPORT:neofsa_read_rt /EXPORT:neofsa_write_rt \
		/EXPORT:neofsa_read_pd_param /EXPORT:neofsa_write_pd_param /EXPORT:neofsa_read_adapter_param /EXPORT:neofsa_last_error /EXPORT:neofsa_shutdown /EXPORT:ava_tool_sdk_shutdown \
		$(OBJ)
	@if exist "$(LIB_DIR)\neofsa_c.exp" del /q "$(LIB_DIR)\neofsa_c.exp"

clean:
	@if exist "$(BUILD_DIR)" rd /s /q "$(BUILD_DIR)"
	@if exist "$(subst /,\,$(DLL))" del /q "$(subst /,\,$(DLL))"
	@if exist "$(LIB_DIR)\neofsa_c.exp" del /q "$(LIB_DIR)\neofsa_c.exp"
endif
