#!/usr/bin/env bash
# Builds a self-contained Linux bundle: dist/scvp-<version>-linux-x86_64/ (+ .tar.gz).
# Qt and every library that does not come from the base system (those found in .deps when
# building without root) are copied in; FFmpeg, OpenGL and X11 come from the system
# (Ubuntu 24.04: apt install ffmpeg, or the libavcodec60/libavformat60/libswscale7 packages).
#
#   scripts/package-linux.sh        then run dist/scvp-*/scvp [files]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
[[ -f .deps/env.sh ]] && source .deps/env.sh

cmake --preset release >/dev/null
cmake --build --preset release
VERSION="$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | head -1)"
OUT="dist/scvp-${VERSION}-linux-x86_64"
rm -rf "$OUT"
cmake --install build/release --prefix "$OUT" >/dev/null

# Qt plugins the player loads (X11 platform + GL integration, input methods, image formats).
QT_PLUGINS="${QT_PLUGIN_PATH:-$(qtpaths6 --query QT_INSTALL_PLUGINS 2>/dev/null || echo /usr/lib/x86_64-linux-gnu/qt6/plugins)}"
for d in platforms xcbglintegrations platforminputcontexts imageformats iconengines; do
  [[ -d "$QT_PLUGINS/$d" ]] && mkdir -p "$OUT/plugins/$d" && cp -L "$QT_PLUGINS/$d"/*.so "$OUT/plugins/$d/"
done

# Libraries resolved outside the base system: Qt itself, and anything ldd finds in .deps.
mkdir -p "$OUT/lib"
collect() {
  ldd "$1" | awk '/=> \// {print $3}' | while read -r lib; do
    case "$lib" in
      */libQt6*|"$ROOT"/.deps/*) [[ -e "$OUT/lib/$(basename "$lib")" ]] || cp -L "$lib" "$OUT/lib/" ;;
    esac
  done
}
collect "$OUT/bin/scvp"
find "$OUT/plugins" -name '*.so' | while read -r p; do collect "$p"; done
for l in "$OUT"/lib/*.so*; do collect "$l"; done

# Launcher: bundled libraries and plugins first.
cat > "$OUT/scvp" <<'SH'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/plugins"
exec "$HERE/bin/scvp" "$@"
SH
chmod +x "$OUT/scvp"

# Every library must resolve with nothing from the build tree.
missing="$(env -i PATH=/usr/bin:/bin LD_LIBRARY_PATH="$OUT/lib" ldd "$OUT/bin/scvp" "$OUT"/plugins/*/*.so | grep 'not found' || true)"
if [[ -n "$missing" ]]; then echo "unresolved libraries:"; echo "$missing"; exit 1; fi

tar -C dist -czf "$OUT.tar.gz" "$(basename "$OUT")"
du -sh "$OUT" "$OUT.tar.gz"
echo "Run: $OUT/scvp [A] [B]"
