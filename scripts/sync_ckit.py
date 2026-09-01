"""从固件项目同步 ckit 源码, 保留桌面构建设置和专用扩展."""

import argparse
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="固件项目中的 ckit 目录")
    parser.add_argument("--check", action="store_true", help="只检查差异, 不修改文件")
    args = parser.parse_args()
    source = args.source.resolve()
    destination = Path(__file__).resolve().parents[1] / "ckit"
    for name in ("include/ckit.h", "include/param_list.h", "src/param_list.c", "include/safety.h", "src/safety.c"):
        if not (source / name).is_file():
            parser.error(f"不是完整的 ckit 源码目录: {source}")
    files = [source / name for name in ("include/ckit.h", ".clang-format", ".gitignore")]
    for name in ("include", "src"):
        files.extend(path for path in (source / name).rglob("*") if path.is_file())
    differences = []
    for path in sorted(set(files)):
        relative = path.relative_to(source)
        target = destination / relative
        content = path.read_bytes()
        # Firmware snapshots may still use platform-first dependency paths.
        if relative.as_posix() == "src/jlink_port.c":
            for platform in (b"win", b"linux", b"mac"):
                content = content.replace(b'"lib/' + platform + b'/', b'"lib/jlink/')
                content = content.replace(b'lib/jlink/' + platform + b'/', b'lib/jlink/')
        if target.is_file() and target.read_bytes() == content:
            continue
        differences.append(relative.as_posix())
        if not args.check:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
    for name in differences:
        print(name)
    if args.check:
        print(f"Checked {len(files)} shared files; {len(differences)} differences.")
        return int(bool(differences))
    print(f"Updated {len(differences)} shared files.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
