"""Test both vendored toast renderers with only ImGui's default text font."""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/notification-icons-test"
OUT.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
if not compiler:
    raise SystemExit("Set CXX to a GCC/Clang C++ compiler")
for variant in ("win32", "unix"):
    exe = OUT / (f"notification_icons_{variant}" + (".exe" if os.name == "nt" else ""))
    subprocess.run([compiler, "-std=c++17", "-O0", "-DIMGUI_ENABLE_TEST_ENGINE", "-DNOTIFY_RENDER_OUTSIDE_MAIN_WINDOW=0",
                    "-Isrc", "-Ithirdparty/imgui", f"-Ithirdparty/ImGuiNotify/{variant}/backends",
                    f"-Ithirdparty/ImGuiNotify/{variant}/fonts", "tests/notification_icons_test.cpp",
                    *[f"thirdparty/imgui/{name}.cpp" for name in ("imgui", "imgui_draw", "imgui_tables", "imgui_widgets")],
                    "-o", str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe)], cwd=OUT, check=True, timeout=30)
