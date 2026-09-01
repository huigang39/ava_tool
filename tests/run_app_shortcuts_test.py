"""Focused app-command routing regression against the vendored ImGui input layer."""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/app-shortcuts-test"
OUT.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
if not compiler:
    raise SystemExit("Set CXX to a GCC/Clang C++ compiler")
exe = OUT / ("app_shortcuts_test.exe" if os.name == "nt" else "app_shortcuts_test")
subprocess.run([compiler, "-std=c++17", "-O0", "-Isrc", "-Ithirdparty/imgui", "tests/app_shortcuts_test.cpp",
                *[f"thirdparty/imgui/{name}.cpp" for name in ("imgui", "imgui_draw", "imgui_tables", "imgui_widgets")],
                "-o", str(exe)], cwd=ROOT, check=True)
subprocess.run([str(exe)], cwd=OUT, check=True, timeout=30)
