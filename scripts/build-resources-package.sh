#!/usr/bin/env bash
# Build GNOME Resources with the Orange Pi 5B/RK3588 thermal-zone patch.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

C="$ROOT/build/resources-chroot"
BASE="$ROOT/cache/sources/ubuntu-base-26.04.1-base-arm64.tar.gz"
PATCH_DIR="$ROOT/config/patches/resources-1.10.2"
UPSTREAM_VERSION="1.10.2-0ubuntu3.1"
PACKAGE_VERSION="${RESOURCES_PACKAGE_VERSION:-${UPSTREAM_VERSION}+orangepi5b2}"
JOBS="${RESOURCES_BUILD_JOBS:-$(nproc)}"

[[ $(id -u) == 0 ]] || { echo "must run as root"; exit 1; }
[[ -s "$BASE" ]] || { echo "Missing base tarball: $BASE"; exit 1; }
grep -qx enabled /proc/sys/fs/binfmt_misc/qemu-aarch64 2>/dev/null || {
 echo "binfmt_misc has no enabled qemu-aarch64 handler; install qemu-user-static"; exit 1; }

chroot_mounts() {
 findmnt -rn -o TARGET | awk -v c="$C" '$0 == c || index($0, c "/") == 1'
}

unmount_chroot() {
 local -a t=()
 mapfile -t t < <(chroot_mounts)
 for ((i=${#t[@]}-1; i>=0; i--)); do umount -l -- "${t[i]}" || return 1; done
 [[ -z "$(chroot_mounts)" ]] || { echo "mounts remain under $C"; return 1; }
}

cleanup() { set +e; unmount_chroot; }
trap cleanup EXIT

mount_api() {
 mkdir -p "$C/dev" "$C/proc" "$C/sys"
 mount --rbind /dev "$C/dev"
 mount --make-rslave "$C/dev"
 mount -t proc proc "$C/proc"
 mount -t sysfs sysfs "$C/sys"
}

if [[ ! -e "$C/.resources-build-ready" ]]; then
 unmount_chroot
 rm -rf "$C"
 mkdir -p "$C"
 tar -xpf "$BASE" -C "$C"
 rm -f "$C/etc/resolv.conf"
 cp -L /etc/resolv.conf "$C/etc/resolv.conf"
 cat > "$C/etc/apt/sources.list.d/ubuntu.sources" <<'APT'
Types: deb deb-src
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: resolute resolute-updates resolute-security
Components: main universe restricted multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
APT
 printf '#!/bin/sh\nexit 101\n' > "$C/usr/sbin/policy-rc.d"
 chmod 755 "$C/usr/sbin/policy-rc.d"
 mount_api
 chroot "$C" dpkg --print-architecture | grep -Fqx arm64
 export DEBIAN_FRONTEND=noninteractive
 chroot "$C" apt-get update -o Acquire::Languages=none
 chroot "$C" apt-get install -y --no-install-recommends \
  build-essential fakeroot devscripts dpkg-dev quilt
 chroot "$C" apt-get build-dep -y resources
 touch "$C/.resources-build-ready"
 unmount_chroot
fi

mount_api
export DEBIAN_FRONTEND=noninteractive

rm -rf "$C/build"
mkdir -p "$C/build"
chroot "$C" sh -c "cd /build && apt-get source --download-only resources"
dpkg-source -x "$C/build/resources_${UPSTREAM_VERSION}.dsc" "$C/build/resources-1.10.2"

SRC="/build/resources-1.10.2"
[[ -d "$C$SRC" ]] || { echo "unexpected source tree layout under $C/build"; exit 1; }
[[ $(chroot "$C" sh -c "cd $SRC && dpkg-parsechangelog -S Version") == "$UPSTREAM_VERSION" ]] || {
 echo "Ubuntu shipped a different resources version than this patch queue targets"; exit 1; }

while IFS= read -r name; do
 [[ -n "$name" && "$name" != \#* ]] || continue
 install -m 0644 "$PATCH_DIR/$name" "$C$SRC/debian/patches/$name"
 printf '%s\n' "$name" >> "$C$SRC/debian/patches/series"
 echo "queued patch: $name"
done < "$PATCH_DIR/series"

chroot "$C" sh -c "cd $SRC && \
 DEBEMAIL='pencil.bsp@gmail.com' DEBFULLNAME='Orange Pi 5B Builder' \
 dch --newversion '$PACKAGE_VERSION' --distribution resolute \
 'cpu: recognize RK3588 thermal zones.'"

chroot "$C" sh -c "cd $SRC && \
 DEB_BUILD_OPTIONS='parallel=$JOBS nocheck' dpkg-buildpackage -b -uc -us"

mkdir -p "$ROOT/output/debs"
deb="resources_${PACKAGE_VERSION}_arm64.deb"
[[ -s "$C/build/$deb" ]] || { echo "Package not produced: $deb"; exit 1; }
install -m 0644 "$C/build/$deb" "$ROOT/output/debs/$deb"

dpkg-deb --fsys-tarfile "$ROOT/output/debs/$deb" |
 tar -t ./usr/bin/resources >/dev/null

echo "Resources package: $ROOT/output/debs/$deb"
sha256sum "$ROOT/output/debs/$deb"
