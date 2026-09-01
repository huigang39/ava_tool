"""检查源码格式, 列出待修改文件, 用户确认后再格式化"""

import argparse
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def source_files(directories):
    """收集源码路径, 保留文件名中的空格并去重"""
    files = set()
    for directory in directories:
        root = Path(directory).resolve()
        if not root.is_dir():
            raise ValueError(f"Source directory not found: {directory}")
        files.update(
            path
            for path in root.rglob("*")
            if path.is_file() and path.suffix.lower() in {".c", ".cpp", ".h", ".hpp"}
        )
    return sorted(files)


def display_path(path):
    """优先显示相对当前目录的路径"""
    try:
        return str(path.relative_to(Path.cwd()))
    except ValueError:
        return str(path)


def needs_formatting(formatter, path):
    """读取格式差异, 检查失败时阻止后续写入"""
    result = subprocess.run(
        [formatter, "--output-replacements-xml", "-style=file", str(path)],
        stdout=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise ValueError(f"Cannot check: {display_path(path)}")
    try:
        report = ET.fromstring(result.stdout)
    except ET.ParseError as error:
        raise ValueError(f"Invalid formatter output: {display_path(path)}") from error
    if report.tag != "replacements" or report.get("incomplete_format") == "true":
        raise ValueError(f"Incomplete format check: {display_path(path)}")
    return bool(report.findall("replacement"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang-format", default="clang-format")
    parser.add_argument("directories", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        formatter = shutil.which(args.clang_format)
        if not formatter:
            raise ValueError(f"clang-format not found: {args.clang_format}")
        files = source_files(args.directories)
        print(f"checking {len(files)} file(s)", flush=True)
        pending = []
        for path in files:
            if needs_formatting(formatter, path):
                pending.append(path)
                print(f"needs formatting: {display_path(path)}", flush=True)
    except (OSError, ValueError) as error:
        print(f"[ERROR] {error}\nChecking failed; no files changed", file=sys.stderr)
        return 1
    if not pending:
        print("All files already conform; no changes needed")
        return 0
    try:
        answer = input(f"Format {len(pending)} file(s)? [y/N]: ")
    except (EOFError, KeyboardInterrupt):
        answer = ""
        print()
    if answer.strip().lower() not in {"y", "yes"}:
        print("Cancelled; no files changed")
        return 0
    changed = 0
    for path in pending:
        try:
            subprocess.run([formatter, "-i", "-style=file", str(path)], check=True)
        except (OSError, subprocess.CalledProcessError) as error:
            print(f"[ERROR] Cannot format {display_path(path)}: {error}", file=sys.stderr)
        else:
            changed += 1
            print(f"formatted: {display_path(path)}", flush=True)
    print(f"formatted {changed} of {len(pending)} file(s)")
    return 0 if changed == len(pending) else 1


if __name__ == "__main__":
    sys.exit(main())
