"""Run KPS6050D SDK tests with fake serial I/O; never opens hardware ports."""
import os
from pathlib import Path
import subprocess

project = Path(__file__).resolve().parents[1]
output = project / "build/kps6050d-check"
output.mkdir(parents=True, exist_ok=True)
vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvarsall.bat"
exe = output / "kps6050d_table_test.exe"
cmd = ["cl.exe", "/nologo", "/std:c11", "/W4", "/O2", "/MD", "/utf-8", "/D_CRT_SECURE_NO_WARNINGS",
       "/Isdk/kps6050d/inc", "tests/kps6050d_table_test.c", "/Fo" + str(output / "test.obj"), "/Fe" + str(exe)]
batch = output / "build.cmd"
batch.write_text('@echo off\ncall "' + str(vcvars) + '" x64 >nul\nif errorlevel 1 exit /b 1\n' +
                 subprocess.list2cmdline(cmd) + '\nexit /b %errorlevel%\n', encoding="utf-8")
subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=project, check=True)
subprocess.run([str(exe)], cwd=project, check=True, timeout=15)
