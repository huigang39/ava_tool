"""构建并测试真实 neofsa DLL + ava_eth 本地模拟器，不访问硬件。"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import shutil

root = Path(__file__).resolve().parents[1]
output = root / "build/neofsa-check"
output.mkdir(parents=True, exist_ok=True)
subprocess.run(["make", "-C", "sdk/neofsa"], cwd=root, check=True)
vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvarsall.bat"
commands = ["@echo off", 'call "' + str(vcvars) + '" x64 >nul', "if errorlevel 1 exit /b 1"]
objects = []
for source in ["src/core/sdk_loader.cpp", "src/core/c_header_parser.cpp", "tests/neofsa_sdk_test.cpp"]:
    obj = output / (Path(source).stem + ".obj")
    objects.append(str(obj))
    commands += [subprocess.list2cmdline(["cl.exe", "/nologo", "/std:c++20", "/utf-8", "/EHsc", "/MD", "/O2",
        "/D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR", "/DNOMINMAX", "/Isrc", "/Ithirdparty/libffi/include", "/c", source, "/Fo" + str(obj)]),
        "if errorlevel 1 exit /b 1"]
exe = output / "neofsa_sdk_test.exe"
commands += [subprocess.list2cmdline(["link.exe", "/nologo", "/NODEFAULTLIB:LIBCMT", "/OUT:" + str(exe), *objects,
    "thirdparty/libffi/lib/win/ffi.lib"]), "exit /b %errorlevel%"]
batch = output / "build.cmd"
batch.write_text("\n".join(commands) + "\n", encoding="utf-8")
subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=root, check=True)
subprocess.run([str(exe), str(root / "sdk/neofsa/lib/win/neofsa_c.dll")], cwd=root, check=True, timeout=30)
sys.path.insert(0, str(root / "sdk/neofsa/example"))
from force_demo import command
assert command("constant", 0, 5, 1, 0.1, 1, 0, 0, 0, 0, 0, 0.5) == 0
assert command("constant", 2, 5, 1, 0.1, 1, 0, 0, 0, 0, 0, 0.5) == 0.1
assert command("constant", 5, 5, 1, 0.1, 1, 0, 0, 0, 0, 0, 0.5) == 0
assert abs(command("sine", 1.25, 5, 1, 0.1, 1, 0, 0, 0, 0, 0, 0.5) - 0.1) < 1e-8
assert command("impedance", 2, 5, 1, 0, 1, 2, 0.5, 1, 2, 0, 0.5) == -0.5
assert abs(command("impedance", 2, 5, 1, 0, 1, 2, 0.5, -0.1, 0.1, 0, 0.5) - 0.15) < 1e-8
for demo in ["constant_torque", "sine_torque", "impedance_torque", "hand_guiding", "virtual_spring", "virtual_wall"]:
    subprocess.run([sys.executable, str(root / f"sdk/neofsa/example/{demo}.py"),
        "--duration", "0.4", "--ramp", "0.1", "--torque", "0.01"], cwd=root, check=True, timeout=15)
# 验证运行库与依赖都位于插件旁时，可脱离源码目录并处理中文路径。
with tempfile.TemporaryDirectory(prefix="neofsa-中文-") as directory:
    directory = Path(directory)
    shutil.copy2(root / "sdk/neofsa/lib/win/neofsa_c.dll", directory)
    for name in ["ava_eth_sdk.dll", "libwinpthread-1.dll"]:
        shutil.copy2(root / "sdk/ava_eth/bin/win" / name, directory)
    subprocess.run([sys.executable, str(root / "sdk/neofsa/example/constant_torque.py"),
        "--dll", str(directory / "neofsa_c.dll"), "--duration", "0.4", "--ramp", "0.1", "--torque", "0.01"],
        cwd=directory, check=True, timeout=15)
subprocess.run([sys.executable, "-B", str(root / "tests/neofsa_interaction_test.py")], cwd=root, check=True, timeout=15)
print("neofsa demos, human interaction laws and relocated runtime passed")
