#!/usr/bin/env python3
"""
ava_tool release script.

Usage:
    python script/release.py 1.2.0
    python script/release.py 1.2.0 --draft
    python script/release.py 1.2.0 --skip-build
"""

import argparse
import os
import re
import subprocess
import sys

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION_HPP = os.path.join(PROJECT_ROOT, "src", "version.hpp")
ISS_FILE = os.path.join(PROJECT_ROOT, "installer", "ava_tool.iss")


def run(cmd: list[str], check: bool = True, **kwargs) -> subprocess.CompletedProcess:
    """Run a command, printing it first."""
    print(f"  $ {' '.join(cmd)}")
    return subprocess.run(cmd, cwd=PROJECT_ROOT, check=check, **kwargs)


def update_version(version: str):
    """Write version into version.hpp and ava_tool.iss."""
    print(f"\n[1/6] Updating version to {version} ...")

    # version.hpp
    with open(VERSION_HPP, "r", encoding="utf-8") as f:
        content = f.read()
    content = re.sub(
        r'#define AVA_VERSION\s+"[^"]+"',
        f'#define AVA_VERSION      "{version}"',
        content,
    )
    with open(VERSION_HPP, "w", encoding="utf-8") as f:
        f.write(content)
    print(f'       src/version.hpp  -> AVA_VERSION = "{version}"')

    # ava_tool.iss
    with open(ISS_FILE, "r", encoding="utf-8") as f:
        content = f.read()
    content = re.sub(
        r'#define MyAppVersion\s+"[^"]+"',
        f'#define MyAppVersion "{version}"',
        content,
    )
    with open(ISS_FILE, "w", encoding="utf-8") as f:
        f.write(content)
    print(f'       installer/ava_tool.iss -> MyAppVersion = "{version}"')


def build():
    """Build the project and package the installer."""
    print("\n[2/6] Building ...")
    run(["make", "-j8"])

    print("\n[3/6] Packaging installer ...")
    run(["make", "package"])


def git_commit_and_tag(version: str):
    """Commit the version bump and create a git tag."""
    tag = f"v{version}"
    print(f"\n[4/6] Committing version bump & creating tag {tag} ...")

    run(["git", "add", "src/version.hpp", "installer/ava_tool.iss"])
    ret = run(["git", "commit", "-m", f"release: {tag}"], check=False)
    if ret.returncode != 0:
        print("       (commit skipped — maybe nothing changed)")

    # Delete existing tag if re-running
    run(["git", "tag", "-d", tag], check=False)
    run(["git", "tag", tag])
    print(f"       Tag: {tag}")


def git_push(version: str):
    """Push commits and tag to origin."""
    tag = f"v{version}"
    print("\n[5/6] Pushing to origin ...")
    run(["git", "push", "origin", "HEAD"])
    run(["git", "push", "origin", tag, "--force"])


def gh_release(version: str, setup_exe: str, draft: bool):
    """Create a GitHub Release and upload the installer."""
    tag = f"v{version}"
    print("\n[6/6] Creating GitHub Release ...")

    cmd = [
        "gh", "release", "create", tag,
        setup_exe,
        "--title", tag,
        "--generate-notes",
    ]
    if draft:
        cmd.append("--draft")

    run(cmd)


def main():
    parser = argparse.ArgumentParser(description="Build & release ava_tool to GitHub.")
    parser.add_argument("version", help='Semantic version, e.g. "1.2.0" (no leading "v")')
    parser.add_argument("--draft", action="store_true", help="Create the release as a draft")
    parser.add_argument("--skip-build", action="store_true", help="Skip build & package steps")
    args = parser.parse_args()

    version = args.version
    if not re.match(r"^\d+\.\d+\.\d+$", version):
        print(f"ERROR: invalid version format: {version}  (expected X.Y.Z)", file=sys.stderr)
        sys.exit(1)

    print("")
    print("═══════════════════════════════════════════════════")
    print(f"  ava_tool release  v{version}")
    print("═══════════════════════════════════════════════════")

    # Pre-flight: check gh
    ret = subprocess.run(["gh", "--version"], capture_output=True)
    if ret.returncode != 0:
        print("ERROR: GitHub CLI (gh) not found. Install from https://cli.github.com/", file=sys.stderr)
        sys.exit(1)

    # Warn about dirty working tree
    ret = subprocess.run(["git", "status", "--porcelain"], capture_output=True, text=True, cwd=PROJECT_ROOT)
    if ret.stdout.strip():
        print(f"\n[WARN] Working tree has uncommitted changes:\n{ret.stdout.strip()}\n")

    # 1. Update version
    update_version(version)

    # 2-3. Build & package
    if not args.skip_build:
        build()
    else:
        print("\n[2/6] Build skipped (--skip-build)")
        print("[3/6] Package skipped (--skip-build)")

    # Verify installer exists
    setup_exe = os.path.join(PROJECT_ROOT, "dist", f"ava_tool_setup_{version}.exe")
    if not os.path.isfile(setup_exe):
        print(f"ERROR: installer not found: {setup_exe}", file=sys.stderr)
        sys.exit(1)
    size_mb = os.path.getsize(setup_exe) / (1024 * 1024)
    print(f"       Installer: {setup_exe} ({size_mb:.2f} MB)")

    # 4. Git commit & tag
    git_commit_and_tag(version)

    # 5. Push
    git_push(version)

    # 6. GitHub Release
    gh_release(version, setup_exe, args.draft)

    print("")
    print("═══════════════════════════════════════════════════")
    print(f"  Release v{version} complete!")
    print("═══════════════════════════════════════════════════")
    print("")


if __name__ == "__main__":
    main()
