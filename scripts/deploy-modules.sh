#!/usr/bin/env bash
# Host side. Rebuilds the display modules and installs them on the board from
# target.json, then reboots into them.
#
#   bash scripts/deploy-modules.sh            build, deploy, reboot
#   bash scripts/deploy-modules.sh --restore  put the originals back, reboot
#
# Vì sao reboot chứ không rmmod/insmod: nạp lại DRM stack lúc đang chạy đã làm
# sập bo này nhiều lần — worker, modeset và teardown chạy chồng nhau trên một
# controller có thể đã mất clock. Reboot mất ~30 giây và tất định.
#
# Vì sao không cần thay kernel: mọi thứ ta đang sửa đều là module
# (DRM_DISPLAY_HELPER, DRM_ROCKCHIP, DRM_DW_HDMI_QP, PHY_ROCKCHIP_SAMSUNG_HDPTX),
# nên vmlinuz và extlinux không bị đụng tới và board luôn boot được. Nếu module
# mới hỏng thì cùng lắm là mất hình — SSH vẫn sống để chạy --restore.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
K="$ROOT/sources/linux-7.1.8"
JOBS=${JOBS:-24}
RESTORE=0
[[ "${1:-}" == "--restore" ]] && RESTORE=1

MODULES=(
    "drivers/gpu/drm/display/drm_display_helper.ko"
    "drivers/gpu/drm/bridge/synopsys/dw-hdmi-qp.ko"
    "drivers/gpu/drm/rockchip/rockchipdrm.ko"
    "drivers/phy/rockchip/phy-rockchip-samsung-hdptx.ko"
)

read -r HOST PORT USER PASS < <(python3 -c "
import json
d = json.load(open('$ROOT/target.json'))
print(d['host'], d['port'], d['username'], d['password'])
")

ssh_board() { sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -p "$PORT" "$USER@$HOST" "$@"; }

# SSH có thể còn trả lời vài giây sau lệnh reboot, nên chờ theo thời điểm boot
# đổi chứ không chỉ chờ cổng 22 mở.
wait_for_board() {
    local before="$1" now i
    for i in $(seq 1 60); do
        sleep 5
        now=$(ssh_board 'uptime -s' 2>/dev/null || true)
        [[ -n "$now" && "$now" != "$before" ]] && {
            echo "board đã lên lại (~$((i * 5))s)"
            return 0
        }
    done
    echo "board không lên lại sau 5 phút" >&2
    return 1
}

if [[ $RESTORE -eq 1 ]]; then
    echo "==> Khôi phục module gốc"
    ssh_board "sudo -S -p '' bash -s" <<EOF
$PASS
set -euo pipefail
B=/root/module-backup
[[ -d \$B ]] || { echo "không có backup ở \$B"; exit 1; }
KREL=\$(uname -r)
cd \$B && find . -name '*.ko' -exec cp --parents {} /lib/modules/\$KREL/kernel/ \;
depmod -a \$KREL
update-initramfs -u -k \$KREL
EOF
    BOOTED=$(ssh_board 'uptime -s' 2>/dev/null || true)
    ssh_board "sudo -S -p '' systemctl reboot" <<< "$PASS" || true
    wait_for_board "$BOOTED"
    exit 0
fi

echo "==> Build modules"
make -C "$K" -j"$JOBS" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
KREL=$(make -s -C "$K" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- kernelrelease)

BOARD_KREL=$(ssh_board 'uname -r')
[[ "$BOARD_KREL" == "$KREL" ]] || {
    echo "board chạy $BOARD_KREL nhưng modules build cho $KREL — dừng." >&2
    exit 1
}

echo "==> Copy $(echo "${MODULES[@]}" | wc -w) module sang board"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
for m in "${MODULES[@]}"; do
    install -D "$K/$m" "$STAGE/$(dirname "$m")/$(basename "$m")"
done
tar -C "$STAGE" -cf - . | sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -p "$PORT" "$USER@$HOST" 'cat > /tmp/modules.tar'

echo "==> Cài đặt (backup bản gốc lần đầu)"
ssh_board "sudo -S -p '' bash -s" <<EOF
$PASS
set -euo pipefail
KREL=\$(uname -r)
D=/lib/modules/\$KREL/kernel
B=/root/module-backup
if [[ ! -d \$B ]]; then
    mkdir -p \$B
    for m in $(printf '%s ' "${MODULES[@]}"); do
        install -D "\$D/\$m" "\$B/\$m"
    done
    echo "đã backup module gốc vào \$B"
fi
tar -C \$D -xf /tmp/modules.tar
depmod -a \$KREL
update-initramfs -u -k \$KREL
EOF

echo "==> Reboot"
BOOTED=$(ssh_board 'uptime -s' 2>/dev/null || true)
ssh_board "sudo -S -p '' systemctl reboot" <<< "$PASS" || true
wait_for_board "$BOOTED"

echo
echo "Xong. Kiểm tra:"
echo "  ssh $USER@$HOST 'dmesg | grep -i FRL'"
echo "Quay lại bản gốc: bash scripts/deploy-modules.sh --restore"
