"""Build isolated SDK caller/merge regressions; only a synthetic DLL is invoked."""
import os
from pathlib import Path
import subprocess
import shutil
import tempfile

project = Path(__file__).resolve().parents[1]
output = project / "build" / "sdk-caller-check"
output.mkdir(parents=True, exist_ok=True)
build_output = output.relative_to(project)
vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                             text=True).strip()
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvarsall.bat"
includes = ["src", "src/core", "src/gui", "ckit", "ckit/include", "thirdparty/imgui",
            "thirdparty/imgui/backends", "thirdparty/implot", "thirdparty/ImGuiNotify/win32/backends",
            "thirdparty/ImGuiNotify/win32/fonts", "thirdparty/GLFW/inc", "thirdparty/cJSON", "thirdparty/libffi/include"]
flags = ["/nologo", "/std:c++20", "/utf-8", "/EHsc", "/MD", "/O2", "/Gy", "/DNOMINMAX",
         "/D_CRT_SECURE_NO_WARNINGS", "/D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR", "/experimental:c11atomics"]
flags += ["/I" + path for path in includes]
commands = ["@echo off", 'call "' + str(vcvars) + '" x64 >nul', "if errorlevel 1 exit /b 1"]
for suffix, offset in [("one", "0"), ("two", "100")]:
    command = ["cl.exe", *flags, "/LD", "/DTEST_OFFSET=" + offset, "tests/sdk_loader_fixture.cpp",
               "/Fo" + str(build_output / (suffix + ".obj")), "/link", "/OUT:" + str(build_output / (suffix + ".dll")),
               "/IMPLIB:" + str(build_output / (suffix + ".lib"))]
    commands += [subprocess.list2cmdline(command), "if errorlevel 1 exit /b 1"]
objects = []
for source in ["src/core/sdk_loader.cpp", "src/core/c_header_parser.cpp", "src/gui/sdk_panel.cpp", "tests/sdk_caller_test.cpp",
               "src/core/export_enum.cpp", "src/platform/win/native_dlg.cpp", "thirdparty/cJSON/cJSON.c",
               "thirdparty/imgui/imgui.cpp", "thirdparty/imgui/imgui_draw.cpp", "thirdparty/imgui/imgui_tables.cpp",
               "thirdparty/imgui/imgui_widgets.cpp"]:
    obj = build_output / (Path(source).stem + ".obj")
    objects.append(str(obj))
    commands += [subprocess.list2cmdline(["cl.exe", *flags, "/c", source, "/Fo" + str(obj)]), "if errorlevel 1 exit /b 1"]
exe = build_output / "sdk_caller_test.exe"
commands += [subprocess.list2cmdline(["link.exe", "/nologo", "/OPT:REF", "/SUBSYSTEM:CONSOLE", "/NODEFAULTLIB:LIBCMT",
                                     "/OUT:" + str(exe), *objects, "thirdparty/libffi/lib/win/ffi.lib",
                                     "ckit/lib/ckit/win/ckit.lib", "dbghelp.lib", "ole32.lib", "comdlg32.lib",
                                     "shell32.lib", "user32.lib", "advapi32.lib", "winmm.lib"]), "exit /b %errorlevel%"]
batch = output / "build.cmd"
batch.write_text("\n".join(commands) + "\n", encoding="utf-8")
subprocess.run(["cmd.exe", "/d", "/c", str(batch.relative_to(project))], cwd=project, check=True)
subprocess.run([str(exe), str(output / "one.dll"), str(output / "two.dll")], cwd=project, check=True, timeout=30)

# Exercise the installed layout from a separate Unicode directory, with no source-tree fallback.
with tempfile.TemporaryDirectory(prefix="ava-sdk-快捷-") as relocated:
    relocated = Path(relocated)
    shutil.copy2(exe, relocated / exe.name)
    shutil.copytree(project / "bin/win/sdk", relocated / "sdk")
    subprocess.run([str(relocated / exe.name), "--presets"], cwd=relocated, check=True, timeout=30)
