"""编译固件匹配测试, 不连接调试器或访问设备。

可选参数依次为 AXF 路径、身份描述地址、构建标识。
"""

from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
compiler = shutil.which("g++")
if compiler is None:
    raise FileNotFoundError("此测试需要支持 C++20 的宿主 g++。")
output = root / "build/firmware-match/firmware_match_test.exe"
output.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(
    [compiler, "-std=c++20", "-O2", "-Isrc", "tests/firmware_match_test.cpp", "-o", str(output)],
    cwd=root,
    check=True,
)
subprocess.run([str(output), *sys.argv[1:]], cwd=root, check=True, timeout=30)
