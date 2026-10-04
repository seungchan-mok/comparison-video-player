#!/usr/bin/env bash
# Runs a command on a private virtual X display (Xvfb + Mesa llvmpipe), so GUI tests never
# open windows on the real desktop. Uses the system Xvfb or the one from .deps (fetch-ubuntu-deps.sh).
#
#   scripts/xvfb-run.sh [--size WxH] command args...
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIZE="1920x1080"
if [[ "${1:-}" == "--size" ]]; then SIZE="$2"; shift 2; fi
XVFB="$(command -v Xvfb || true)"
[[ -z "$XVFB" && -x "$ROOT/.deps/root/usr/bin/Xvfb" ]] && XVFB="$ROOT/.deps/root/usr/bin/Xvfb"
if [[ -z "$XVFB" ]]; then echo "Xvfb not found (apt install xvfb, or scripts/fetch-ubuntu-deps.sh)" >&2; exit 2; fi

# Pick a free display number.
for n in $(seq 90 130); do
  [[ -e "/tmp/.X$n-lock" || -e "/tmp/.X11-unix/X$n" ]] || { DISP=":$n"; break; }
done
"$XVFB" "$DISP" -screen 0 "${SIZE}x24" -nolisten tcp -noreset +extension GLX >/tmp/scvp-xvfb-${DISP#:}.log 2>&1 &
XPID=$!
trap 'kill $XPID 2>/dev/null; wait $XPID 2>/dev/null || true' EXIT
for _ in $(seq 1 100); do [[ -e "/tmp/.X11-unix/X${DISP#:}" ]] && break; sleep 0.05; done
export DISPLAY="$DISP"
export __GLX_VENDOR_LIBRARY_NAME=mesa LIBGL_ALWAYS_SOFTWARE=1 QT_QPA_PLATFORM=xcb
"$@"
