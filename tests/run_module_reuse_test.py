"""验证共用参数记录、半精度转换和 C/C++ 内存池, 不连接设备。"""

import os
from pathlib import Path
import shutil
import subprocess


def main():
    project = Path(__file__).resolve().parents[1]
    output = project / "build/module-reuse/test"
    output.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        subprocess.run(
            ["make", "-C", "ckit", "static", "COMPILER=msvc", "-j4"],
            cwd=project,
            check=True,
            stdout=subprocess.DEVNULL,
        )
        vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / (
            "Microsoft Visual Studio/Installer/vswhere.exe"
        )
        installation = subprocess.check_output(
            [str(vswhere), "-latest", "-products", "*", "-requires",
             "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
            text=True,
        ).strip()
        vcvars = Path(installation) / "VC/Auxiliary/Build/vcvarsall.bat"
        commands = [
            "@echo off",
            f'call "{vcvars}" x64 >nul',
            "if errorlevel 1 exit /b 1",
            'cl.exe /nologo /TC /std:clatest /experimental:c11atomics /utf-8 /MD /O2 '
            '/Ickit/include /c tests/module_reuse_fixture.c '
            '/Fobuild/module-reuse/test/fixture.obj',
            "if errorlevel 1 exit /b 1",
            'cl.exe /nologo /std:c++20 /experimental:c11atomics /utf-8 /MD /O2 /EHsc /DNOMINMAX '
            '/Isrc /Ickit/include tests/module_reuse_test.cpp src/core/flash_array.cpp '
            'build/module-reuse/test/fixture.obj ckit/lib/ckit/win/ckit.lib '
            '/Fobuild/module-reuse/test/ /Febuild/module-reuse/test/module_reuse_test.exe',
            "exit /b %errorlevel%",
        ]
        batch = output / "compile.cmd"
        batch.write_text("\n".join(commands) + "\n", encoding="utf-8")
        subprocess.run(["cmd.exe", "/d", "/c", str(batch.relative_to(project))], cwd=project, check=True)
        executable = output / "module_reuse_test.exe"
    else:
        sources = ("param_list", "mempool", "list", "mpsc", "log", "shm", "fixture")
        for name in sources:
            source = "tests/module_reuse_fixture.c" if name == "fixture" else f"ckit/src/{name}.c"
            subprocess.run(
                [shutil.which("cc") or "gcc", "-std=gnu11", "-O2", "-Ickit/include", "-c", source,
                 "-o", str(output / f"{name}.o")],
                cwd=project,
                check=True,
            )
        executable = output / "module_reuse_test"
        subprocess.run(
            [shutil.which("c++") or "g++", "-std=c++20", "-O2", "-Isrc", "-Ickit/include",
             "tests/module_reuse_test.cpp", "src/core/flash_array.cpp",
             *(str(output / f"{name}.o") for name in sources), "-pthread",
             "-o", str(executable)],
            cwd=project,
            check=True,
        )
    subprocess.run([str(executable)], cwd=project, check=True, timeout=60)


if __name__ == "__main__":
    main()
