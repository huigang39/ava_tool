#!/usr/bin/env python3
"""
Authenticode-sign one or more files with signtool (SHA-256 + timestamp).

Removes the Windows "Unknown Publisher" warning. Requires a code-signing
certificate. Configure it one of two ways:

  PFX file (OV certificate):
    set AVA_SIGN_PFX=C:/path/cert.pfx
    set AVA_SIGN_PASS=pfx-password        (optional if the pfx has none)

  Cert already in the Windows store (e.g. an EV token):
    set AVA_SIGN_SHA1=<cert thumbprint>

If no certificate is configured the script prints a note and exits 0, so an
unsigned build still succeeds.

Usage:
    python tools/sign.py bin/win/ava_tool.exe bin/win/updater.exe
    python tools/sign.py dist/ava_tool_setup_*.exe
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys


def find_signtool() -> str | None:
    """Locate signtool.exe from PATH or the Windows SDK."""
    path = shutil.which("signtool.exe") or shutil.which("signtool")
    if path:
        return path

    # Search in Windows SDK
    sdk_root = r"C:\Program Files (x86)\Windows Kits\10\bin"
    if os.path.isdir(sdk_root):
        candidates = sorted(
            glob.glob(os.path.join(sdk_root, "*", "x64", "signtool.exe")),
            reverse=True,
        )
        if candidates:
            return candidates[0]

    return None


def main():
    parser = argparse.ArgumentParser(description="Authenticode sign files with signtool")
    parser.add_argument("files", nargs="+", help="Files or glob patterns to sign")
    parser.add_argument("--pfx", default=None, help="PFX certificate path")
    parser.add_argument("--password", default=None, help="PFX password")
    parser.add_argument("--sha1", default=None, help="Certificate thumbprint (store/EV)")
    parser.add_argument("--timestamp", default=None, help="Timestamp server URL")
    args = parser.parse_args()

    pfx = args.pfx or os.environ.get("AVA_SIGN_PFX", "")
    password = args.password or os.environ.get("AVA_SIGN_PASS", "")
    sha1 = args.sha1 or os.environ.get("AVA_SIGN_SHA1", "")
    timestamp = args.timestamp or os.environ.get("AVA_SIGN_TS", "http://timestamp.digicert.com")

    if not pfx and not sha1:
        print("[sign] No certificate configured (set AVA_SIGN_PFX/AVA_SIGN_PASS or AVA_SIGN_SHA1) - skipping signing.")
        sys.exit(0)

    signtool = find_signtool()
    if not signtool:
        print("ERROR: signtool.exe not found. Install the Windows SDK (or add signtool to PATH).", file=sys.stderr)
        sys.exit(1)

    cmd_base = [signtool, "sign", "/fd", "SHA256", "/tr", timestamp, "/td", "SHA256"]
    if pfx:
        cmd_base += ["/f", pfx]
        if password:
            cmd_base += ["/p", password]
    else:
        cmd_base += ["/sha1", sha1]

    for pattern in args.files:
        # Support comma-separated patterns (for Makefile compat)
        for p in pattern.split(","):
            p = p.strip()
            if not p:
                continue
            matches = glob.glob(p)
            if not matches:
                print(f"[sign] skip (no match): {p}")
                continue
            for filepath in matches:
                print(f"[sign] {filepath}")
                ret = subprocess.run(cmd_base + [filepath])
                if ret.returncode != 0:
                    print(f"ERROR: signtool failed for {filepath}", file=sys.stderr)
                    sys.exit(ret.returncode)

    print("[sign] done.")


if __name__ == "__main__":
    main()
