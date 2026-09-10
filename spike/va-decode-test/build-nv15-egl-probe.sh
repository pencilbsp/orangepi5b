#!/usr/bin/env bash
# Cross-build the Mesa/GBM dma-buf format probe for the ARM64 board.
# Sysroot prerequisite: libgles2-mesa-dev:arm64, libegl-dev:arm64,
# libgbm-dev:arm64 and libdrm-dev:arm64.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SYSROOT="$ROOT/build/cross-chroot"
SOURCE="$ROOT/spike/va-decode-test/nv15-egl-probe.c"
OUTPUT="${1:-$ROOT/build/nv15-egl-probe}"

export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig"

mkdir -p "$(dirname "$OUTPUT")"
# shellcheck disable=SC2046
aarch64-linux-gnu-gcc --sysroot="$SYSROOT" -O2 -Wall -Wextra -Werror \
	-o "$OUTPUT" "$SOURCE" $(pkg-config --cflags --libs egl glesv2 gbm libdrm)

file "$OUTPUT"
