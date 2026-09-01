"""Headless navigation and undo routing tests; no hardware or user profile."""
from pathlib import Path
import shutil
import subprocess
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/interaction-history-test'
OUT.mkdir(parents=True, exist_ok=True)
exe = OUT / 'test.exe'
subprocess.run([shutil.which('g++') or 'g++', '-std=c++20', '-O0', '-Isrc', '-Ithirdparty/imgui',
                'tests/interaction_history_test.cpp',
                *[f'thirdparty/imgui/{name}.cpp' for name in ('imgui', 'imgui_draw', 'imgui_tables', 'imgui_widgets')],
                '-o', str(exe)], cwd=ROOT, check=True)
subprocess.run([str(exe)], cwd=OUT, check=True, timeout=30)
