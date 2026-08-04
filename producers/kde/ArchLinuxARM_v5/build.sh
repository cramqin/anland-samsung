#!/bin/bash
#
# build.sh - build patched KWin with the anland backend on Arch Linux / Arch Linux ARM.
#
# Usage:  ./build.sh [kwin-version]        (default 6.7.3)
# Env:    WORKDIR     work directory (default $HOME/anland-archbuild)
#         KWIN_PATCH  explicit path to kwin.patch
#         JOBS        parallel jobs (default nproc)
#
# It downloads the official KWin tarball, overlays the anland backend sources
# from ../anland_backend_archlinuxarm_v5 (via the "kwin" symlink), applies
# kwin.patch, builds with CMake/Ninja and installs with sudo.
#
# NOTE: files installed with `cmake --install` are not tracked by pacman.
# Keep `IgnorePkg = kwin` in /etc/pacman.conf, or build a proper package
# (PKGBUILD) so pacman knows about the patched kwin.
set -u

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKDIR="${WORKDIR:-$HOME/anland-archbuild}"
VERSION="${1:-6.7.3}"
JOBS="${JOBS:-$(nproc)}"

log()  { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m[warn] %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m[error] %s\033[0m\n' "$*" >&2; exit 1; }

mkdir -p "$WORKDIR"

TARBALL="$WORKDIR/kwin-$VERSION.tar.xz"
URL="https://download.kde.org/stable/plasma/$VERSION/kwin-$VERSION.tar.xz"
TREE="$WORKDIR/kwin-$VERSION"
PATCH="${KWIN_PATCH:-$SCRIPT_DIR/kwin.patch}"
BACKEND_SRC="$SCRIPT_DIR/kwin"   # symlink -> ../anland_backend_archlinuxarm_v5

[ -f "$PATCH" ] || die "patch not found: $PATCH"
[ -d "$BACKEND_SRC/src/backends/anland" ] || die "backend sources not found: $BACKEND_SRC"

if [ ! -f "$TARBALL" ]; then
    log "Downloading $URL"
    if ! wget -q --show-progress "$URL" -O "$TARBALL"; then
        rm -f "$TARBALL"
        curl -fL "$URL" -o "$TARBALL" || die "download failed: $URL"
    fi
fi

if [ ! -d "$TREE" ]; then
    log "Extracting $TARBALL"
    tar -xf "$TARBALL" -C "$WORKDIR"
fi

log "Overlaying anland backend sources"
cp -a "$BACKEND_SRC/." "$TREE/"

log "Applying $PATCH"
if ( cd "$TREE" && patch -p1 --forward --reject-file=- < "$PATCH" ); then
    :
elif grep -rqF 'add_subdirectory(anland)' "$TREE/src/backends/CMakeLists.txt"; then
    warn "patch looks already applied, continuing"
else
    die "kwin.patch did not apply cleanly"
fi

log "Configuring (CMake/Ninja)"
cmake -S "$TREE" -B "$TREE/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib || die "cmake configure failed"

log "Building (-j$JOBS)"
cmake --build "$TREE/build" -j "$JOBS" || die "build failed"

log "Installing (sudo cmake --install)"
$SUDO cmake --install "$TREE/build" || die "install failed"

cat <<EOF

Done. Patched kwin_wayland is installed at /usr/bin/kwin_wayland.
Start it with startup.sh, or adapt your existing systemd unit.
Keep IgnorePkg containing "kwin" in /etc/pacman.conf, since pacman no longer
tracks these files.
EOF
