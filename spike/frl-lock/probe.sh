#!/bin/bash
# Chạy TRÊN BOARD, bằng root. Kích hoạt một lần FRL link training rồi thu bằng chứng.
#
#   frl-probe [rate_per_lane]      mặc định 6 Gbps/lane x 4 lane = FRL3 (24 Gbps)
#
# MỘT CHIỀU. Sau khi chạy, màn hình sẽ tắt và chỉ reboot mới lấy lại được.
# Đừng tìm cách "trả về TMDS" bằng cách ghi tiếp vào sysfs: lúc đó controller
# đã bị cắt clock và mọi truy cập register sẽ treo cứng SoC, không log gì cả.
#
# Không đụng vào rootfs ngoài thư mục kết quả.
set -u

RATE="${1:-6}"
ATTR=/sys/devices/platform/fde80000.hdmi/frl_spike

if [[ "$RATE" == "0" ]]; then
    echo "Không có đường về TMDS. Reboot đi." >&2
    exit 1
fi
OUT="${OUT:-/root/frl-probe-$(date +%Y%m%d-%H%M%S)}"

if [[ ! -w "$ATTR" ]]; then
    echo "Không thấy $ATTR — modules spike chưa được nạp?" >&2
    echo "Kiểm tra: modinfo dw-hdmi-qp | grep -i frl" >&2
    exit 1
fi

mkdir -p "$OUT"

snapshot() {
    local tag="$1"
    {
        echo "### $tag"
        cat /sys/kernel/debug/dri/0/state 2>/dev/null |
            grep -E "^connector|mode:|active=|output_bpc|output_format|tmds_char_rate|format="
    } >> "$OUT/drm-state.txt"
}

echo "=== trạng thái trước ==="
snapshot "before"
tail -20 "$OUT/drm-state.txt"

echo
echo "=== kích hoạt FRL: ${RATE} Gbps/lane x 4 lane ==="
dmesg -C
echo "$RATE" > "$ATTR"
rc=$?
sleep 3

dmesg > "$OUT/dmesg.txt"
snapshot "after"

echo
echo "=== kernel log ==="
grep -E "frl-spike|hdptx|dw-hdmi-qp|rockchip" "$OUT/dmesg.txt" | tail -40

echo
if grep -q "frl-spike: PASS" "$OUT/dmesg.txt"; then
    echo "KẾT QUẢ: PASS — link đã train xong ở FRL."
    verdict=PASS
elif grep -q "sink never raised FLT_ready" "$OUT/dmesg.txt"; then
    echo "KẾT QUẢ: FAIL-LTS2 — sink không vào chế độ FRL. Vấn đề ở phía source enable."
    verdict=FAIL-LTS2
elif grep -q "LTS3 timed out\|requested more TxFFE\|requested a rate change" "$OUT/dmesg.txt"; then
    echo "KẾT QUẢ: FAIL-LTS3 — sink có train nhưng không đạt. Vấn đề ở PHY/FFE."
    verdict=FAIL-LTS3
else
    echo "KẾT QUẢ: không rõ (write trả về $rc). Đọc $OUT/dmesg.txt."
    verdict=UNKNOWN
fi

echo "$verdict rate_per_lane=$RATE" > "$OUT/RESULT.txt"
echo
echo "Kết quả đầy đủ: $OUT"
echo
echo "Màn hình giờ đã tắt và sẽ không tự về. Copy kết quả ra rồi reboot:"
echo "  sudo systemctl reboot"
