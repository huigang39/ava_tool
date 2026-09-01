"""Compile standalone parameter editor checks; never build or connect the app."""
import os
from pathlib import Path
import subprocess

project = Path(__file__).resolve().parents[1]
output = project / "build" / "parameter-editor-check"
output.mkdir(parents=True, exist_ok=True)
vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                              "installationPath"], text=True).strip()
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvarsall.bat"
commands = ["@echo off", f'call "{vcvars}" x64 >nul', "if errorlevel 1 exit /b 1"]
for name in ("parameter_composition_test", "parameter_editor_perf_test"):
    arguments = ["/nologo", "/std:c++20", "/EHsc", "/O2", "/MD", "/utf-8",
                 "/D_CRT_SECURE_NO_WARNINGS", "/Isrc", "/Ithirdparty/cJSON",
                 f"tests/{name}.cpp", "thirdparty/cJSON/cJSON.c",
                 f"/Fo{output.as_posix()}/", f"/Fe{(output / name).as_posix()}.exe"]
    response = output / f"{name}.rsp"
    response.write_text(subprocess.list2cmdline(arguments), encoding="utf-8")
    commands += [f'cl.exe @"{response}"', "if errorlevel 1 exit /b 1"]
commands += ["exit /b 0"]
batch = output / "compile.cmd"
batch.write_text("\n".join(commands) + "\n", encoding="utf-8")
subprocess.run(["cmd.exe", "/d", "/c", str(batch.relative_to(project))], cwd=project, check=True)
for name in ("parameter_composition_test", "parameter_editor_perf_test"):
    subprocess.run([str(output / f"{name}.exe")], cwd=project, check=True, timeout=60)
