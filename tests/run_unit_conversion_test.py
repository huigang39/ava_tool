"""Pure unit conversions and cJSON settings round trips; no GUI or hardware."""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/unit-conversion-test"
OUT.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
if not compiler:
    raise SystemExit("Set CXX to a GCC/Clang C++ compiler")
exe = OUT / ("unit_conversion_test.exe" if os.name == "nt" else "unit_conversion_test")
subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-O2", "-Isrc", "-Ithirdparty/cJSON",
                "tests/unit_conversion_test.cpp", "thirdparty/cJSON/cJSON.c", "-o", str(exe)],
               cwd=ROOT, check=True)
subprocess.run([str(exe)], cwd=OUT, check=True, timeout=30)
