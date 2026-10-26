#!/usr/bin/env bash
# Builds a relocatable Linux bundle: binary + Qt + plugins + dependency closure.
# Output: dist/AdaptNote-<version>-linux-x64.zip
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${VERSION:-0.22}"
NAME="AdaptNote-${VERSION}-linux-x64"
DIST="dist"
STAGE="$DIST/$NAME"

echo "== build"
QT_QPA_PLATFORM=offscreen cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)" >/dev/null

rm -rf "$STAGE"
mkdir -p "$STAGE"/{bin,lib,plugins/{platforms,platformthemes,imageformats,styles,sqldrivers,iconengines,tls},data,samples}

echo "== binary"
cp build/adaptnote "$STAGE/bin/"
strip --strip-unneeded "$STAGE/bin/adaptnote" 2>/dev/null || true

echo "== plugin set"
QT_PLUGINS="/usr/lib/x86_64-linux-gnu/qt6/plugins"
copy_plugin() {
    local rel="$1"
    [ -f "$QT_PLUGINS/$rel" ] || return 0
    cp "$QT_PLUGINS/$rel" "$STAGE/plugins/$rel"
}
copy_plugin platforms/libqxcb.so
copy_plugin platforms/libqoffscreen.so
copy_plugin platforms/libqminimal.so
# platformthemes: only the Qt-native one; the GTK theme drags in all of gtk3/gio/gdk
copy_plugin platformthemes/libqgenericunixtheme.so

for p in "$QT_PLUGINS"/imageformats/*.so; do copy_plugin "imageformats/$(basename "$p")"; done
for p in "$QT_PLUGINS"/styles/*.so; do copy_plugin "styles/$(basename "$p")"; done
for p in "$QT_PLUGINS"/sqldrivers/*.so; do copy_plugin "sqldrivers/$(basename "$p")"; done
for p in "$QT_PLUGINS"/iconengines/*.so; do copy_plugin "iconengines/$(basename "$p")"; done
for p in "$QT_PLUGINS"/tls/*.so; do copy_plugin "tls/$(basename "$p")"; done

echo "== dependency closure"
# everything the binary and the plugins pull in, except the C library / loader
collect() {
    ldd "$1" 2>/dev/null | awk '/=> \//{print $3}' | while read -r lib; do
        base="$(basename "$lib")"
        case "$base" in
            libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|ld-linux*) continue ;;
        esac
        [ -f "$STAGE/lib/$base" ] || cp -L "$lib" "$STAGE/lib/$base"
    done
}
collect "$STAGE/bin/adaptnote"
find "$STAGE/plugins" -name '*.so' | while read -r so; do collect "$so"; done

echo "== strip libraries"
find "$STAGE/lib" -name '*.so*' -exec strip --strip-unneeded {} + 2>/dev/null || true

echo '== rpath ($ORIGIN/lib)'
patchelf --set-rpath '$ORIGIN/../lib' "$STAGE/bin/adaptnote"
find "$STAGE/plugins" -name '*.so' -exec patchelf --set-rpath '$ORIGIN/../../lib' {} \;

echo "== launcher, manifest, samples"
cat > "$STAGE/AdaptNote.sh" <<'LAUNCHER'
#!/usr/bin/env bash
# Portable launcher: keeps every byte of data inside this folder.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export QT_PLUGIN_PATH="$HERE/plugins"
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export ADAPT_DATA_DIR="$HERE/data"   # Qt reads XDG_* here, but be explicit
export XDG_DATA_HOME="$HERE/data"
export XDG_CONFIG_HOME="$HERE/data/config"
export XDG_CACHE_HOME="$HERE/data/cache"
mkdir -p "$XDG_DATA_HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"
exec "$HERE/bin/adaptnote" "$@"
LAUNCHER
chmod +x "$STAGE/AdaptNote.sh"

cat > "$STAGE/README.txt" <<'README'
AdaptNote（便携版）
===================

运行:  ./AdaptNote.sh             （或者 ./AdaptNote.sh --doc 某本书.pdf）
数据:  全部存放在本目录的 data/ 里（笔记库 data/AdaptNote/AdaptNote/library.db），
       整个目录拷到 U 盘即可带走；删掉 data/ 就是出厂状态。
示例:  samples/test.pdf 是一本 3 页的测试书，用来试手；samples/test.epub 是测试 EPUB。

包含:  AdaptNote 主程序、Qt 6.4 运行库与插件（xcb/offscreen、sqlite 驱动、图片格式、图标、
       样式、TLS）、MuPDF 1.21 静态渲染器、依赖闭包（不含 glibc）。
注意:  目标是 x86_64 Linux；首次运行需要系统里有 X11/Wayland 会话（或 xvfb）。
       glibc 未打包，因此需要与构建机 glibc 兼容（≥ 2.36）。

快捷键:  Ctrl+O 打开 · Ctrl+F 查找 · H/U/S/N 标注 · X 摘录图 · P 手写 · E 橡皮
        M 脑图 · R 朗读 · L 书库 · Ctrl+S 写入 PDF · Ctrl+Z 撤销
README

cp build/test.pdf build/test.epub "$STAGE/samples/" 2>/dev/null || true

cat > "$STAGE/BUILD-INFO.txt" <<INFO
build host : $(uname -srm)
built at   : $(date -u +%Y-%m-%dT%H:%M:%SZ)
qt         : $(grep -m1 QT_VERSION_STR /usr/include/x86_64-linux-gnu/qt6/QtCore/qtcoreversion.h 2>/dev/null | sed 's/.*"\(.*\)".*/\1/' || echo 6.4)
files      : $(find "$STAGE" -type f | wc -l)
size       : $(du -sh "$STAGE" | cut -f1)
INFO

echo "== zip"
(cd "$DIST" && rm -f "$NAME.zip" && zip -qry "$NAME.zip" "$NAME")
ls -lh "$DIST/$NAME.zip"
du -sh "$STAGE"
