#!/bin/bash
# Chạy TRÊN BOARD (orangepi5b), qua SSH, khi màn hình HDMI đang đen.
# Mục đích: tách bạch hai nghi vấn cho màn hình đen ở 1920x1080@60:
#   (1) output_bpc = 10 (deep colour)
#   (2) mode đang dùng là DMT 0x52 (-hsync/-vsync) chứ không phải CEA VIC 16 (+/+)
# Không ghi gì vào rootfs ngoài thư mục kết quả.
set -u

OUT="${1:-$HOME/hdmi-probe-$(date +%H%M%S)}"
mkdir -p "$OUT"

need() { command -v "$1" >/dev/null 2>&1; }
if ! need modetest; then
    echo "modetest chưa có; cài: sudo apt-get install -y libdrm-tests" >&2
    exit 1
fi

CARD=/dev/dri/card0

echo "### danh sách mode + cờ đồng bộ (tìm dòng 1920x1080 60 và cột flags)"
modetest -M rockchip -c > "$OUT/modetest-connectors.txt" 2>&1
grep -nE "^Connectors|^[0-9]+\s|1920x1080|flags:" "$OUT/modetest-connectors.txt" | head -80

CONN=$(awk '/^Connectors:/{f=1;next} f&&/^[0-9]+/{print $1; exit}' "$OUT/modetest-connectors.txt")
CRTC=$(awk '/^CRTCs:/{f=1;next} f&&/^[0-9]+/{print $1; exit}' "$OUT/modetest-connectors.txt")
echo "connector=$CONN crtc=$CRTC"

# Chỉ số của từng mode 1920x1080@60 trong danh sách của connector.
mapfile -t IDX < <(awk -v c="$CONN" '
  /^Connectors:/{inc=1}
  inc && $1==c {found=1; next}
  found && /modes:/{m=1; i=-1; next}
  m && /^\s+#/ {next}
  m && /^\s+[0-9]+\s/ {i++; if ($1=="1920x1080" || $2=="1920x1080") print i}
  m && /props:/{exit}' "$OUT/modetest-connectors.txt")
echo "chỉ số mode 1920x1080: ${IDX[*]:-<không phân tích được, xem file>}"

run_case() {
    local label="$1" bpc="$2" modespec="$3"
    echo
    echo "=== $label : max bpc=$bpc, mode=$modespec ==="
    echo "Nhìn màn hình trong 8 giây rồi trả lời."
    modetest -M rockchip -w "$CONN:max bpc:$bpc" -s "$CONN@$CRTC:$modespec" -v >"$OUT/$label.txt" 2>&1 &
    local pid=$!
    sleep 8
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    {
        echo "--- $label (bpc=$bpc mode=$modespec) ---"
        cat /sys/kernel/debug/dri/0/state 2>/dev/null | grep -E "mode:|output_bpc|tmds_char_rate|output_format"
    } >> "$OUT/state-per-case.txt"
    read -r -p "Có thấy hình không? [y/N] " a
    echo "$label bpc=$bpc mode=$modespec visible=${a:-n}" >> "$OUT/RESULT.txt"
}

# 2x2: {mode đầu tiên, mode thứ hai} x {8 bpc, 10 bpc}
for i in "${IDX[@]:0:2}"; do
    for b in 8 10; do
        run_case "mode${i}-bpc${b}" "$b" "#${i}1920x1080"
    done
done

echo
echo "Kết quả: $OUT/RESULT.txt"
cat "$OUT/RESULT.txt"
