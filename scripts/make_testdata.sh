#!/usr/bin/env bash
# Builds the sample documents used by selftest + screenshots.
set -euo pipefail
export QT_QPA_PLATFORM=offscreen
OUT="${1:-build}"
mkdir -p "$OUT"

# ---------- PDF (3 pages: text, two columns, hierarchy) ----------
"$OUT/mkpdf" "$OUT/test.pdf"

# ---------- the same document with a cover page (for the migration test) ----------
"$OUT/mkpdf" "$OUT/test2.pdf" --blank-first

# ---------- EPUB ----------
python3 "$(dirname "$0")/make_test_epub.py" "$OUT/test.epub"

ls -l "$OUT/test.pdf" "$OUT/test.epub"
