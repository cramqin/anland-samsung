#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="/opt/weston-anland"
BUILD_DIR="$SCRIPT_DIR/weston/builddir"

echo "=== Installing build dependencies ==="
apt-get update -qq
apt-get install -y -qq \
    build-essential meson ninja-build pkg-config \
    libwayland-dev libpixman-1-dev libxkbcommon-dev \
    libinput-dev libevdev-dev libdrm-dev libgbm-dev \
    libudev-dev libseat-dev libcairo2-dev \
    libjpeg-dev libwebp-dev libpam0g-dev \
    libgles-dev libegl-dev libvulkan-dev glslang-tools \
    libxcb-composite0-dev libxcb-shape0-dev libxcb-xfixes0-dev \
    libxcursor-dev libxcb1-dev \
    libpango1.0-dev libglib2.0-dev \
    libwayland-cursor0 wayland-protocols libwayland-bin \
    libpng-dev libfontconfig-dev libfreetype-dev \
    hwdata

echo "=== Patching wayland-protocols ==="
cp -v "$SCRIPT_DIR/wayland-protocols-override/staging/color-representation/color-representation-v1.xml" \
    /usr/share/wayland-protocols/staging/color-representation/color-representation-v1.xml

echo "=== Fixing subproject wraps ==="
rm -f "$SCRIPT_DIR/weston/subprojects/edid-decode.wrap"

echo "=== Configuring weston ==="
MESON_OPTS=(
    --prefix="$PREFIX"
    -Dbackend-drm=false
    -Dbackend-headless=false
    -Dbackend-pipewire=false
    -Dbackend-rdp=false
    -Dbackend-vnc=false
    -Dbackend-wayland=false
    -Dbackend-x11=false
    -Dbackend-anland=true
    -Dbackend-default=auto

    -Drenderer-gl=true
    -Drenderer-vulkan=true
    -Dxwayland=true
    -Dcolor-management-lcms=false
    -Dimage-jpeg=true
    -Dimage-webp=true
    -Dshell-desktop=true
    -Dshell-kiosk=true
    -Dshell-ivi=false
    -Dshell-lua=false
    -Ddemo-clients=false
    -Dsimple-clients=[]
    -Dtools=terminal
    -Dsystemd=false
    -Dtests=false
    -Ddoc=false
    -Dperfetto=false
    -Ddeprecated-remoting=false
    -Ddeprecated-pipewire=false
)

if [ -d "$BUILD_DIR" ]; then
    meson setup --reconfigure "$BUILD_DIR" "$SCRIPT_DIR/weston" "${MESON_OPTS[@]}"
else
    meson setup "$BUILD_DIR" "$SCRIPT_DIR/weston" "${MESON_OPTS[@]}"
fi

echo "=== Building weston ==="
ninja -C "$BUILD_DIR" -j$(nproc)

echo "=== Installing to $PREFIX ==="
ninja -C "$BUILD_DIR" install

ldconfig "$PREFIX/lib/aarch64-linux-gnu"

LIBDIR="$PREFIX/lib/aarch64-linux-gnu"
cat > "$PREFIX/start.sh" << EOF
#!/bin/bash
SOCK="\${1:-/run/display.sock}"

export LD_LIBRARY_PATH="$LIBDIR:$LIBDIR/libweston-16:$LIBDIR/weston:\$LD_LIBRARY_PATH"
export XDG_RUNTIME_DIR="\${XDG_RUNTIME_DIR:-/tmp}"
export WESTON_MODULE_MAP="anland-backend.so=$LIBDIR/libweston-16/anland-backend.so;gl-renderer.so=$LIBDIR/libweston-16/gl-renderer.so;vulkan-renderer.so=$LIBDIR/libweston-16/vulkan-renderer.so;desktop-shell.so=$LIBDIR/weston/desktop-shell.so;xwayland.so=$LIBDIR/libweston-16/xwayland.so"

# Native freedreno GL on the kgsl GPU node (loader name "kgsl"). GALLIUM_DRIVER
# is set so EGL does NOT silently fall back to zink if freedreno init fails
# (we require freedreno end-to-end).
export MESA_LOADER_DRIVER_OVERRIDE=kgsl
export GALLIUM_DRIVER=kgsl
# Force the freedreno DRM layer to open /dev/kgsl-3d0 directly. EGL/GBM clients
# (Xwayland glamor, GL apps) are otherwise handed the sde-kms display node
# (renderD128), which drmGetVersion reports as "msm" with no GPU behind it ->
# half-initialised screen -> crash. The Adreno GPU is kgsl-only on this device.
export FD_FORCE_KGSL=1
export MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=1
export MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE_DRI3=1

