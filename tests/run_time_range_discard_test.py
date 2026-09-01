"""Run storage-only discard regressions using the application's MSVC build settings.

No GUI window or hardware connection is created. Pass --skip-build when the
application objects in --build-dir have already been rebuilt by another check.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess

project = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--build-dir", type=Path, default=project / "build/focus-check")
parser.add_argument("--skip-build", action="store_true")
args = parser.parse_args()
build = args.build_dir.resolve()
binary = build / "bin"
build.mkdir(parents=True, exist_ok=True)
options = ["BUILD_DIR=" + build.as_posix(), "BIN_DIR=" + binary.as_posix()]
if not args.skip_build:
    subprocess.run(["make", "-j4", *options], cwd=project, check=True)
dry = subprocess.check_output(["make", "-nB", *options, "VCVARS_DONE=1"], cwd=project,
                              text=True, errors="replace")
compile_line = next(line for line in dry.splitlines()
                    if line.startswith("cl.exe ") and '"src/main.cpp"' in line)
test_object = (build / "time_range_discard_test.o").as_posix()
main_object = (build / "msvc-win-x86_64/src/main.o").as_posix()
compile_line = compile_line.removeprefix("cl.exe ").replace(main_object, test_object)
compile_line = compile_line.replace('"src/main.cpp"', '"tests/time_range_discard_test.cpp"')
link_line = next(line for line in dry.splitlines()
                 if line.startswith("link.exe ") and "/win/ava_tool.exe" in line)
link_line = link_line.removeprefix("link.exe ").replace("/SUBSYSTEM:WINDOWS", "/SUBSYSTEM:CONSOLE")
link_line = link_line.replace("/win/ava_tool.exe", "/win/time_range_discard_test.exe")
link_line = link_line.replace(main_object, test_object)
link_line = re.sub(r'/MANIFESTINPUT:"[^"]*"', '', link_line)
link_line = re.sub(r'/MANIFESTUAC:"[^"]*"', '/MANIFESTUAC:"level=\'asInvoker\' uiAccess=\'false\'"', link_line)
compile_rsp = build / "discard-compile.rsp"
link_rsp = build / "discard-link.rsp"
compile_rsp.write_text(compile_line, encoding="utf-8")
link_rsp.write_text(link_line, encoding="utf-8")
vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                              "installationPath"], text=True).strip()
batch = build / "discard-test-build.cmd"
batch.write_text('@echo off\ncall "' + str(Path(vs) / "VC/Auxiliary/Build/vcvarsall.bat") + '" x64 >nul\n'
                 'if errorlevel 1 exit /b 1\ncl.exe @"' + str(compile_rsp) + '"\n'
                 'if errorlevel 1 exit /b 1\nlink.exe @"' + str(link_rsp) + '"\n'
                 'exit /b %errorlevel%\n', encoding="utf-8")
subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=project, check=True)
subprocess.run([str(binary / "win/time_range_discard_test.exe")], cwd=binary / "win", check=True, timeout=45)
