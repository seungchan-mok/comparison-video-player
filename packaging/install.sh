#!/bin/sh
# Installs SCVP for the current user, no root needed: the application goes to ~/.local/opt/scvp,
# and an applications-menu entry, icons and file types are registered so SCVP can be started
# from the desktop, offered in "Open With" and chosen as the default video player.
#
# A plain install changes no defaults: desktops pick the first registered application for types
# without an explicit default, and user applications come first, so the defaults in effect
# before are pinned explicitly. SCVP only becomes the handler where nothing handled a type
# before (raw .h264/.h265/.yuv/.y4m/...).
#
#   ./install.sh                 install or update
#   ./install.sh --set-default   install, then make SCVP the default for the video types it opens
#   ./install.sh --uninstall     remove it all again (default-application entries included)
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
CONFIG="${XDG_CONFIG_HOME:-$HOME/.config}"
APPDIR="$HOME/.local/opt/scvp"
BINDIR="$HOME/.local/bin"
DESKTOP="$DATA/applications/scvp.desktop"

refresh() {
    command -v update-mime-database >/dev/null 2>&1 && update-mime-database "$DATA/mime" >/dev/null 2>&1 || true
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$DATA/applications" >/dev/null 2>&1 || true
    command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t -f "$DATA/icons/hicolor" >/dev/null 2>&1 || true
}

mime_types() {
    sed -n 's/^MimeType=//p' "$1" | tr ';' ' '
}

uninstall() {
    rm -f "$DESKTOP" "$DATA/mime/packages/scvp.xml"
    find "$DATA/icons/hicolor" -path '*/apps/scvp.png' -delete 2>/dev/null || true
    if [ -L "$BINDIR/scvp" ] && [ "$(readlink "$BINDIR/scvp")" = "$APPDIR/scvp" ]; then
        rm -f "$BINDIR/scvp"
    fi
    # Default-application entries pointing at SCVP (written by --set-default or the app).
    for list in "$CONFIG/mimeapps.list" "$DATA/applications/mimeapps.list"; do
        [ -f "$list" ] || continue
        sed -i -e '/^[^=]*=scvp\.desktop;*$/d' -e 's/\([=;]\)scvp\.desktop;/\1/g' -e 's/;scvp\.desktop$//' "$list"
    done
    refresh
    echo "SCVP removed."
    # Last, as this script may itself live in the folder being removed.
    exec rm -rf "$APPDIR"
}

# The application a double-click on this type opens, as the file manager resolves it (GIO
# follows type aliases, which "xdg-mime query" does not).
query_default() {
    if command -v gio >/dev/null 2>&1; then
        LC_ALL=C gio mime "$1" 2>/dev/null | sed -n 's/^Default application for .*: //p'
    else
        xdg-mime query default "$1" 2>/dev/null || true
    fi
}

# "type default" for every type SCVP handles, as resolved right now.
current_defaults() {
    command -v xdg-mime >/dev/null 2>&1 || return 0
    for t in $(mime_types "$1"); do
        echo "$t $(query_default "$t")"
    done
}

# Pins the recorded defaults that registering SCVP would otherwise take over.
keep_defaults() {
    command -v xdg-mime >/dev/null 2>&1 || return 0
    kept=0
    while read -r t prev; do
        [ -n "$prev" ] && [ "$prev" != scvp.desktop ] || continue
        if [ "$(query_default "$t")" = scvp.desktop ]; then
            xdg-mime default "$prev" "$t"
            kept=$((kept + 1))
        fi
    done < "$1"
    [ "$kept" -eq 0 ] || echo "Kept your current default players for $kept video types."
}

install_files() {
    if [ ! -x "$HERE/bin/scvp" ] || [ ! -f "$HERE/share/applications/scvp.desktop" ]; then
        echo "Run this script from the unpacked SCVP folder (bin/, lib/, share/ next to it)." >&2
        exit 1
    fi
    if [ "$HERE" != "$APPDIR" ]; then
        mkdir -p "$(dirname "$APPDIR")"
        rm -rf "$APPDIR.new"
        cp -a "$HERE/." "$APPDIR.new"
        rm -rf "$APPDIR"
        mv "$APPDIR.new" "$APPDIR"
    fi

    before="$(mktemp)"
    trap 'rm -f "$before"' EXIT
    current_defaults "$APPDIR/share/applications/scvp.desktop" > "$before"

    mkdir -p "$DATA/applications" "$DATA/mime/packages" "$DATA/icons/hicolor"
    # The packaged entry runs "scvp" from PATH; point it at the installed launcher.
    sed -e "s|^Exec=.*|Exec=\"$APPDIR/scvp\" %F|" -e "s|^TryExec=.*|TryExec=$APPDIR/scvp|" \
        "$APPDIR/share/applications/scvp.desktop" > "$DESKTOP"
    chmod 644 "$DESKTOP"
    cp "$APPDIR/share/mime/packages/scvp.xml" "$DATA/mime/packages/scvp.xml"
    (cd "$APPDIR/share/icons/hicolor" && find . -name scvp.png) | while read -r icon; do
        mkdir -p "$DATA/icons/hicolor/$(dirname "$icon")"
        cp "$APPDIR/share/icons/hicolor/$icon" "$DATA/icons/hicolor/$icon"
    done
    # Command line too (~/.local/bin is on PATH in most distributions once it exists).
    mkdir -p "$BINDIR"
    if [ ! -e "$BINDIR/scvp" ] || [ -L "$BINDIR/scvp" ]; then
        ln -sfn "$APPDIR/scvp" "$BINDIR/scvp"
    fi
    refresh
    [ "${1:-}" = keep ] && keep_defaults "$before"
    echo "SCVP installed: find it in the applications menu, or right-click a video > Open With."
}

set_default() {
    # shellcheck disable=SC2046
    xdg-mime default scvp.desktop $(mime_types "$DESKTOP")
    echo "SCVP is now the default application for videos (change it back in Settings > Default Applications)."
}

case "${1:-}" in
    --uninstall) uninstall ;;
    --set-default) install_files; set_default ;;
    "") install_files keep ;;
    *) echo "usage: $0 [--set-default | --uninstall]" >&2; exit 2 ;;
esac
