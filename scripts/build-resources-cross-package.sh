#!/usr/bin/env bash
# Cross-build GNOME Resources for arm64 on the x86 host.
#
# The compiler runs natively and emits arm64 binaries, so this avoids the slow
# qemu path while still using the same Ubuntu 26.04 dependency set as the image.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
# shellcheck source=lib/arm64-cross.sh
source "$ROOT/scripts/lib/arm64-cross.sh"

C="$ROOT/build/cross-chroot"
PATCH_DIR="$ROOT/config/patches/resources-1.10.2"
UPSTREAM_VERSION="1.10.2-0ubuntu3.1"
PACKAGE_VERSION="${RESOURCES_PACKAGE_VERSION:-${UPSTREAM_VERSION}+orangepi5b2}"
JOBS="${RESOURCES_BUILD_JOBS:-$(nproc)}"
SRC="/build/resources-1.10.2"

prepare_resources_cross_chroot() {
 local marker="$C/.resources-cross-ready"

 if [[ -e "$marker" ]]; then
  mount_api
  return 0
 fi

 unmount_chroot
 rm -rf "$C"
 mkdir -p "$C"
 tar -xpf "$ROOT/$CROSS_BASE_TARBALL" -C "$C"
 rm -f "$C/etc/resolv.conf"; cp -L /etc/resolv.conf "$C/etc/resolv.conf"
 cat > "$C/etc/apt/sources.list.d/ubuntu.sources" <<'APT'
Types: deb deb-src
URIs: http://archive.ubuntu.com/ubuntu
Suites: resolute resolute-updates resolute-security
Components: main universe restricted multiverse
Architectures: amd64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb deb-src
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: resolute resolute-updates resolute-security
Components: main universe restricted multiverse
Architectures: arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
APT
 printf '#!/bin/sh\nexit 101\n' > "$C/usr/sbin/policy-rc.d"
 chmod 755 "$C/usr/sbin/policy-rc.d"

 mount_api
 chroot "$C" dpkg --print-architecture | grep -Fqx amd64
 chroot "$C" dpkg --add-architecture arm64
 chroot "$C" apt-get update -o Acquire::Languages=none
 chroot "$C" apt-get install -y --no-install-recommends \
  build-essential crossbuild-essential-arm64 fakeroot devscripts dpkg-dev quilt \
  debhelper dh-sequence-gnome desktop-file-utils cargo rustc meson pkgconf \
  libstd-rust-dev:arm64 libglib2.0-dev libglib2.0-dev:arm64 libgtk-4-dev:arm64 \
  libadwaita-1-dev:arm64 libsoup-3.0-dev:arm64
 touch "$marker"
}

require_cross_prereqs
cleanup() { set +e; unmount_chroot; }
trap cleanup EXIT

export DEBIAN_FRONTEND=noninteractive
prepare_resources_cross_chroot

fetch_and_unpack_source_cross resources "resources_${UPSTREAM_VERSION}.dsc"

[[ -d "$C$SRC" ]] || { echo "unexpected source tree layout under $C/build"; exit 1; }
[[ $(chroot "$C" sh -c "cd $SRC && dpkg-parsechangelog -S Version") == "$UPSTREAM_VERSION" ]] || {
 echo "Ubuntu shipped a different resources version than this patch queue targets"; exit 1; }

queue_patches "$SRC" "$PATCH_DIR"

chroot "$C" sh -c "cd $SRC && \
 DEBEMAIL='pencil.bsp@gmail.com' DEBFULLNAME='Orange Pi 5B Builder' \
 dch --newversion '$PACKAGE_VERSION' --distribution resolute \
 'cpu: recognize Orange Pi 5B thermal sensors.'"

chroot "$C" sh -c "cd $SRC && \
 DEB_BUILD_OPTIONS='parallel=$JOBS nocheck' \
 CC='aarch64-linux-gnu-gcc' CXX='aarch64-linux-gnu-g++' \
 AR='aarch64-linux-gnu-ar' PKG_CONFIG='aarch64-linux-gnu-pkg-config' \
 PKG_CONFIG_ALLOW_CROSS='1' \
 PKG_CONFIG_aarch64_unknown_linux_gnu='aarch64-linux-gnu-pkg-config' \
 CARGO_BUILD_TARGET='aarch64-unknown-linux-gnu' \
 CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER='aarch64-linux-gnu-gcc' \
 dpkg-buildpackage -aarm64 -b -d -uc -us"

mkdir -p "$ROOT/output/debs"
deb="resources_${PACKAGE_VERSION}_arm64.deb"
[[ -s "$C/build/$deb" ]] || { echo "Package not produced: $deb"; exit 1; }
install -m 0644 "$C/build/$deb" "$ROOT/output/debs/$deb"

dpkg-deb --fsys-tarfile "$ROOT/output/debs/$deb" |
 tar -t ./usr/bin/resources >/dev/null

echo "Resources package: $ROOT/output/debs/$deb"
sha256sum "$ROOT/output/debs/$deb"
