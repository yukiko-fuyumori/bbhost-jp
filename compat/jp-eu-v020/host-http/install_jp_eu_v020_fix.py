#!/usr/bin/env python3
"""Safely install the reviewed HTTP fix for the exact user-provided BBHost v0.2.20 source.

The installer intentionally refuses unknown versions: it never heuristically rewrites
arbitrary updated code. Works on Linux and Windows with Python 3.9+.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import sys
from datetime import datetime

EXPECTED_SHA256 = "cefd8ff8ceb2b9c48188c17501c973225203a4db57481f7f109e25cbcad353ca"
PATCHED_SHA256 = "131335786375605fe14109434eec27619b3436563834fb94f534120c18ef5f42"


def digest(content):
    return hashlib.sha256(content).hexdigest()


def main():
    parser = argparse.ArgumentParser(description="Apply reviewed JP/EU reconnect and play-log HTTP fix")
    parser.add_argument("--source", type=Path, required=True, help="Root of BBHost source checkout")
    parser.add_argument("--dry-run", action="store_true", help="Check source and payload without writing anything")
    args = parser.parse_args()

    target = args.source.expanduser().resolve() / "src" / "hle" / "http.cpp"
    payload = Path(__file__).resolve().parent / "http-jp-eu-v020-fixed.cpp"
    if not target.is_file() or not payload.is_file():
        parser.error("Missing src/hle/http.cpp or companion http-jp-eu-v020-fixed.cpp")

    original = target.read_bytes()
    patch = payload.read_bytes()
    current_hash = digest(original)
    if digest(patch) != PATCHED_SHA256:
        parser.error("Bundled patched source failed integrity check; nothing changed")
    if current_hash == PATCHED_SHA256:
        print("Already installed. Nothing changed.")
        return
    if current_hash != EXPECTED_SHA256:
        parser.error(f"Unexpected http.cpp SHA-256 {current_hash}. Refusing to overwrite modified or new BBHost code.")
    if args.dry_run:
        print("PASS: exact source recognized; payload verified; safe to install.")
        return

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = target.with_name(f"http.cpp.before-jp-eu-fix-{stamp}.bak")
    if backup.exists():
        parser.error("A backup with that timestamp already exists; try again later")
    temp = target.with_name("http.cpp.jp-eu-install-tmp")
    if temp.exists():
        parser.error("Temporary install path already exists; inspect it before retrying")

    shutil.copy2(target, backup)
    try:
        temp.write_bytes(patch)
        shutil.copymode(target, temp)
        os.replace(temp, target)
    except Exception:
        temp.unlink(missing_ok=True)
        if digest(target.read_bytes()) != EXPECTED_SHA256:
            shutil.copy2(backup, target)
        raise
    print(f"Installed source: {target}")
    print(f"Backup: {backup}")
    print("Now rebuild BBHost and test initial login + title-screen reconnect using a backed-up save.")

if __name__ == "__main__":
    main()
