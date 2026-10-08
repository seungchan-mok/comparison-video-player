#!/usr/bin/env bash
# Checks that the macOS dmg runs on a Mac with nothing else installed:
#  1. no Mach-O file in scvp.app references a library outside the app and macOS (Homebrew's
#     /opt/homebrew and /usr/local, MacPorts' /opt/local);
#  2. with a clip, the app from the dmg opens it and saves a frame while /opt/homebrew is moved
#     away (needs sudo: meant for CI runners), so nothing can still come from there.
#
#   scripts/check-macos-bundle.sh scvp-<version>-Darwin.dmg [clip]
set -euo pipefail
dmg="$1" clip="${2:+$(cd "$(dirname "$2")" && pwd)/$(basename "$2")}"
mnt="$(mktemp -d)"
hidden=""
cleanup() {
  [[ -z "$hidden" ]] || sudo mv /opt/homebrew.hidden /opt/homebrew
  hdiutil detach "$mnt" >/dev/null 2>&1 || true
}
trap cleanup EXIT
hdiutil attach -nobrowse -readonly -mountpoint "$mnt" "$dmg" >/dev/null
app="$(find "$mnt" -maxdepth 2 -name '*.app' -type d | head -1)"
[[ -n "$app" ]] || { echo "no .app in $dmg" >&2; exit 1; }

bad=0 n=0
while IFS= read -r -d '' f; do
  kind="$(file -b "$f")"
  [[ "$kind" == *Mach-O* ]] || continue
  n=$((n + 1))
  refs="$(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -E '^(/opt/homebrew|/usr/local|/opt/local)/' || true)"
  if [[ -n "$refs" ]]; then
    echo "${f#"$app"/} uses: $(echo "$refs" | tr '\n' ' ')"
    bad=1
  fi
done < <(find "$app" -type f -print0)
[[ $bad == 0 ]] || { echo "libraries outside the app" >&2; exit 1; }
echo "libraries: all $n Mach-O files reference only the app and macOS"

if [[ -n "$clip" ]]; then
  out="$(mktemp -d)"
  if [[ -d /opt/homebrew ]]; then
    sudo mv /opt/homebrew /opt/homebrew.hidden
    hidden=1
  fi
  code=0
  HOME="$out" "$app/Contents/MacOS/scvp" "$clip" --debug-size 700x600 --debug-grab "$out/grab.png" \
    >"$out/log.txt" 2>&1 || code=$?
  cat "$out/log.txt"
  if [[ $code != 0 || ! -f "$out/grab.png" || $(stat -f %z "$out/grab.png") -lt 10000 ]]; then
    echo "the app from the dmg did not show $(basename "$clip") (exit $code)" >&2
    exit 1
  fi
  echo "run: opened $(basename "$clip") with Homebrew out of the way ($(stat -f %z "$out/grab.png") bytes)"
fi
