"""Exercise log selection, clipboard, colors and scrolling without hardware or a visible window."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / "build/log-view-check"
out.mkdir(parents=True, exist_ok=True)
vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                              "installationPath"], text=True).strip()
commands = ["@echo off", 'call "' + str(Path(vs) / "VC/Auxiliary/Build/vcvarsall.bat") + '" x64 >nul',
            "if errorlevel 1 exit /b 1"]
objects = []
for source in ["tests/log_view_test.cpp", *["thirdparty/imgui/" + name + ".cpp" for name in
               ("imgui", "imgui_draw", "imgui_tables", "imgui_widgets")]]:
    obj = out / (Path(source).stem + ".obj")
    objects.append(str(obj.relative_to(root)))
    commands += [subprocess.list2cmdline(["cl.exe", "/nologo", "/c", "/std:c++20", "/utf-8", "/EHsc", "/MD", "/O2",
                 "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS", "/Isrc", "/Ithirdparty/imgui", "/Fo" + str(obj.relative_to(root)), source]),
                 "if errorlevel 1 exit /b 1"]
exe = out / "log_view_test.exe"
commands += [subprocess.list2cmdline(["link.exe", "/nologo", "/OUT:" + str(exe.relative_to(root)), *objects,
                                     "user32.lib", "imm32.lib", "gdi32.lib", "dwmapi.lib"]),
             "exit /b %errorlevel%"]
batch = out / "build.cmd"
batch.write_text("\n".join(commands) + "\n", encoding="utf-8")
subprocess.run(["cmd.exe", "/d", "/c", str(batch.relative_to(root))], cwd=root, check=True)
subprocess.run([str(exe)], cwd=out, check=True, timeout=30)
