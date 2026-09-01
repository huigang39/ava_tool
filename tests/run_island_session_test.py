"""Compile the real application UI regression against existing Windows build objects."""
from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/island-session-test'
OUT.mkdir(parents=True, exist_ok=True)
dry = subprocess.check_output(['make', '-nB', 'VCVARS_DONE=1'], cwd=ROOT, text=True, errors='replace')
compile_line = next(x for x in dry.splitlines() if x.startswith('cl.exe ') and '"src/main.cpp"' in x)
compile_line = compile_line.replace('src/main.cpp', 'tests/island_session_test.cpp').replace('build/msvc-win-x86_64/src/main.o', 'build/island-session-test/test.obj')
link = next(x for x in dry.splitlines() if x.startswith('link.exe ') and '/win/ava_tool.exe' in x)
link = link.replace('/SUBSYSTEM:WINDOWS', '/SUBSYSTEM:CONSOLE').replace('bin/win/ava_tool.exe', 'build/island-session-test/test.exe')
link = link.replace('build/msvc-win-x86_64/src/main.o', 'build/island-session-test/test.obj')
vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
batch = OUT / 'build.cmd'
fixture = 'cl /nologo /LD /O2 /EHsc /utf-8 tests/sdk_loader_fixture.cpp /Fobuild/island-session-test/sdk_fixture.obj /Febuild/island-session-test/sdk_fixture.dll'
batch.write_text('@echo off\ncall "' + str(Path(vs) / 'VC/Auxiliary/Build/vcvarsall.bat') + '" x64 >nul\nif errorlevel 1 exit /b 1\n' + fixture + '\nif errorlevel 1 exit /b 1\n' + compile_line + '\nif errorlevel 1 exit /b 1\n' + link + '\nexit /b %errorlevel%\n', encoding='utf-8')
subprocess.run(['cmd.exe', '/d', '/c', str(batch.relative_to(ROOT))], cwd=ROOT, check=True)
for dll in (ROOT / 'bin/win').glob('*.dll'): shutil.copy2(dll, OUT / dll.name)
# Every test uses an isolated profile so personal sessions/defaults are untouched.
import tempfile
with tempfile.TemporaryDirectory(prefix='workspace-', dir=OUT) as profile:
    shutil.copy2(OUT / 'sdk_fixture.dll', Path(profile) / 'sdk_fixture.dll')
    subprocess.run([str(OUT / 'test.exe')], cwd=profile, check=True, timeout=90)
    for capture in Path(profile).glob('*.ppm'):
        shutil.copy2(capture, OUT / capture.name)
