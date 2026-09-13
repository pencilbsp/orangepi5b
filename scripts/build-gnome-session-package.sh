#!/usr/bin/env bash
# Build gnome-session with a guard that keeps the per-user D-Bus alive while
# GDM hands a login from an existing local session to Remote Login.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
# Cross-build on the host; never run a compiler under qemu. See CLAUDE.md.
# shellcheck source=lib/arm64-cross.sh
source "$ROOT/scripts/lib/arm64-cross.sh"

C="$ROOT/build/cross-chroot"
PATCH_DIR="$ROOT/config/patches/gnome-session-50.1"
UPSTREAM_VERSION="50.1-0ubuntu0.1"
PACKAGE_VERSION="${GNOME_SESSION_PACKAGE_VERSION:-${UPSTREAM_VERSION}+orangepi5b1}"
JOBS="${GNOME_SESSION_BUILD_JOBS:-$(nproc)}"
SRC="/build/gnome-session-50.1"

require_cross_prereqs
cleanup() { set +e; unmount_chroot; }
trap cleanup EXIT

# apt-get build-dep -a arm64 cannot resolve this source package because
# dh-sequence-user-session-migration and xmlto are Architecture: all/native
# build tools without a usable :arm64 variant. Install host tools and target
# libraries explicitly; dpkg-buildpackage still selects the arm64 toolchain.
prepare_cross_chroot - \
 debhelper dh-sequence-gnome dh-sequence-user-session-migration \
 meson pkgconf xmlto xsltproc systemd-dev \
 libdbus-1-dev:arm64 libgl-dev:arm64 libgles-dev:arm64 \
 libglib2.0-dev:arm64 libgnome-desktop-4-dev:arm64 \
 libice-dev:arm64 libsm-dev:arm64 libsystemd-dev:arm64
export DEBIAN_FRONTEND=noninteractive

fetch_and_unpack_source_cross gnome-session \
 "gnome-session_${UPSTREAM_VERSION}.dsc"

[[ -d "$C$SRC" ]] || { echo "unexpected source tree layout under $C/build"; exit 1; }
[[ $(chroot "$C" sh -c "cd $SRC && dpkg-parsechangelog -S Version") == "$UPSTREAM_VERSION" ]] || {
 echo "Ubuntu shipped a different gnome-session than this patch queue targets"
 exit 1
}

queue_patches "$SRC" "$PATCH_DIR"

chroot "$C" sh -c "cd $SRC && \
 DEBEMAIL='pencil.bsp@gmail.com' DEBFULLNAME='Orange Pi 5B Builder' \
 dch --newversion '$PACKAGE_VERSION' --distribution resolute \
 'Keep the shared user D-Bus alive during GDM remote-login handoff.'"

chroot "$C" sh -c "cd $SRC && \
 DEB_BUILD_OPTIONS='parallel=$JOBS nocheck' DEB_BUILD_PROFILES='nocheck' \
 dpkg-buildpackage -aarm64 -b -d -uc -us"

mkdir -p "$ROOT/output/debs"
deb="gnome-session-bin_${PACKAGE_VERSION}_arm64.deb"
[[ -s "$C/build/$deb" ]] || { echo "Package not produced: $deb"; exit 1; }
install -m 0644 "$C/build/$deb" "$ROOT/output/debs/$deb"

dpkg-deb --fsys-tarfile "$ROOT/output/debs/$deb" |
 tar -xO ./usr/libexec/gnome-session-ctl > "$C/build/gnome-session-ctl.bin"
grep -aq 'Not restarting DBus: session' "$C/build/gnome-session-ctl.bin" || {
 echo "gnome-session-ctl does not contain the remote-handoff guard"; exit 1; }

echo "GNOME Session package: $ROOT/output/debs/$deb"
sha256sum "$ROOT/output/debs/$deb"
