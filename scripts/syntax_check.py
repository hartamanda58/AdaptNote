#!/usr/bin/env python3
"""Syntax-checks one source file using the exact flags CMake produced.

Why: a full build is slow and this environment's compiler gets killed on big TUs, while
clangd cannot resolve the include paths here (the checkout path contains a space). This
runs `-fsyntax-only` with the real command line instead, which catches typos before a
45-minute CI round trip.

Usage:
    python3 scripts/syntax_check.py src/core/store.cpp [more.cpp ...]

Requires: configure once with `cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`.
"""

from __future__ import annotations

import json
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DB = ROOT / "build" / "compile_commands.json"

DROP_WITH_VALUE = {"-o", "-MT", "-MF", "-MQ", "-MD", "-MMD", "-c"}


def command_for(entry: dict) -> list[str]:
    args = shlex.split(entry["command"])
    out: list[str] = []
    skip = False
    for a in args:
        if skip:
            skip = False
            continue
        if a in DROP_WITH_VALUE:
            if a in {"-o", "-MT", "-MF", "-MQ"}:
                skip = True
            continue
        out.append(a)
    out.append("-fsyntax-only")
    return out


def main(argv: list[str]) -> int:
    if not DB.exists():
        print(f"missing {DB}; configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON", file=sys.stderr)
        return 2
    entries = json.loads(DB.read_text())
    by_file = {Path(e["file"]).resolve(): e for e in entries}

    failed = 0
    for target in argv:
        path = (ROOT / target).resolve()
        entry = by_file.get(path)
        if entry is None:
            print(f"{target}: not in compile_commands.json", file=sys.stderr)
            failed += 1
            continue
        cmd = command_for(entry)
        proc = subprocess.run(cmd, cwd=entry["directory"], capture_output=True, text=True)
        if proc.returncode == 0:
            print(f"{target}: OK")
        else:
            failed += 1
            print(f"{target}: FAILED")
            print((proc.stderr or proc.stdout).strip()[:4000])
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
