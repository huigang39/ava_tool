"""Inject monitor enumeration failures without changing physical displays."""
from pathlib import Path
import os
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / "build/glfw-monitor-check"
out.mkdir(parents=True, exist_ok=True)
subprocess.run(["make", "-j4"], cwd=root, check=True)
dry = subprocess.check_output(["make", "-nB", "VCVARS_DONE=1"], cwd=root, text=True, errors="replace")
compile_line = next(line for line in dry.splitlines() if line.startswith("cl.exe ") and 'src/main.cpp' in line)
compile_line = compile_line.replace('src/main.cpp', 'tests/glfw_monitor_test.cpp').replace('build/msvc-win-x86_64/src/main.o', 'build/glfw-monitor-check/test.obj')
link = next(line for line in dry.splitlines() if line.startswith("link.exe ") and '/win/ava_tool.exe' in line)
link = link.replace('/SUBSYSTEM:WINDOWS', '/SUBSYSTEM:CONSOLE').replace('bin/win/ava_tool.exe', 'build/glfw-monitor-check/test.exe')
link = link.replace('build/msvc-win-x86_64/src/main.o', 'build/glfw-monitor-check/test.obj')
link = link.replace('build/msvc-win-x86_64/thirdparty/imgui/backends/imgui_impl_glfw.o', '')
vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
batch = out / 'build.cmd'
batch.write_text('@echo off\ncall "' + str(Path(vs) / 'VC/Auxiliary/Build/vcvarsall.bat') + '" x64 >nul\nif errorlevel 1 exit /b 1\n' + compile_line + '\nif errorlevel 1 exit /b 1\n' + link + '\nexit /b %errorlevel%\n', encoding='utf-8')
subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=root, check=True)
for dll in (root / 'bin/win').glob('*.dll'):
    shutil.copy2(dll, out / dll.name)
subprocess.run([str(out / 'test.exe')], cwd=out, check=True, timeout=60)
