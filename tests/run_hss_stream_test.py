"""Replay fragmented HSS frames without connecting a probe."""
import os
from pathlib import Path
import subprocess

project = Path(__file__).resolve().parents[1]
output = project / "build" / "hss-check"
output.mkdir(parents=True, exist_ok=True)
vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                              "installationPath"], text=True).strip()
arguments = ["/nologo", "/std:c++20", "/EHsc", "/O2", "/MD", "/Isrc",
             "tests/hss_stream_test.cpp", f"/Fo{output.as_posix()}/", f"/Fe{output.as_posix()}/hss_stream_test.exe"]
response = output / "test.rsp"
response.write_text(subprocess.list2cmdline(arguments), encoding="utf-8")
batch = output / "compile.cmd"
batch.write_text(f'@echo off\ncall "{vs}/VC/Auxiliary/Build/vcvarsall.bat" x64 >nul\n'
                 f'if errorlevel 1 exit /b 1\ncl.exe @"{response}"\nexit /b %errorlevel%\n', encoding="utf-8")
subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=project, check=True)
subprocess.run([str(output / "hss_stream_test.exe")], cwd=project, check=True)
