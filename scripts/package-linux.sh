#!/usr/bin/env bash
# Builds the portable Linux bundle dist/scvp-<version>-linux-x86_64/ (+ .tar.gz). It runs on
# Ubuntu 20.04+, Debian 10+, RHEL 8+ and Fedora with nothing else to install: Qt, FFmpeg and
# the libraries a desktop may lack are inside. Only what every desktop with a GPU driver has
# comes from the system (the `excluded` list below: glibc, libstdc++, OpenGL, X11 and the xcb
# libraries Mesa uses, fontconfig, glib, dbus, ...). Taking those from the system is what keeps
# the bundle compatible with the system's GL driver.
#
# The build runs in a container with glibc 2.28, Qt 6.8 and an LGPL FFmpeg built there
# (packaging/linux/Dockerfile), so docker is all that is needed:
#
#   scripts/package-linux.sh        then run dist/scvp-*/scvp [files], or its install.sh
#
# SCVP_BUILDER_IMAGE=<image> uses that image (CI builds it with a layer cache) instead of
# building packaging/linux/Dockerfile here.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ "${SCVP_IN_BUILDER:-}" != 1 ]]; then
  IMAGE="${SCVP_BUILDER_IMAGE:-}"
  if [[ -z "$IMAGE" ]]; then
    IMAGE="scvp-linux-builder:$(sha256sum packaging/linux/Dockerfile | cut -c1-12)"
    docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" packaging/linux
  fi
  # As the calling user, so build/ and dist/ stay theirs.
  exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e SCVP_IN_BUILDER=1 \
    -v "$ROOT:/src" -w /src "$IMAGE" scripts/package-linux.sh "$@"
fi

# ---- inside the builder ---------------------------------------------------------------------
QT="${SCVP_QT_DIR:?not in the builder image}"
FF=/opt/ffmpeg
BUILD=build/linux-bundle
# OpenGL through libGL.so.1, as Qt itself: GLVND's libOpenGL.so.0 is not installed everywhere
# (Ubuntu 20.04 has no libopengl0 by default).
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSCVP_BUILD_TESTS=OFF -DSCVP_BUILD_TOOLS=OFF \
  -DCMAKE_PREFIX_PATH="$QT" -DFFMPEG_ROOT="$FF" -DOpenGL_GL_PREFERENCE=LEGACY >/dev/null
cmake --build "$BUILD"
VERSION="$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | head -1)"
NAME="scvp-${VERSION}-linux-x86_64"
OUT="dist/$NAME"
rm -rf "$OUT" "$OUT.tar.gz"
cmake --install "$BUILD" --prefix "$OUT" >/dev/null

# Qt plugins: the X11 platform (also under Wayland, through XWayland: the GL and CUDA interop
# paths are the GLX/EGL-on-X11 ones), its GL integrations, and input methods (compose, IBus).
# bin/qt.conf points Qt at them, so no QT_PLUGIN_PATH leaks into programs scvp starts.
for p in platforms/libqxcb.so xcbglintegrations/libqxcb-glx-integration.so \
         xcbglintegrations/libqxcb-egl-integration.so \
         platforminputcontexts/libcomposeplatforminputcontextplugin.so \
         platforminputcontexts/libibusplatforminputcontextplugin.so; do
  install -D -m 755 "$QT/plugins/$p" "$OUT/plugins/$p"
done
printf '[Paths]\nPrefix = ..\nPlugins = plugins\n' > "$OUT/bin/qt.conf"

# Base system: never bundled. GL drivers load these too, and a bundled older copy would shadow
# the version the driver needs. libxcb's extension libraries that Mesa uses are here; those only
# Qt uses (xkb, xinput, render, shape, the xcb-util family) are bundled, being often missing.
excluded() {
  case "$1" in
    ld-linux*|libc.so*|libm.so*|libmvec.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|libutil.so*) ;;
    libanl.so*|libnss_*|libthread_db.so*|libBrokenLocale.so*|libstdc++.so*|libgcc_s.so*) ;;
    libGL.so*|libGLX.so*|libEGL.so*|libOpenGL.so*|libGLdispatch.so*|libGLESv2.so*|libglapi.so*) ;;
    libgbm.so*|libdrm.so*|libvulkan.so*|libcuda.so*|libnvcuvid.so*|libnvidia-encode.so*) ;;
    libX11.so*|libX11-xcb.so*|libXext.so*|libXau.so*|libXdmcp.so*|libSM.so*|libICE.so*) ;;
    libxcb.so*|libxcb-glx.so*|libxcb-dri2.so*|libxcb-dri3.so*|libxcb-present.so*|libxcb-sync.so*) ;;
    libxcb-xfixes.so*|libxcb-randr.so*|libxcb-shm.so*|libwayland-*) ;;
    libfontconfig.so*|libfreetype.so*|libharfbuzz.so*|libfribidi.so*|libz.so*|libexpat.so*) ;;
    libuuid.so*|libzstd.so*|libglib-2.0.so*|libgobject-2.0.so*|libgio-2.0.so*|libgthread-2.0.so*) ;;
    libgmodule-2.0.so*|libdbus-1.so*|libgpg-error.so*|libcom_err.so*) ;;
    *) return 1 ;;
  esac
}
# libva must be the system's, which matches its VA drivers; lib/fallback/ (added by the launcher
# only where the system has none) just lets the program start there.
fallback() { case "$1" in libva.so*|libva-drm.so*) return 0 ;; esac; return 1; }

