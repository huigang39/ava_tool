"""Host memory fixture + optional read-only CM7 ELF resolution. No J-Link/hardware IO."""
from pathlib import Path
import os
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
out = project / 'build/motor-jlink-check'
out.mkdir(parents=True, exist_ok=True)
vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
flags = ['/nologo', '/std:c++20', '/utf-8', '/EHsc', '/MD', '/O2', '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS',
         '/D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR', '/experimental:c11atomics']
flags += ['/I' + path for path in ('src', 'ckit', 'ckit/include', 'thirdparty/cJSON')]
sources = ('src/core/motor_jlink.cpp', 'src/core/motor_controller.cpp', 'src/core/elf_parser.cpp', 'src/core/dwarf_parser.cpp', 'tests/motor_jlink_test.cpp')
commands = ['@echo off', f'call "{vs}/VC/Auxiliary/Build/vcvarsall.bat" x64 >nul', 'if errorlevel 1 exit /b 1']
objects = []
for source in sources:
    obj = out / (Path(source).stem + '.obj')
    objects.append(str(obj))
    commands += [subprocess.list2cmdline(['cl.exe', *flags, '/c', source, '/Fo' + str(obj)]), 'if errorlevel 1 exit /b 1']
exe = out / 'motor_jlink_test.exe'
commands += [subprocess.list2cmdline(['link.exe', '/nologo', '/OUT:' + str(exe), *objects]), 'exit /b %errorlevel%']
batch = out / 'build.cmd'
batch.write_text('\n'.join(commands) + '\n', encoding='utf-8')
subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=project, check=True)
subprocess.run([str(exe), *sys.argv[1:]], cwd=project, check=True, timeout=30)
