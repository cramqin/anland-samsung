#!/bin/bash
# Native path: kwin_wayland IS the top-level compositor, talking to the display
# daemon directly through its built-in "anland" backend (--anland). There is no
# weston layer and no nested kwin — kwin replaces both. The patched kwin_wayland
# must be installed (see kdefix/build.sh, which builds the .deb with the anland
# backend baked in).
SOCK="${1:-/run/display.sock}"

pkill -9 plasmashell 2>/dev/null; pkill -9 kwin_wayland 2>/dev/null; pkill -9 startplasma 2>/dev/null
sleep 1
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
mkdir -p "$XDG_RUNTIME_DIR"; chmod 0700 "$XDG_RUNTIME_DIR"
unset DISPLAY
export ANLAND_SOCKET=/run/display.sock
export ANLAND=1
export ANLAND_DRM_DEVICE=/dev/dri/renderD128
export MESA_LOADER_DRIVER_OVERRIDE=kgsl GALLIUM_DRIVER=kgsl FD_FORCE_KGSL=1
export QT_QPA_PLATFORM=wayland
# Tell xdg-desktop-portal this is a KDE session, so it loads the kde backend and
# kde-portals.conf. Without this, portal detection falls back to the gtk backend,
# which is X11-only and crashes ("cannot open display"), hanging portal startup
# for 120s and leaving the workspace black. Required for the camera portal too.
export XDG_CURRENT_DESKTOP=KDE
export XDG_SESSION_DESKTOP=KDE
rm -f "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null
dbus-run-session startplasma-wayland
