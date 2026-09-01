"""生成含 Python 运行时的 Windows x64 neofsa 便携包，并验证异地解压运行。"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PYTHON_VERSION = "3.14.8"
PYTHON_URL = f"https://www.python.org/ftp/python/{PYTHON_VERSION}/python-{PYTHON_VERSION}-embed-amd64.zip"
PYTHON_SHA256 = "a93abe456ab01bd96d7a085b3cdb6566b3063f4241360d114142fbdb07f0a310"
PACKAGE_NAME = "neofsa-force-demo-win-x64"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def crt_directory():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
    candidates = list((Path(vs) / "VC/Redist/MSVC").glob("[0-9]*/x64/Microsoft.VC143.CRT"))
    if not candidates:
        raise FileNotFoundError("找不到 x64 VC Redistributable，请安装 Visual Studio C++ 运行库构建组件")
    return max(candidates, key=lambda p: tuple(int(n) for n in p.parents[1].name.split(".")))


def validate_package(archive):
    # 测试实际 ZIP，而非构建目录：中文/空格路径、无开发工具 PATH、任意当前目录。
    with tempfile.TemporaryDirectory(prefix="neofsa-便携 验证-") as directory:
        relocated = Path(directory)
        with zipfile.ZipFile(archive) as bundle:
            bundle.extractall(relocated)
        root = relocated / PACKAGE_NAME
        manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
        for name, expected in manifest["files"].items():
            if digest(root / name) != expected:
                raise RuntimeError(f"打包文件校验失败：{name}")
        env = os.environ.copy()
        env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
        env["PYTHONHOME"] = str(relocated / "不存在的系统Python")
        env["PYTHONPATH"] = str(relocated / "不存在的用户模块")
        env["PYTHONUTF8"] = "1"
        cmd = str(Path(os.environ["SystemRoot"]) / "System32/cmd.exe")
        subprocess.run([cmd, "/d", "/c", str(root / "run.cmd"), "self-test"],
                       cwd=relocated, env=env, check=True, timeout=45)
        subprocess.run([cmd, "/d", "/c", str(root / "run.cmd"), "--help"],
                       cwd=relocated, env=env, check=True, timeout=10)
        # Windows 名称解析只读系统网卡信息，不枚举 Npcap 或连接硬件。
        subprocess.run([str(root / "runtime/python/python.exe"), "-B", "-c",
                        "from adapter_names import windows_adapters,adapter_label; "
                        "names=windows_adapters(); "
                        "assert all(isinstance(v['name'],str) and v['name'] for v in names.values()); "
                        "print('Windows adapter name lookup passed:', len(names))"],
                       cwd=relocated, env=env, check=True, timeout=10)
        # 只修改解压副本的配置，菜单验证缩短至0.4秒，成品仍为30秒。
        settings = json.loads((root / "settings.json").read_text(encoding="utf-8"))
        settings["common"].update(duration=.4, ramp=.1)
        (root / "settings.json").write_text(json.dumps(settings), encoding="utf-8")
        # 菜单选择模拟器，并验证输入流程、settings 与自动日志。
        subprocess.run([cmd, "/d", "/c", str(root / "run.cmd")], input="1\n1\nstart\n\n",
                       cwd=relocated, env=env, check=True, timeout=40, text=True, encoding="utf-8")
        if not list((root / "logs").glob("*-hand_guiding.csv")):
            raise RuntimeError("菜单运行未产生 CSV 日志")
    print("便携包验证通过：中文/空格路径、干净 PATH、内置 Python、三个 PD demo 和菜单日志。")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-build", action="store_true", help="使用已有 neofsa DLL")
    parser.add_argument("--output", type=Path, default=ROOT / "dist" / (PACKAGE_NAME + ".zip"))
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("此打包脚本需要 Windows x64 和 Visual Studio x64 运行库")
    if not args.skip_build:
        subprocess.run(["make", "-C", "sdk/neofsa"], cwd=ROOT, check=True)
    cache = ROOT / "build/neofsa-package-cache"
    cache.mkdir(parents=True, exist_ok=True)
    python_zip = cache / f"python-{PYTHON_VERSION}-embed-amd64.zip"
    if not python_zip.exists():
        print("下载官方 Python 嵌入式运行时：" + PYTHON_URL)
        urllib.request.urlretrieve(PYTHON_URL, python_zip)
    if digest(python_zip) != PYTHON_SHA256:
        raise RuntimeError("Python 运行时 SHA256 与官方发布值不一致，停止打包；请检查缓存文件")
    crt = crt_directory()
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="package-", dir=cache) as stage:
        package = Path(stage) / PACKAGE_NAME
        native = package / "lib/win"
        python = package / "runtime/python"
        examples = package / "example"
        for directory in (native, python, examples):
            directory.mkdir(parents=True, exist_ok=True)
        for path in sorted((ROOT / "sdk/neofsa/example").glob("*.py")):
            shutil.copy2(path, examples)
        for path in (ROOT / "sdk/neofsa/portable").iterdir():
            if path.is_file():
                shutil.copy2(path, package)
        shutil.copy2(ROOT / "sdk/neofsa/neofsa_c.h", package)
        shutil.copy2(ROOT / "sdk/neofsa/README.md", package / "SDK_README.md")
        for source in (ROOT / "sdk/neofsa/lib/win/neofsa_c.dll",
                       ROOT / "sdk/ava_eth/bin/win/ava_eth_sdk.dll",
                       ROOT / "sdk/ava_eth/bin/win/libwinpthread-1.dll"):
            shutil.copy2(source, native)
        with zipfile.ZipFile(python_zip) as bundle:
            bundle.extractall(python)
        # ._pth 使用相对位置并隔离系统 Python 环境；不启用 site 或用户插件。
        (python / "python314._pth").write_text("python314.zip\n.\n../../example\n", encoding="utf-8")
        for dll in crt.glob("*.dll"):
            shutil.copy2(dll, native)
            if not (python / dll.name).exists():
                shutil.copy2(dll, python)
        (package / "logs").mkdir()
        (package / "THIRD_PARTY.txt").write_text(
            "Python " + PYTHON_VERSION + " Windows x64 embeddable distribution\n" + PYTHON_URL +
            "\nSHA256: " + PYTHON_SHA256 + "\nRelease: https://www.python.org/downloads/release/python-3148/\n" +
            "Python license: runtime/python/LICENSE.txt\n" +
            "Microsoft Visual C++ runtime: app-local Microsoft.VC143.CRT (Visual Studio redistributable).\n" +
            "ava_eth_sdk.dll and libwinpthread-1.dll: copied from sdk/ava_eth/bin/win.\n" +
            "Npcap is NOT included: install separately from https://npcap.com/#download\n",
            encoding="utf-8")
        manifest = {"package": PACKAGE_NAME, "created_utc": datetime.now(timezone.utc).isoformat(),
                    "python_version": PYTHON_VERSION, "python_archive_sha256": PYTHON_SHA256,
                    "files": {p.relative_to(package).as_posix(): digest(p) for p in sorted(package.rglob("*")) if p.is_file()}}
        (package / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        # 同目录临时文件验证后再替换输出，失败不覆盖上一次成功的包。
        with tempfile.NamedTemporaryFile(prefix=PACKAGE_NAME + "-", suffix=".zip", dir=args.output.parent, delete=False) as f:
            pending = Path(f.name)
        try:
            with zipfile.ZipFile(pending, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
                for path in sorted(package.rglob("*")):
                    if path.is_file():
                        bundle.write(path, path.relative_to(package.parent).as_posix())
            validate_package(pending)
            pending.replace(args.output)
        finally:
            pending.unlink(missing_ok=True)
    checksum = digest(args.output)
    args.output.with_suffix(".zip.sha256").write_text(checksum + "  " + args.output.name + "\n", encoding="ascii")
    print(f"生成：{args.output} ({args.output.stat().st_size / 1024 / 1024:.1f} MiB)")
    print("SHA256: " + checksum)


if __name__ == "__main__":
    main()
