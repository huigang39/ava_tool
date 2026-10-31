#!/usr/bin/env python3
"""Build and package ava_tool locally.

Usage:
    python scripts/release.py          # Use the current version
    python scripts/release.py 1.2.0    # Update the version, then build and package
    python scripts/release.py --publish  # Build and publish a committed, tagged version
"""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys

PROJECT_ROOT = Path(__file__).resolve().parent.parent
VERSION_HPP = PROJECT_ROOT / "src" / "version.hpp"
ISS_FILE = PROJECT_ROOT / "installer" / "ava_tool.iss"
VERSION_PATTERN = r'#define AVA_VERSION\s+"([^"\r\n]+)"'
INSTALLER_VERSION_PATTERN = r'#define MyAppVersion\s+"([^"\r\n]+)"'


def read_version(path: Path, pattern: str) -> str:
    match = re.search(pattern, path.read_text(encoding="utf-8"))
    if not match:
        raise ValueError(f"Version definition not found in {path}")
    return match.group(1)


def update_version(version: str):
    """Update both version definitions, preserving each file's line endings."""
    updates = []
    for path, pattern in (
        (VERSION_HPP, VERSION_PATTERN),
        (ISS_FILE, INSTALLER_VERSION_PATTERN),
    ):
        content = path.read_bytes().decode("utf-8")
        matches = list(re.finditer(pattern, content))
        if len(matches) != 1:
            raise ValueError(f"Expected one version definition in {path}")
        match = matches[0]
        updated = content[:match.start(1)] + version + content[match.end(1):]
        updates.append((path, content, updated))
    for path, content, updated in updates:
        if updated != content:
            path.write_bytes(updated.encode("utf-8"))
        print(f"  {path.relative_to(PROJECT_ROOT)} -> {version}", flush=True)


def build():
    """The package target builds the app before creating the installer."""
    cmd = ["make", "package", "-j8"]
    print(f"  $ {' '.join(cmd)}", flush=True)
    subprocess.run(cmd, cwd=PROJECT_ROOT, check=True)


def check_publish(version: str):
    """Only publish source that matches an existing tag on origin."""
    if not shutil.which("gh"):
        raise ValueError("GitHub CLI (gh) not found. Install it and run: gh auth login")
    subprocess.run(["gh", "auth", "status", "--hostname", "github.com"],
                   cwd=PROJECT_ROOT, check=True)

    def git(*args):
        return subprocess.run(["git", *args], cwd=PROJECT_ROOT, check=True,
                              capture_output=True, text=True).stdout.strip()

    if git("status", "--porcelain", "--untracked-files=normal"):
        raise ValueError("Commit your changes before publishing (including version changes).")
    remote = git("remote", "get-url", "origin")
    match = re.fullmatch(r"(?:git@github\.com:|https://github\.com/|ssh://git@github\.com/)([^/]+/[^/]+?)(?:\.git)?/?", remote)
    if not match:
        raise ValueError("origin must point to a GitHub repository.")
    tag = f"v{version}"
    refs = git("ls-remote", "origin", f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}")
    commits = dict(line.split()[::-1] for line in refs.splitlines())
    target = commits.get(f"refs/tags/{tag}^{{}}", commits.get(f"refs/tags/{tag}"))
    if target != git("rev-parse", "HEAD"):
        raise ValueError(f"Push tag {tag} pointing to the current commit before publishing.")
    return match.group(1)


def publish(version: str, setup_exe: Path, repo: str):
    cmd = ["gh", "release", "create", f"v{version}", str(setup_exe),
           "--repo", repo, "--verify-tag", "--title", f"ava_tool v{version}",
           "--generate-notes"]
    print(f"Publishing GitHub Release v{version} ...", flush=True)
    subprocess.run(cmd, cwd=PROJECT_ROOT, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build ava_tool and optionally publish to GitHub Releases.")
    parser.add_argument(
        "version", nargs="?",
        help='Optional version X.Y.Z; defaults to the current project version',
    )
    parser.add_argument("--publish", action="store_true",
                        help="Publish installer to GitHub (requires gh login and a pushed version tag)")
    args = parser.parse_args()
    if args.version is not None and not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        parser.error(f"invalid version: {args.version} (expected X.Y.Z)")

    try:
        if args.version is not None:
            version = args.version
            update_version(version)
        else:
            version = read_version(VERSION_HPP, VERSION_PATTERN)
            installer_version = read_version(ISS_FILE, INSTALLER_VERSION_PATTERN)
            if version != installer_version:
                raise ValueError(
                    f"Version mismatch: app={version}, installer={installer_version}. "
                    "Pass a version X.Y.Z to synchronize them."
                )

        repo = check_publish(version) if args.publish else None
        print(f"Building local installer v{version} ...", flush=True)
        build()
        setup_exe = PROJECT_ROOT / "dist" / f"ava_tool_setup_{version}.exe"
        if not setup_exe.is_file():
            raise FileNotFoundError(f"Installer not found: {setup_exe}")
        size_mb = setup_exe.stat().st_size / (1024 * 1024)
        print(f"Local package complete: {setup_exe} ({size_mb:.2f} MB)")
        if args.publish:
            publish(version, setup_exe, repo)
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
