"""Headless native ImGui mouse/key regression for the unit conversion hover card."""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/unit-conversion-ui-test"
OUT.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
if not compiler:
    raise SystemExit("Set CXX to a GCC/Clang C++ compiler")
exe = OUT / ("unit_conversion_ui_test.exe" if os.name == "nt" else "unit_conversion_ui_test")
subprocess.run([compiler, "-std=c++17", "-O0", "-DIMGUI_ENABLE_TEST_ENGINE", "-Isrc", "-Ithirdparty/imgui",
                "tests/unit_conversion_ui_test.cpp",
                *[f"thirdparty/imgui/{name}.cpp" for name in ("imgui", "imgui_draw", "imgui_tables", "imgui_widgets")],
                "-o", str(exe)], cwd=ROOT, check=True)
subprocess.run([str(exe)], cwd=OUT, check=True, timeout=30)
