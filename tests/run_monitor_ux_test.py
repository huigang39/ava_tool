"""Run focused UI regressions using existing Windows app build objects; no make build or hardware."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
group = parser.add_mutually_exclusive_group()
group.add_argument('--guide', action='store_true', help='Run panel guide context, navigation and persistence regressions')
group.add_argument('--pointers', action='store_true', help='Run pointer inspection and MP4 capture regressions without hardware')
group.add_argument('--focus', action='store_true', help='Run existing variable/monitor hierarchy interaction regression')
group.add_argument('--dock', action='store_true', help='Run existing dock/session and SDK table regression')
group.add_argument('--device', action='store_true', help='Run device manager UI and persistence regression without hardware')
group.add_argument('--units', action='store_true', help='Run monitor curve/table unit hover regression')
group.add_argument('--axes', action='store_true', help='Run existing plot axes, measurement and cursor regressions')
group.add_argument('--rtt', action='store_true', help='Run J-Link connect/reset and RTT regression without hardware')
group.add_argument('--history', action='store_true', help='Run panel undo/redo and navigation regression without hardware')
group.add_argument('--peek', action='store_true', help='Run statistics hover and manual width regression without hardware')
group.add_argument('--values', action='store_true', help='Run variable copy, child display format and exact raw value regression')
group.add_argument('--choices', action='store_true', help='Run segmented choices, symbol search reload and browser layout regression')
group.add_argument('--server', action='store_true', help='Run fixed-size server settings and tab switching regression')
args = parser.parse_args()
name = ('tutorial_guide_test' if args.guide else 'server_settings_ui_test' if args.server else 'choice_browser_test' if args.choices else 'watch_copy_format_test' if args.values else 'monitor_statistics_peek_test' if args.peek else 'panel_history_test' if args.history else 'rtt_reconnect_test' if args.rtt else 'variable_focus_test' if args.focus else 'dock_session_test' if args.dock else
        'device_manager_ui_test' if args.device else 'monitor_unit_hover_test' if args.units else
        'plot_axes_test' if args.axes else 'monitor_layout_test')
if args.pointers:
    name = 'pointer_capture_test'
OUT = ROOT / 'build' / ('ux-' + name)
OUT.mkdir(parents=True, exist_ok=True)
dry = subprocess.check_output(['make', '-nB', 'VCVARS_DONE=1'], cwd=ROOT, text=True, errors='replace')
obj = (OUT / 'test.obj').relative_to(ROOT).as_posix()
exe = (OUT / 'test.exe').relative_to(ROOT).as_posix()
compile_line = next(x for x in dry.splitlines() if x.startswith('cl.exe ') and '"src/main.cpp"' in x)
compile_line = compile_line.replace('src/main.cpp', f'tests/{name}.cpp').replace('build/msvc-win-x86_64/src/main.o', obj)
link = next(x for x in dry.splitlines() if x.startswith('link.exe ') and '/win/ava_tool.exe' in x)
link = link.replace('/SUBSYSTEM:WINDOWS', '/SUBSYSTEM:CONSOLE').replace('bin/win/ava_tool.exe', exe)
link = link.replace('build/msvc-win-x86_64/src/main.o', obj)
if args.rtt:
    link = link.replace('"build/msvc-win-x86_64/src/core/jlink_port.o"', '').replace('build/msvc-win-x86_64/src/core/jlink_port.o', '')
vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
batch = OUT / 'build.cmd'
batch.write_text('@echo off\ncall "' + str(Path(vs) / 'VC/Auxiliary/Build/vcvarsall.bat') + '" x64 >nul\nif errorlevel 1 exit /b 1\n' + compile_line + '\nif errorlevel 1 exit /b 1\n' + link + '\nexit /b %errorlevel%\n', encoding='utf-8')
subprocess.run(['cmd.exe', '/d', '/c', str(batch.relative_to(ROOT))], cwd=ROOT, check=True)
if args.rtt:
    (OUT / 'config').mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'config/STM32H7_LiveAttach.JLinkScript', OUT / 'config/STM32H7_LiveAttach.JLinkScript')
for dll in (ROOT / 'bin/win').glob('*.dll'):
    shutil.copy2(dll, OUT / dll.name)
if args.dock:
    shutil.copytree(ROOT / 'bin/win/sdk/dyn200', OUT / 'sdk/dyn200', dirs_exist_ok=True)
with tempfile.TemporaryDirectory(prefix='profile-', dir=OUT) as profile:
    command = [str(OUT / 'test.exe')]
    if args.dock:
        fixture = Path(profile) / 'sdk-caller-check'
        fixture.mkdir()
        shutil.copy2(ROOT / 'build/sdk-caller-check/one.dll', fixture / 'one.dll')
        command.append(str(Path(profile) / 'session.ava'))
    elif args.axes:
        command.append(str(Path(profile) / 'session.ava'))
    result = subprocess.run(command, cwd=profile, text=True, errors='replace',
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
    (OUT / 'test.log').write_text(result.stdout, encoding='utf-8')
    print(result.stdout, end='')
    result.check_returncode()
    if args.dock:
        for capture in Path(profile).glob('*.ppm'):
            shutil.copy2(capture, OUT / capture.name)
