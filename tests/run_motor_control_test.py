"""Compile the motor actor and render the real ImGui page using only local simulation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import re

project = Path(__file__).resolve().parents[1]
output = project / "build" / "motor-control-check"
output.mkdir(parents=True, exist_ok=True)
vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                             "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvarsall.bat"
includes = ["src", "src/core", "src/gui", "ckit", "ckit/include", "thirdparty/imgui",
            "thirdparty/imgui/backends", "thirdparty/implot", "thirdparty/ImGuiNotify/win32/backends",
            "thirdparty/ImGuiNotify/win32/fonts", "thirdparty/GLFW/inc", "thirdparty/cJSON", "thirdparty/libffi/include"]
flags = ["/nologo", "/std:c++20", "/utf-8", "/EHsc", "/MD", "/O2", "/Gy", "/DNOMINMAX", "/D_USE_MATH_DEFINES",
         "/D_CRT_SECURE_NO_WARNINGS", "/D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR", "/experimental:c11atomics"]
flags += ["/I" + path for path in includes]
flags += ["/I" + "C:/mujoco/include", "/DAVA_HAS_MUJOCO"]
commands = ["@echo off", f'call "{vcvars}" x64 >nul', "if errorlevel 1 exit /b 1"]
resource = output / "motor-runtime.res"
commands += [subprocess.list2cmdline(["rc.exe", "/nologo", "/fo", str(resource), "assets/app.rc"]), "if errorlevel 1 exit /b 1"]
sources = ["src/core/motor_controller.cpp", "src/gui/motor_control.cpp", "tests/motor_controller_test.cpp", "tests/motor_control_ui_test.cpp", "src/gui/motor_position_dial.cpp", "src/gui/motor_dashboard.cpp", "src/core/motor_jlink.cpp"]
fixture = output / "motor_fault_fixture.dll"
commands += [subprocess.list2cmdline(["cl.exe", *flags, "/LD", "tests/motor_fault_fixture.cpp", "/Fo" + str(output / "motor_fault_fixture.obj"),
             "/link", "/OUT:" + str(fixture), "/IMPLIB:" + str(output / "motor_fault_fixture.lib")]), "if errorlevel 1 exit /b 1"]
objects = {}
for source in sources:
    obj = output / (Path(source).stem + ".obj")
    objects[source] = str(obj)
    commands += [subprocess.list2cmdline(["cl.exe", *flags, "/c", source, "/Fo" + str(obj)]), "if errorlevel 1 exit /b 1"]
actor = output / "motor_controller_test.exe"
commands += [subprocess.list2cmdline(["link.exe", "/nologo", "/OUT:" + str(actor),
             objects[sources[0]], objects[sources[2]], objects["src/core/motor_jlink.cpp"], str(resource)]), "if errorlevel 1 exit /b 1"]
ui = output / "motor_control_ui_test.exe"
dry_run = subprocess.check_output(["make", "-nB", "BIN_DIR=bin/spice-fix", "VCVARS_DONE=1"], cwd=project, text=True, errors="replace")
link_line = next(line for line in dry_run.splitlines() if line.startswith("link.exe ") and "/win/ava_tool.exe" in line)
link_line = link_line.replace("/SUBSYSTEM:WINDOWS", "/SUBSYSTEM:CONSOLE").replace('bin/spice-fix/win/ava_tool.exe',ui.as_posix())
link_line = link_line.replace("build/msvc-win-x86_64/src/main.o", '"' + objects[sources[3]] + '"')
link_line = re.sub(r'/MANIFESTINPUT:"[^"]*"', '', link_line)
link_line = re.sub(r'/MANIFESTUAC:"[^"]*"', '/MANIFESTUAC:"level=\'asInvoker\' uiAccess=\'false\'"', link_line)
commands += [link_line,"exit /b %errorlevel%"]
batch = output / "build.cmd"
batch.write_text("\n".join(commands) + "\n", encoding="utf-8")
subprocess.run(["cmd.exe", "/d", "/c", str(batch.relative_to(project))], cwd=project, check=True)
for runtime in ("glfw3.dll", "libfftw3f-3.dll", "JLink_x64.dll", "mujoco.dll"):
    source = project / "bin/win" / runtime
    if source.exists(): shutil.copy2(source, output / runtime)
library = Path(os.environ.get("AVA_ETH_SDK_LIBRARY", str(project / "sdk/ava_eth/bin/win/ava_eth_sdk.dll")))
subprocess.run([str(actor), str(library), str(fixture)], cwd=project, check=True, timeout=30)
with tempfile.TemporaryDirectory(prefix="ava-motor-") as cache:
    environment = dict(os.environ, LOCALAPPDATA=cache)
    # An empty working directory and cache exercise extraction without loose SDK DLLs.
    subprocess.run([str(actor), "", str(fixture)], cwd=cache, env=environment, check=True, timeout=30)
    extracted = list(Path(cache).rglob("ava_eth_sdk.dll"))
    assert len(extracted) == 1
    for name in ("ava_eth_sdk.dll", "libwinpthread-1.dll"):
        assert (extracted[0].parent / name).read_bytes() == (project / "sdk/ava_eth/bin/win" / name).read_bytes()
    extracted[0].write_bytes(b"damaged cache")
    subprocess.run([str(actor), "", str(fixture)], cwd=cache, env=environment, check=True, timeout=30)
    assert extracted[0].read_bytes() == (project / "sdk/ava_eth/bin/win/ava_eth_sdk.dll").read_bytes()
    print("Bundled runtime: clean extraction, both DLL payloads and damaged-cache repair passed")
subprocess.run([str(ui), "", str(output), str(fixture)], cwd=project, check=True, timeout=60)
try:
    from PIL import Image
    for ppm in output.glob("motor-*.ppm"):
        with Image.open(ppm) as image:
            image.save(ppm.with_suffix(".png"))
except ImportError:
    pass
