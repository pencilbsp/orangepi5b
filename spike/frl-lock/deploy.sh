#!/usr/bin/env bash
# Host side. Copies the spike modules onto the board described by target.json,
# backs up the originals once, and reboots.
#
# Rollback lives on the board at /root/frl-spike-backup: see restore() below
# and the README.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SPIKE="$ROOT/spike/frl-lock"
OUT="$SPIKE/out"

[[ -e "$OUT/dw-hdmi-qp.ko" ]] || { echo "Chạy build.sh trước." >&2; exit 1; }
KREL=$(cat "$OUT/kernelrelease")

read -r HOST PORT USER PASS < <(python3 -c "
import json
d = json.load(open('$ROOT/target.json'))
print(d['host'], d['port'], d['username'], d['password'])
")

ssh_board() { sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -p "$PORT" "$USER@$HOST" "$@"; }
scp_board() { sshpass -p "$PASS" scp -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -P "$PORT" "$@" "$USER@$HOST:/tmp/"; }

BRIDGE_DIR="/lib/modules/$KREL/kernel/drivers/gpu/drm/bridge/synopsys"
ROCKCHIP_DIR="/lib/modules/$KREL/kernel/drivers/gpu/drm/rockchip"

echo "==> Kiểm tra kernel trên board khớp $KREL"
BOARD_KREL=$(ssh_board 'uname -r')
[[ "$BOARD_KREL" == "$KREL" ]] || {
    echo "Board đang chạy $BOARD_KREL, modules build cho $KREL. Dừng." >&2
    exit 1
}

echo "==> Copy modules và probe script"
scp_board "$OUT/dw-hdmi-qp.ko" "$OUT/rockchipdrm.ko" "$SPIKE/probe.sh"

echo "==> Cài đặt (backup bản gốc lần đầu)"
# sudo -S đọc dòng đầu của stdin làm password, bash -s đọc phần còn lại.
ssh_board "sudo -S -p '' bash -s" <<EOF
$PASS
set -euo pipefail
BACKUP=/root/frl-spike-backup
if [[ ! -d \$BACKUP ]]; then
    mkdir -p \$BACKUP
    cp "$BRIDGE_DIR/dw-hdmi-qp.ko" \$BACKUP/
    cp "$ROCKCHIP_DIR/rockchipdrm.ko" \$BACKUP/
    echo "Đã backup modules gốc vào \$BACKUP"
fi
install -m 0644 /tmp/dw-hdmi-qp.ko "$BRIDGE_DIR/dw-hdmi-qp.ko"
install -m 0644 /tmp/rockchipdrm.ko "$ROCKCHIP_DIR/rockchipdrm.ko"
install -m 0755 /tmp/probe.sh /usr/local/bin/frl-probe
depmod -a "$KREL"
EOF

echo "==> Reboot board"
ssh_board "echo '$PASS' | sudo -S -p '' systemctl reboot" || true

echo
echo "Board đang reboot. Sau khi lên, chạy:"
echo "  ssh $USER@$HOST 'sudo frl-probe 6'"
