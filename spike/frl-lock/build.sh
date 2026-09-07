#!/usr/bin/env bash
# Host side. Applies the FRL spike patch to the already-built kernel tree and
# rebuilds only the two modules it touches.
#
# Do NOT run scripts/build-boot-image.sh while spiking: it does `rm -rf` on
# sources/linux-7.1.8 and you lose both the patch and the incremental build.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SPIKE="$ROOT/spike/frl-lock"
PATCH="$SPIKE/0001-hdmi-qp-frl-link-training-spike.patch"
K="$ROOT/sources/linux-7.1.8"
JOBS=${JOBS:-8}

if [[ ! -e "$K/Makefile" ]]; then
    echo "Chưa có kernel tree ở $K." >&2
    echo "Chạy 'sudo bash scripts/build-boot-image.sh' một lần trước đã." >&2
    exit 1
fi

if [[ ! -e "$K/vmlinux" ]]; then
    echo "Kernel tree chưa build xong; spike cần build tăng dần." >&2
    exit 1
fi

# Idempotent: a successful reverse dry-run means the patch is already in.
if patch -d "$K" -p1 --batch --dry-run -R -f < "$PATCH" >/dev/null 2>&1; then
    echo "Patch spike đã được áp dụng, bỏ qua."
else
    patch -d "$K" -p1 --batch --forward < "$PATCH"
fi

make -C "$K" -j"$JOBS" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules

KREL=$(make -s -C "$K" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- kernelrelease)
mkdir -p "$SPIKE/out"
cp "$K/drivers/gpu/drm/bridge/synopsys/dw-hdmi-qp.ko" "$SPIKE/out/"
cp "$K/drivers/gpu/drm/rockchip/rockchipdrm.ko" "$SPIKE/out/"
echo "$KREL" > "$SPIKE/out/kernelrelease"

echo
echo "Modules sẵn sàng trong $SPIKE/out (kernel $KREL):"
ls -l "$SPIKE/out"/*.ko
