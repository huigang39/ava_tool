"""Build and run the headless variable editor regression on Windows/MSVC.

No probe connection or device writes are performed. Runtime DLLs come from the
independent build/rewrite-check/bin/win output directory.

Optional positional arguments: firmware.axf session.ava. For just the session
loader regression, use --session-load [session.ava] [firmware.axf].
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
output = project / 'build' / 'rewrite-check'
output.mkdir(parents=True, exist_ok=True)
log = output / 'test-build.log'
with log.open('w', encoding='utf-8') as stream:
    result = subprocess.run(['make', '-j', '4', '-W', 'src/gui/variable.cpp',
                             'BIN_DIR=build/rewrite-check/bin'], cwd=project,
                            stdout=stream, stderr=subprocess.STDOUT)
if result.returncode:
    print(log.read_text(encoding='utf-8', errors='replace'))
    sys.exit(result.returncode)
build = log.read_text(encoding='utf-8', errors='replace')
if not any(line.startswith('link.exe ') and '/win/ava_tool.exe' in line for line in build.splitlines()):
    build = subprocess.check_output(['make', '-n', '-W', 'src/gui/variable.cpp',
                                     'VCVARS_DONE=1', 'BIN_DIR=build/rewrite-check/bin'],
                                    cwd=project, text=True, errors='replace')
link = next(line for line in build.splitlines()
            if line.startswith('link.exe ') and '/win/ava_tool.exe' in line)
link = link.removeprefix('link.exe ').replace('/SUBSYSTEM:WINDOWS', '/SUBSYSTEM:CONSOLE')
link = link.replace('/win/ava_tool.exe', '/win/variable_rewrite_test.exe')
link = link.replace('build/msvc-win-x86_64/src/main.o', 'build/rewrite-check/variable_rewrite_test.o')
commands = json.loads((project / 'compile_commands.json').read_text(encoding='utf-8'))
entry = next(item for item in commands if item['file'].replace('\\', '/').endswith('gui/variable.cpp'))
arguments = [arg for arg in entry['arguments'] if not arg.startswith('/Fo') and not arg.endswith('variable.cpp')]
arguments += ['/c', '/utf-8', '/D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR']
for name in ('variable_rewrite_test', 'parameter_composition_test'):
    compile_args = arguments + ['/Fobuild/rewrite-check/' + name + '.o', 'tests/' + name + '.cpp']
    (output / (name + '-compile.rsp')).write_text(subprocess.list2cmdline(compile_args), encoding='utf-8')
(output / 'test-link.rsp').write_text(link, encoding='utf-8')
(output / 'composition-link.rsp').write_text(
    '/nologo /SUBSYSTEM:CONSOLE /NODEFAULTLIB:LIBCMT '
    '/OUT:build/rewrite-check/bin/win/parameter_composition_test.exe '
    'build/rewrite-check/parameter_composition_test.o '
    'build/msvc-win-x86_64/thirdparty/cJSON/cJSON.o', encoding='utf-8')
vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
                              'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property',
                              'installationPath'], text=True).strip()
vcvars = Path(vs) / 'VC/Auxiliary/Build/vcvarsall.bat'
batch = output / 'test-build.cmd'
batch.write_text('@echo off\ncall "' + str(vcvars) + '" x64 >nul\n'
                 'if errorlevel 1 exit /b 1\n'
                 'cl.exe @build/rewrite-check/variable_rewrite_test-compile.rsp\n'
                 'if errorlevel 1 exit /b 1\n'
                 'link.exe @build/rewrite-check/test-link.rsp\n'
                 'if errorlevel 1 exit /b 1\n'
                 'cl.exe @build/rewrite-check/parameter_composition_test-compile.rsp\n'
                 'if errorlevel 1 exit /b 1\n'
                 'link.exe @build/rewrite-check/composition-link.rsp\n'
                 'exit /b %errorlevel%\n', encoding='utf-8')
subprocess.run(['cmd.exe', '/d', '/c', str(batch.relative_to(project))], cwd=project, check=True)
exe = output / 'bin/win/variable_rewrite_test.exe'
profiles = Path(os.environ.get('JSON_PROFILE_DIRECTORY', exe.parent / 'parameters')).resolve()
test_environment = os.environ.copy()
if profiles.is_dir():
    test_environment['JSON_PROFILE_DIRECTORY'] = str(profiles)
elif 'JSON_PROFILE_DIRECTORY' in os.environ:
    raise FileNotFoundError(f'Configured JSON_PROFILE_DIRECTORY does not exist: {profiles}')
else:
    test_environment.pop('JSON_PROFILE_DIRECTORY', None)
    print(f'SKIP: optional bundled parameter profiles are not installed: {profiles}', flush=True)
composition_args = [str(profiles)] if (profiles / 'NEO_45H.json').exists() and (profiles / 'NEO_65H.json').exists() else []
subprocess.run([str(exe.with_name('parameter_composition_test.exe')), *composition_args],
               cwd=exe.parent, timeout=60, check=True, env=test_environment)
# Actual firmware tests import/export all 18 profiles and compose each group;
# give this integration path time to finish on slower development machines.
result = subprocess.run([str(exe), *sys.argv[1:]], cwd=exe.parent,
                        timeout=300 if len(sys.argv) > 1 else 60, env=test_environment)
sys.exit(result.returncode)
