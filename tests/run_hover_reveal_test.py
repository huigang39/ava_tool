"""Pure hover interaction state regression; no GUI, hardware or user profile."""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/hover-reveal-test'
OUT.mkdir(parents=True, exist_ok=True)
exe = OUT / 'test.exe'
subprocess.run([shutil.which('g++') or 'g++', '-std=c++20', '-O0', '-Isrc',
                'tests/hover_reveal_test.cpp', '-o', str(exe)], cwd=ROOT, check=True)
subprocess.run([str(exe)], cwd=OUT, check=True, timeout=30)
