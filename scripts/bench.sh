#!/usr/bin/env bash
# Runs the performance bench for a small and a large book and prints markdown tables.
set -euo pipefail
cd "$(dirname "$0")/.."

QT_QPA_PLATFORM=offscreen
export QT_QPA_PLATFORM
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)" >/dev/null

echo "## 300 pages / 3,000 notes"
(cd build && ./bench --pages 300 --notes 3000 | sed -n '/^|/p')

echo
echo "## 1000 pages / 6,000 notes"
(cd build && ./bench --pages 1000 --notes 6000 | sed -n '/^|/p')