# Walks DT_NEEDED from the program and plugins, copying each library outside the base system
# under its soname. A flattened `ldd` list would also drag in what excluded libraries need.
export LD_LIBRARY_PATH="$QT/lib:$FF/lib"
bundle() {
  local f="$1" n path dest
  for n in $(patchelf --print-needed "$f"); do
    excluded "$n" && continue
    if fallback "$n"; then dest="$OUT/lib/fallback"; else dest="$OUT/lib"; fi
    [[ -e "$dest/$n" ]] && continue
    path="$(ldd "$f" | awk -v n="$n" '$1 == n && $3 ~ /^\// {print $3}')"
    if [[ -z "$path" ]]; then echo "cannot find $n (needed by $f)" >&2; exit 1; fi
    mkdir -p "$dest"
    cp -L "$path" "$dest/$n"
    chmod 755 "$dest/$n"
    bundle "$dest/$n"
  done
}
bundle "$OUT/bin/scvp"
for p in "$OUT"/plugins/*/*.so; do bundle "$p"; done
unset LD_LIBRARY_PATH

# Everything finds its libraries relative to itself (DT_RUNPATH: unlike DT_RPATH it is not
# inherited, so libraries the GL driver loads never search the bundle).
patchelf --set-rpath '$ORIGIN/../lib' "$OUT/bin/scvp"
for l in "$OUT"/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$l"; done
for l in "$OUT"/lib/fallback/*.so*; do patchelf --remove-rpath "$l"; done
for p in "$OUT"/plugins/*/*.so; do patchelf --set-rpath '$ORIGIN/../../lib' "$p"; done

# Launcher (also the AppImage's AppRun).
cat > "$OUT/scvp" <<'SH'
#!/bin/sh
# Starts the bundled SCVP. Libraries come through RUNPATH and Qt plugins through bin/qt.conf, so
# programs scvp starts (xdg-mime, ...) see the environment as it was, except for this:
HERE="$(dirname "$(readlink -f "$0")")"
# A desktop session's Qt plugins would load into the bundled Qt; only the X11 platform is bundled.
unset QT_PLUGIN_PATH
case "${QT_QPA_PLATFORM:-}" in wayland*) QT_QPA_PLATFORM=xcb; export QT_QPA_PLATFORM ;; esac
# No libva on this system: the bundled one, so the program starts (no VA-API decoding then).
if LD_TRACE_LOADED_OBJECTS=1 "$HERE/bin/scvp" 2>/dev/null | grep -q 'libva.* not found'; then
  LD_LIBRARY_PATH="$HERE/lib/fallback${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  export LD_LIBRARY_PATH
fi
exec "$HERE/bin/scvp" "$@"
SH
chmod 755 "$OUT/scvp"
# Per-user installer (applications menu, icons, file types, default player).
install -m 755 packaging/install.sh "$OUT/install.sh"

# ---- checks: the bundle is complete and asks for nothing newer than glibc 2.28 -------------
mapfile -t elves < <(find "$OUT" -type f \( -name '*.so*' -o -path "$OUT/bin/scvp" \))
fail=0
newest() { objdump -T "$1" | { grep -o "$2_[0-9][0-9.]*" || true; } | sed "s/^$2_//" | sort -uV | tail -1; }
for f in "${elves[@]}"; do
  for want in GLIBC:2.28 GLIBCXX:3.4.25 CXXABI:1.3.11; do
    v="$(newest "$f" "${want%%:*}")"
    if [[ -n "$v" && "$(printf '%s\n%s\n' "$v" "${want#*:}" | sort -V | tail -1)" != "${want#*:}" ]]; then
      echo "${f#"$OUT"/} needs ${want%%:*}_$v (newer than ${want#*:})"; fail=1
    fi
  done
  # (Output captured first: `| grep -q` under pipefail fails when grep stops reading early.)
  deps="$(env -i LD_LIBRARY_PATH="$OUT/lib/fallback" ldd "$f" 2>&1)"
  if grep -q 'not found' <<<"$deps"; then
    echo "${f#"$OUT"/}: $(grep 'not found' <<<"$deps" | tr -s ' \t' ' ')"; fail=1
  fi
done
[[ $fail == 0 ]] || { echo "bundle check failed" >&2; exit 1; }

tar -C dist -czf "$OUT.tar.gz" "$NAME"
echo "bundled: $(ls "$OUT/lib" | tr '\n' ' ')"
du -sh "$OUT" "$OUT.tar.gz"
echo "Run: $OUT/scvp [A] [B]    Install for this user: $OUT/install.sh [--set-default]"
