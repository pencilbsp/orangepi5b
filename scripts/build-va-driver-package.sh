#!/usr/bin/env bash
# Build the VA-API driver that bridges libva to the V4L2 stateless decoders.
#
# Chrome on Linux has no V4L2 decode path -- it is compiled behind
# BUILDFLAG(IS_CHROMEOS) -- and GNOME Remote Desktop encodes through VA-API, so
# this driver is the only way either of them reaches the RK3588S codec blocks.
#
# Source lives in va-driver/ and is ours: derived from libva-v4l2-request's
# skeleton, with the codec backends written against the mainline
# V4L2_CID_STATELESS_* ABI. See docs/VA-DRIVER-DESIGN.md.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
# Cross-build on the host; never run a compiler under qemu. See CLAUDE.md.
# shellcheck source=lib/arm64-cross.sh
source "$ROOT/scripts/lib/arm64-cross.sh"

C="$ROOT/build/cross-chroot"
SRC_DIR="$ROOT/va-driver"
TEMPLATE="$ROOT/packages/orangepi5b-va-driver"
PACKAGE_VERSION="${VA_DRIVER_VERSION:-0.1.0}"
DRIVER_REL="usr/lib/aarch64-linux-gnu/dri/v4l2_request_drv_video.so"

require_cross_prereqs
[[ -d "$SRC_DIR/src" ]] || { echo "Missing source tree: $SRC_DIR"; exit 1; }
cleanup() { set +e; unmount_chroot; }
trap cleanup EXIT

prepare_cross_chroot - \
 meson ninja-build pkgconf pkg-config \
 libva-dev:arm64 libdrm-dev:arm64
mount_api
export DEBIAN_FRONTEND=noninteractive

rm -rf "$C/build/va" && mkdir -p "$C/build/va/src"
tar -C "$SRC_DIR" --exclude=.git -cf - . | tar -C "$C/build/va/src" -xf -

cat > "$C/build/va/aarch64.cross" <<'CROSS'
[binaries]
c = 'aarch64-linux-gnu-gcc'
ar = 'aarch64-linux-gnu-ar'
strip = 'aarch64-linux-gnu-strip'
pkg-config = 'pkg-config'

# Ubuntu ships no <triplet>-pkg-config wrapper, so point the native binary at
# the target architecture's .pc files instead. Without this it would answer
# with the host's libva and the driver would link against the wrong ABI.
[properties]
pkg_config_libdir = '/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
CROSS

chroot "$C" meson setup /build/va/out /build/va/src \
 --cross-file /build/va/aarch64.cross \
 --prefix=/usr --libdir=lib/aarch64-linux-gnu --buildtype=release
chroot "$C" meson compile -C /build/va/out
chroot "$C" meson install -C /build/va/out --destdir /build/va/stage

built="$C/build/va/stage/$DRIVER_REL"
[[ -s "$built" ]] || { echo "Driver was not produced: $DRIVER_REL"; exit 1; }

# A VA driver is entered through a symbol carrying the VA-API version it was
# built for. Refuse to package one the image's libva will never call: that
# failure otherwise surfaces only as "vaInitialize failed" at runtime.
libva_version=$(chroot "$C" env \
 PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig \
 pkg-config --modversion libva)
va_minor=${libva_version#*.}; va_minor=${va_minor%%.*}
symbol="__vaDriverInit_1_${va_minor}"
aarch64-linux-gnu-nm -D --defined-only "$built" | grep -Fq "$symbol" || {
 echo "Driver does not export $symbol for libva $libva_version"; exit 1; }

STAGE="$ROOT/build/packages/orangepi5b-va-driver"
rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN" "$STAGE/$(dirname "$DRIVER_REL")" "$STAGE/usr/lib/environment.d"
install -m 0644 "$built" "$STAGE/$DRIVER_REL"
install -m 0644 "$TEMPLATE/data/90-orangepi5b-vaapi.conf" \
 "$STAGE/usr/lib/environment.d/90-orangepi5b-vaapi.conf"
sed -e "s/@PACKAGE_VERSION@/${PACKAGE_VERSION}/g" \
    -e "s/@LIBVA_VERSION@/${libva_version%.*}/g" \
    "$TEMPLATE/DEBIAN/control.in" > "$STAGE/DEBIAN/control"

mkdir -p "$ROOT/output/debs"
deb="$ROOT/output/debs/orangepi5b-va-driver_${PACKAGE_VERSION}_arm64.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$deb" >/dev/null

echo "VA driver package: $deb"
echo "Built against libva $libva_version, entry point $symbol"
sha256sum "$deb"
