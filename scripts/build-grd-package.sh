#!/usr/bin/env bash
# Build gnome-remote-desktop with the patches Mali/RKVENC needs to reach the
# hardware H.264 encoder and describe the visible AVC frame size correctly.
#
# They are worked around in this fork only because upstream has not fixed them
# yet; see config/patches/gnome-remote-desktop-50.2/README.md for what has to
# happen before each can be dropped.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
# Cross-build on the host; never run a compiler under qemu. See CLAUDE.md.
# shellcheck source=lib/arm64-cross.sh
source "$ROOT/scripts/lib/arm64-cross.sh"

C="$ROOT/build/cross-chroot"
PATCH_DIR="$ROOT/config/patches/gnome-remote-desktop-50.2"
UPSTREAM_VERSION="50.2-0ubuntu0.1"
PACKAGE_VERSION="${GRD_PACKAGE_VERSION:-${UPSTREAM_VERSION}+orangepi5b3}"
JOBS="${GRD_BUILD_JOBS:-$(nproc)}"
SRC="/build/gnome-remote-desktop-50.2"

require_cross_prereqs
cleanup() { set +e; unmount_chroot; }
trap cleanup EXIT

prepare_cross_chroot gnome-remote-desktop
export DEBIAN_FRONTEND=noninteractive

fetch_and_unpack_source_cross gnome-remote-desktop \
 "gnome-remote-desktop_${UPSTREAM_VERSION}.dsc"

[[ -d "$C$SRC" ]] || { echo "unexpected source tree layout under $C/build"; exit 1; }
[[ $(chroot "$C" sh -c "cd $SRC && dpkg-parsechangelog -S Version") == "$UPSTREAM_VERSION" ]] || {
 echo "Ubuntu shipped a different gnome-remote-desktop than this patch queue targets"
 exit 1; }

queue_patches "$SRC" "$PATCH_DIR"

chroot "$C" sh -c "cd $SRC && \
 DEBEMAIL='pencil.bsp@gmail.com' DEBFULLNAME='Orange Pi 5B Builder' \
 dch --newversion '$PACKAGE_VERSION' --distribution resolute \
 'Reach the RKVENC hardware H.264 encoder on Mali-G610 and crop AVC padding.'"

chroot "$C" sh -c "cd $SRC && \
 DEB_BUILD_OPTIONS='parallel=$JOBS nocheck' DEB_BUILD_PROFILES='nocheck' \
 dpkg-buildpackage -aarm64 -b -uc -us"

mkdir -p "$ROOT/output/debs"
deb="gnome-remote-desktop_${PACKAGE_VERSION}_arm64.deb"
[[ -s "$C/build/$deb" ]] || { echo "Package not produced: $deb"; exit 1; }
install -m 0644 "$C/build/$deb" "$ROOT/output/debs/$deb"

# The VA-API path is the whole point; refuse a package built without it.
dpkg-deb --fsys-tarfile "$ROOT/output/debs/$deb" |
 tar -xO ./usr/libexec/gnome-remote-desktop-daemon > "$C/build/daemon.bin"
for sym in vaCreateConfig vaExportSurfaceHandle vaBeginPicture; do
 grep -aq "$sym" "$C/build/daemon.bin" || {
  echo "Daemon has no $sym: built without the VA-API encoder"; exit 1; }
done

echo "GNOME Remote Desktop package: $ROOT/output/debs/$deb"
sha256sum "$ROOT/output/debs/$deb"
