#!/usr/bin/env bash
# Runs the portable Linux bundle (scripts/package-linux.sh) on a fresh distribution in docker,
# with only the base system a desktop has: GL, X11, fontconfig and a font, glib, dbus. Nothing
# else is installed: no Qt, no FFmpeg, no xcb-util, and no libva unless asked for.
#
#   scripts/smoke-linux-bundle.sh clips DIR
#       makes the test clips (an ffmpeg CLI with x264, x265, libvpx and svt-av1 or libaom)
#   scripts/smoke-linux-bundle.sh IMAGE BUNDLE.tar.gz CLIPS [PACKAGE...]
#       e.g. ubuntu:20.04 dist/scvp-*-linux-x86_64.tar.gz /tmp/clips libva2 libva-drm2
#
# 1. With the base system only, every ELF in the bundle resolves all its libraries (through the
#    launcher's choice of system or bundled libva).
# 2. With Xvfb and Mesa added, the launcher opens each clip (H.264, HEVC 10-bit, VP9, AV1 through
#    dav1d, a PNG sequence, raw NV12, an A/B pair) with LD_BIND_NOW=1, so a symbol missing from
#    a system library fails at once, and saves a frame that is not blank. So does the AppImage
#    next to the tarball, if there is one.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

make_clips() {
  local d="$1" av1=libsvtav1
  mkdir -p "$d/png_seq"
  ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libsvtav1 || av1=libaom-av1
  local src=(-hide_banner -loglevel error -y -f lavfi -i testsrc2=size=320x240:rate=25 -frames:v 10)
  ffmpeg "${src[@]}" -c:v libx264 -pix_fmt yuv420p "$d/h264.mp4"
  ffmpeg "${src[@]}" -c:v libx265 -pix_fmt yuv420p10le -x265-params log-level=error "$d/hevc10.mkv"
  ffmpeg "${src[@]}" -c:v libvpx-vp9 -deadline realtime -b:v 500k "$d/vp9.webm"
  ffmpeg "${src[@]}" -c:v "$av1" -pix_fmt yuv420p "$d/av1.mkv"
  ffmpeg "${src[@]}" "$d/png_seq/frame_%04d.png"
  ffmpeg "${src[@]}" -f rawvideo -pix_fmt nv12 "$d/clip_320x240_nv12.yuv"
  ls "$d"
}

# ---- inside the container (root) -----------------------------------------------------------
inside() {
  local b="$1" clips="$2"; shift 2
  local base xvfb
  if command -v apt-get >/dev/null; then
    export DEBIAN_FRONTEND=noninteractive
    pkgs() { apt-get install -y -qq --no-install-recommends "$@" >/dev/null; }
    apt-get update -qq >/dev/null
    base="libgl1 libegl1 libfontconfig1 fonts-dejavu-core libglib2.0-0 libdbus-1-3 libx11-xcb1 libxext6
          libsm6 libice6 libxcb-glx0 libxcb-randr0 libxcb-shm0 libxcb-sync1 libxcb-xfixes0 libxcb-present0
          libxcb-dri2-0 libxcb-dri3-0"
    xvfb="xvfb libgl1-mesa-dri"
  else
    pkgs() { dnf install -y -q "$@" >/dev/null; }
    base="mesa-libGL mesa-libEGL fontconfig dejavu-sans-fonts glib2 dbus-libs libX11-xcb libXext libSM libICE libxcb"
    xvfb="xorg-x11-server-Xvfb mesa-dri-drivers"
  fi
  # shellcheck disable=SC2086
  pkgs $base "$@"
  . /etc/os-release
  echo "== $PRETTY_NAME, $(getconf GNU_LIBC_VERSION)"

  # Outputs are captured before grep: `| grep -q` under pipefail fails when grep stops early.
  local ldso=/lib64/ld-linux-x86-64.so.2 libpath="" deps
  deps="$(LD_TRACE_LOADED_OBJECTS=1 "$b/bin/scvp" 2>&1)"
  if grep -q 'libva.* not found' <<<"$deps"; then
    libpath="$b/lib/fallback"
    echo "libva: none on the system, the launcher adds the bundled one"
  else
    echo "libva: $(awk '$1 == "libva.so.2" {print $3}' <<<"$deps")"
  fi
  local missing=0 f
  shopt -s nullglob
  for f in "$b/bin/scvp" "$b"/lib/*.so* "$b"/lib/fallback/*.so* "$b"/plugins/*/*.so; do
    deps="$(LD_LIBRARY_PATH="$libpath" "$ldso" --list "$f" 2>&1)" || true
    if grep -q 'not found' <<<"$deps"; then
      echo "unresolved in ${f#"$b"/}:"; grep 'not found' <<<"$deps"
      missing=1
    fi
  done
  [[ $missing == 0 ]] || { echo "FAIL: libraries missing on the base system"; exit 1; }
  echo "libraries: every ELF resolves on the base system"

  # shellcheck disable=SC2086
  pkgs $xvfb
  local fail=0 log=""
  # grab LABEL PROGRAM ARGS...: the program opens ARGS and saves a frame that is not blank.
  grab() {
    local label="$1" png=/tmp/grab.png
    shift
    rm -f "$png"
    log="$(LD_BIND_NOW=1 timeout 120 bash /scripts/xvfb-run.sh "$@" --debug-size 700x600 --debug-grab "$png" 2>&1)" || true
    if grep -q '^debug-grab: saved' <<<"$log" && [[ -f "$png" && $(stat -c %s "$png") -gt 10000 ]]; then
      echo "ok   $label ($(stat -c %s "$png") bytes)"
    else
      echo "FAIL $label"; echo "$log" | tail -20; fail=1
    fi
  }
  local c
  for c in h264.mp4 hevc10.mkv vp9.webm av1.mkv png_seq clip_320x240_nv12.yuv; do
    grab "$c" "$b/scvp" "$clips/$c"
  done
  grab "A/B diff" "$b/scvp" "$clips/h264.mp4" "$clips/hevc10.mkv" --view diff
  grep -m1 '^scvp ' <<<"$log" || true
  # The AppImage, unpacked by its runtime (containers have no FUSE).
  if [[ -e /scvp.AppImage ]]; then
    grab "AppImage hevc10.mkv" env APPIMAGE_EXTRACT_AND_RUN=1 /scvp.AppImage "$clips/hevc10.mkv"
  fi
  [[ $fail == 0 ]] || exit 1
}

case "${1:-}" in
  clips) make_clips "$2" ;;
  --inside) shift; inside "$@" ;;
  ""|-h|--help) sed -n '2,15p' "$0"; exit 2 ;;
  *)
    image="$1" tarball="$(readlink -f "$2")" clips="$(readlink -f "$3")"; shift 3
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    tar -C "$tmp" -xzf "$tarball"
    bundle="$(find "$tmp" -mindepth 1 -maxdepth 1 -type d)"
    # The AppImage package-linux.sh made next to the tarball, if there.
    appimage="${tarball%-linux-x86_64.tar.gz}-x86_64.AppImage"
    mounts=(-v "$bundle:/bundle:ro" -v "$clips:/clips:ro" -v "$ROOT/scripts:/scripts:ro")
    [[ -f "$appimage" ]] && mounts+=(-v "$appimage:/scvp.AppImage:ro")
    docker run --rm "${mounts[@]}" "$image" bash /scripts/smoke-linux-bundle.sh --inside /bundle /clips "$@"
    ;;
esac
