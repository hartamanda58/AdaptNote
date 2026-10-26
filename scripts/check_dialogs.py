#!/usr/bin/env python3
"""Guard: no modal dialog may be opened outside a headless-aware wrapper.

Unattended runs (CI, scripts) set ADAPT_HEADLESS=1 and run under QT_QPA_PLATFORM=offscreen.
A bare QMessageBox/QInputDialog call there opens a modal event loop that nobody can answer,
so the process hangs until the CI watchdog kills it. This check fails the build instead.

Each file below must route every dialog through the listed wrapper functions; the wrappers
themselves are the only place where the Qt dialog classes may be named.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FILES = {
    "src/ui/mainwindow.cpp": {"confirm", "askYes", "inform", "warnUser", "askText",
                              "askMultiline", "askItem"},
    "src/ui/libraryview.cpp": {"confirm", "askText"},
    "src/ui/conflictdialog.cpp": set(),
    "src/ui/pageview.cpp": set(),
}

# Only *blocking* entry points matter (enums like QMessageBox::Yes are fine).
DIALOG = re.compile(
    r"\bQMessageBox\s*::\s*(question|warning|warningCritical|information|critical|about|exec)\b"
    r"|\bQInputDialog\s*::\s*(getText|getMultiLineText|getItem|getInt|getDouble|getExistingDirectory)\b")

# Top-level function definitions in this code base start at column 0 and end with `}` at column 0.
DEF_LINE = re.compile(r"^[A-Za-z_][\w:<>,&\s\*]*?\b([A-Za-z_]\w*)\s*\(")


def check_file(rel: str, allowed: set[str]) -> list[str]:
    path = ROOT / rel
    if not path.exists():
        return [f"{rel}: missing"]
    lines = path.read_text(encoding="utf-8").splitlines()
    problems: list[str] = []

    if allowed and "ADAPT_HEADLESS" not in "\n".join(lines):
        problems.append(f"{rel}: dialogs are wrapped but ADAPT_HEADLESS is never read")

    current = ""
    for idx, line in enumerate(lines, start=1):
        stripped = line.strip()
        if line[:1] not in (" ", "\t", "", "}"):
            m = DEF_LINE.match(line)
            if m:
                current = m.group(1)
        elif line.startswith("}") and not line.startswith("};"):
            current = ""
        if DIALOG.search(line) and current not in allowed:
            problems.append(
                f"{rel}:{idx}: modal dialog inside '{current or '?'}' -> would hang under "
                f"ADAPT_HEADLESS: {stripped[:80]}")
    return problems


def main() -> int:
    problems: list[str] = []
    for rel, allowed in FILES.items():
        problems.extend(check_file(rel, allowed))
    if problems:
        print("DIALOG GUARD FAILED")
        for p in problems:
            print("  " + p)
        return 1
    print(f"DIALOG GUARD PASSED ({len(FILES)} files, no bare modal dialogs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
