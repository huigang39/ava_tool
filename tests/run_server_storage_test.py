from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
out = root / 'build/server-storage-test'
out.mkdir(parents=True, exist_ok=True)
exe = out / 'test.exe'
subprocess.run([shutil.which('g++') or 'g++', '-std=c++20', '-O0', '-Isrc', '-Ithirdparty/cJSON',
                'tests/server_storage_test.cpp', 'src/core/local_file_server.cpp', 'thirdparty/cJSON/cJSON.c',
                '-lws2_32', '-liphlpapi', '-o', str(exe)], cwd=root, check=True)
with tempfile.TemporaryDirectory(prefix='fixture-', dir=out) as fixture:
    subprocess.run([str(exe), fixture], cwd=root, check=True, timeout=45)
