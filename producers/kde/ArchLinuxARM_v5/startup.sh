#!/bin/bash
#
# startup.sh - start kwin_wayland with the built-in anland backend
# (Arch Linux / Arch Linux ARM, Adreno kgsl stack).
#
# Usage:
#   ./startup.sh [socket path]          (default /run/display.sock)
# Env:
#   KWIN_BIN     path to kwin_wayland (default: the one installed in PATH)
#   ANLAND_SOCKET, ANLAND_DRM_DEVICE
set -u

SOCK="${1:-${ANLAND_SOCKET:-/run/display.sock}}"
KWIN_BIN="${KWIN_BIN:-kwin_wayland}"

command -v "$KWIN_BIN" >/dev/null 2>&1 || {
    echo "kwin_wayland not found in PATH; set KWIN_BIN=/path/to/kwin_wayland" >&2
    exit 1
}

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
if [ ! -d "$XDG_RUNTIME_DIR" ]; then
    sudo mkdir -p "$XDG_RUNTIME_DIR"
    sudo chown "$(id -u):$(id -g)" "$XDG_RUNTIME_DIR"
    sudo chmod 0700 "$XDG_RUNTIME_DIR"
fi

# kgsl/turnip (Adreno) environment
export MESA_LOADER_DRIVER_OVERRIDE=kgsl
export GALLIUM_DRIVER=kgsl
export FD_FORCE_KGSL=1
export MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=1
export FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1
export XCURSOR_THEME=breeze_cursors
export XCURSOR_SIZE=24
export QT_QPA_PLATFORM=wayland
export ANLAND_SOCKET="$SOCK"
export ANLAND_DRM_DEVICE="${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}"
unset DISPLAY

echo "==> $KWIN_BIN --anland (socket=$SOCK, drm=$ANLAND_DRM_DEVICE)"
"$KWIN_BIN" --anland &
KWIN_PID=$!

WAYLAND_SOCKET=""
for _ in $(seq 1 30); do
    sleep 1
    for wl in "$XDG_RUNTIME_DIR"/wayland-*; do
        [ -S "$wl" ] || continue
        case "$wl" in *.lock) continue ;; esac
        WAYLAND_SOCKET="$(basename "$wl")"
        break 2
    done
    kill -0 "$KWIN_PID" 2>/dev/null || break
done

if [ -z "$WAYLAND_SOCKET" ]; then
    echo "ERROR: no wayland socket found; kwin_wayland may have failed" >&2
    wait "$KWIN_PID"
    exit 1
fi

echo "==> wayland socket: $WAYLAND_SOCKET"
export WAYLAND_DISPLAY="$WAYLAND_SOCKET"

dbus-run-session startplasma-wayland
wait "$KWIN_PID"
