#!/usr/bin/env bash
# End-to-end demo: build -> test documents -> tests -> screenshots.
set -euo pipefail
cd "$(dirname "$0")/.."

QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
export QT_QPA_PLATFORM

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)" >/dev/null
bash scripts/make_testdata.sh build >/dev/null

THOME="$(mktemp -d)"
export XDG_DATA_HOME="$THOME"

echo "== self test (engine / selection / storage / write-back / epub)"
(cd build && XDG_DATA_HOME="$(mktemp -d)" ./selftest test.pdf test.epub)

echo
echo "== visual regression (overlay geometry vs rendered text)"
(cd build && XDG_DATA_HOME="$(mktemp -d)" ./visualtest test.pdf)

echo "== feature test (tags / search / image excerpt / undo / reading position)"
(cd build && XDG_DATA_HOME="$(mktemp -d)" ./featuretest test.pdf)

echo "== export test (md / html / opml / xmind / apkg read-back)"
(cd build && XDG_DATA_HOME="$(mktemp -d)" ./exporttest test.pdf)

echo "== sync test (version vectors + two devices over a real WebDAV server)"
(cd build && XDG_DATA_HOME="$(mktemp -d)" ./synctest test.pdf "$PWD/../scripts/dav_server.py")

echo "== AI test (prompt building + SSE streaming against a fake endpoint)"
(cd build && XDG_DATA_HOME="$(mktemp -d)" ./aitest "$PWD/../scripts/fake_openai_server.py")

echo "== P5 test (handwriting geometry / mind map layout / read-aloud iterator)"
(cd build && XDG_DATA_HOME="$(mktemp -d)" ./p5test test.pdf)

echo "== library test (document table / cross-document search / filters)"
(cd build && XDG_DATA_HOME="$(mktemp -d)" ./librarytest)

echo "== migration test (notes follow their text across a PDF revision)"
(cd build && XDG_DATA_HOME="$(mktemp -d)" ./migratetest test.pdf)

echo "== import test (markdown / json round trip, foreign markdown)"
(cd build && XDG_DATA_HOME="$(mktemp -d)" ./importtest test.pdf)

echo "== conflict test (pairing + keep left/right/both)"
(cd build && XDG_DATA_HOME="$(mktemp -d)" ./conflicttest)

echo
echo "== exports + independent validation (python stdlib)"
rm -rf shots/exports && mkdir -p shots/exports
QT_QPA_PLATFORM=xcb xvfb-run -a -s "-screen 0 1400x950x24" \
  ./build/adaptnote --doc build/test.pdf --demo --export-dir shots/exports >/dev/null 2>&1
python3 scripts/validate_exports.py shots/exports

echo
echo "== screenshots"
mkdir -p shots
shot() { # name mode zoom rotate doc [extra args...]
  local name=$1 mode=$2 zoom=$3 rot=$4 doc=$5
  shift 5
  QT_QPA_PLATFORM=xcb HOME=/tmp/anhome xvfb-run -a -s "-screen 0 1500x1000x24" \
    ./build/adaptnote --doc "$doc" --mode "$mode" --zoom "$zoom" --rotate "$rot" \
    --screenshot "shots/$name.png" "$@" >/dev/null 2>&1
  echo "  shots/$name.png"
}
shot main      continuous fitwidth  0 build/test.pdf --demo
shot double    double     fitpage   0 build/test.pdf --demo
shot rotated   single     fitwidth 90 build/test.pdf --demo
shot epub      continuous fitwidth  0 build/test.epub
shot find      continuous fitwidth  0 build/test.pdf --demo --find 阅读

QT_QPA_PLATFORM=xcb xvfb-run -a -s "-screen 0 1400x900x24" \
  ./build/adaptnote --doc build/test.pdf --demo --mindmap shots/mindmap.png >/dev/null 2>&1
[ -f shots/mindmap.png ] && echo "  shots/mindmap.png"

echo
echo "done."

# the library window reads the app data dir: seed it from a demo run first
LIB_HOME="$(mktemp -d)"
XDG_DATA_HOME="$LIB_HOME" QT_QPA_PLATFORM=xcb xvfb-run -a -s "-screen 0 1400x900x24"   ./build/adaptnote --doc build/test.pdf --demo --screenshot /dev/null >/dev/null 2>&1 || true
XDG_DATA_HOME="$LIB_HOME" QT_QPA_PLATFORM=xcb xvfb-run -a -s "-screen 0 1200x800x24"   ./build/adaptnote --library shots/library.png >/dev/null 2>&1
[ -f shots/library.png ] && echo "  shots/library.png"
