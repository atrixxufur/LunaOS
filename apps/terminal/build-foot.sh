#!/usr/bin/env bash
# =============================================================================
# build-foot.sh — Build foot terminal emulator for LunaOS
#
# foot is a fast, minimal Wayland-native terminal with:
#   - Pure Wayland client (no X11 dependency)
#   - GPU-accelerated text rendering via OpenGL ES (lavapipe on LunaOS)
#   - 256 color + true color support
#   - Scrollback, copy/paste, URL detection
#
# Deps: wayland-client, pixman, libxkbcommon, fontconfig, freetype2
# =============================================================================

set -euo pipefail
LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ROOTFS="${LUNA_ROOT}/build/rootfs"
PREFIX="${ROOTFS}/usr/local"
SRC="${LUNA_ROOT}/build/apps-src/foot"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

log() { echo -e "\033[0;32m[foot]\033[0m $*"; }

log "Building foot terminal emulator"
export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig"
export CFLAGS="-arch x86_64"

if [[ ! -d "$SRC/.git" ]]; then
    git clone --depth 1 --branch 1.19.0 \
        https://codeberg.org/dnkl/foot.git "$SRC" --quiet
fi

meson setup "${LUNA_ROOT}/build/obj/foot" "$SRC" \
    --prefix="$PREFIX" \
    --buildtype=release \
    -Dterminfo=disabled \
    -Dfcft:text-shaping=disabled \
    -Dfcft:run-shaping=disabled 2>&1 | tail -5

ninja -C "${LUNA_ROOT}/build/obj/foot" -j"$JOBS"
ninja -C "${LUNA_ROOT}/build/obj/foot" install

# Install default config
mkdir -p "${ROOTFS}/etc/foot"
cat > "${ROOTFS}/etc/foot/foot.ini" <<'INI'
# /etc/foot/foot.ini — LunaOS default foot configuration
[main]
font=monospace:size=12
pad=4x4
term=foot

[colors]
background=0d1117
foreground=e6edf3
regular0=21262d
regular1=ff7b72
regular2=3fb950
regular3=d29922
regular4=58a6ff
regular5=bc8cff
regular6=39d353
regular7=b1bac4

[key-bindings]
scrollback-up-page=Shift+Page_Up
scrollback-down-page=Shift+Page_Down
clipboard-copy=Control+Shift+c
clipboard-paste=Control+Shift+v
INI

# Desktop file for launcher
mkdir -p "${PREFIX}/share/applications"
cat > "${PREFIX}/share/applications/foot.desktop" <<'DESKTOP'
[Desktop Entry]
Name=Terminal
Exec=foot
Icon=terminal
Type=Application
Categories=System;TerminalEmulator;
DESKTOP

log "foot installed: ${PREFIX}/bin/foot"