shift 2>/dev/null || true
exec $PREFIX/bin/weston -Banland-backend.so --disp-sock="\$SOCK" --xwayland "\$@"
EOF
chmod +x "$PREFIX/start.sh"

cat > "$PREFIX/start_kde.sh" << EOF
#!/bin/bash
SOCK="\${1:-/run/display.sock}"

export LD_LIBRARY_PATH="$LIBDIR:$LIBDIR/libweston-16:$LIBDIR/weston:\$LD_LIBRARY_PATH"
export XDG_RUNTIME_DIR="\${XDG_RUNTIME_DIR:-/run/user/\$(id -u)}"
mkdir -p "\$XDG_RUNTIME_DIR"
chmod 0700 "\$XDG_RUNTIME_DIR"
export WESTON_MODULE_MAP="anland-backend.so=$LIBDIR/libweston-16/anland-backend.so;gl-renderer.so=$LIBDIR/libweston-16/gl-renderer.so;vulkan-renderer.so=$LIBDIR/libweston-16/vulkan-renderer.so;xwayland.so=$LIBDIR/libweston-16/xwayland.so;kiosk-shell.so=$LIBDIR/weston/kiosk-shell.so"
unset DISPLAY

# Use the native freedreno GL driver on the kgsl GPU node. The gallium driver
# is registered under the name "kgsl" (an alias of msm in the patched mesa from
# mesa-for-android-container dev branch) — NOT "freedreno", which is not a valid
# loader name and would fall through to zink->llvmpipe.
# GALLIUM_DRIVER pins it so EGL won't silently fall back to zink on failure.
export MESA_LOADER_DRIVER_OVERRIDE=kgsl
export GALLIUM_DRIVER=kgsl
export FD_FORCE_KGSL=1
export MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=1

WESTON_PID=
KDE_PID=

cleanup() {
    [ -n "\$KDE_PID" ]    && kill "\$KDE_PID"    2>/dev/null
    [ -n "\$WESTON_PID" ] && kill "\$WESTON_PID" 2>/dev/null
    sleep 0.3
    [ -n "\$KDE_PID" ]    && kill -9 "\$KDE_PID"    2>/dev/null
    [ -n "\$WESTON_PID" ] && kill -9 "\$WESTON_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

rm -f "\${XDG_RUNTIME_DIR}"/wayland-* 2>/dev/null

$PREFIX/bin/weston -Banland-backend.so --disp-sock="\$SOCK" --shell=kiosk-shell.so --no-config &
WESTON_PID=\$!

WESTON_SOCKET=""
for i in \$(seq 1 300); do
    sleep 1
    for wl in "\${XDG_RUNTIME_DIR}"/wayland-*; do
        [ -S "\$wl" ] || continue
        WESTON_SOCKET="\$(basename "\$wl")"
        break 2
    done
done

if [ -z "\$WESTON_SOCKET" ]; then
    echo "ERROR: weston wayland socket not found"
    wait "\$WESTON_PID"
    exit 1
fi
echo "weston socket: \$WESTON_SOCKET"

export WAYLAND_DISPLAY="\$WESTON_SOCKET"
# Native freedreno GL on the kgsl GPU node. Registered loader name is "kgsl"
# (alias of msm in the patched mesa), NOT "freedreno". GALLIUM_DRIVER pins it
# so EGL won't silently fall back to zink if freedreno init fails.
export MESA_LOADER_DRIVER_OVERRIDE=kgsl
export GALLIUM_DRIVER=kgsl
export FD_FORCE_KGSL=1
# Kept for any client that still falls back to the zink path: force the default
# Vulkan device (plain var = surfaceless, _DRI3 = Xwayland GBM/DRI3 glamor path)
# so it lands on turnip instead of llvmpipe.
export MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=1
export MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE_DRI3=1
export QT_QPA_PLATFORM=wayland
export FD_MESA_DEBUG=notile
export XWAYLAND_NO_DRI3_MODIFIERS=1
dbus-run-session startplasma-wayland &
KDE_PID=\$!

wait "\$WESTON_PID"
EOF
chmod +x "$PREFIX/start_kde.sh"

echo ""
echo "=== Done ==="
echo "  Installed to: $PREFIX"
echo "  Start:        $PREFIX/start.sh [socket-path]"
echo "  Start KDE:    $PREFIX/start_kde.sh [socket-path]"
echo "  Default sock: /run/display.sock"
