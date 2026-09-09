#!/usr/bin/env bash
# Play clips back to back in ONE Chrome instance, so the VA driver's object
# heap recycles slots. A fresh process gets zeroed pages from the kernel and
# hides any uninitialised field; only recycling exposes it.
set -u
CYCLES="${1:-10}"
SECS_PER="${2:-4}"
export XDG_RUNTIME_DIR=/run/user/1000
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
export WAYLAND_DISPLAY=wayland-0
export LIBVA_DRIVER_NAME=v4l2_request

HOME_DIR=/home/pencil
PAGE=$(mktemp --suffix=.html)
cat > "$PAGE" <<HTML
<body style="margin:0;background:#000">
<video id=v autoplay muted style="width:640px"></video>
<script>
const clips = ["file://$HOME_DIR/chrome-h264.mp4", "file://$HOME_DIR/chrome-hevc.mp4"];
let i = 0;
const v = document.getElementById("v");
function next() { v.src = clips[i % clips.length]; i++; v.play(); }
next();
setInterval(next, ${SECS_PER}000);
</script>
</body>
HTML
chmod a+r "$PAGE"

irq() { awk '/fdc38000.video-codec/ {s=0; for(i=2;i<=9;i++) s+=$i; print s}' /proc/interrupts; }
crashes() { ls /var/crash/ 2>/dev/null | wc -l; }

IRQ0=$(irq); CR0=$(crashes)
LOG=$(mktemp); PROFILE=$(mktemp -d)

google-chrome-stable \
  --ozone-platform=wayland --user-data-dir="$PROFILE" \
  --no-first-run --no-default-browser-check \
  --autoplay-policy=no-user-gesture-required \
  --enable-features=VaapiVideoDecodeLinuxGL,VaapiIgnoreDriverChecks \
  --ignore-gpu-blocklist --allow-file-access-from-files \
  --enable-logging=stderr --vmodule=*vaapi*=3,*video_decoder*=2 \
  "file://$PAGE" >"$LOG" 2>&1 &
PID=$!

printf 'cycle  rkvdec-IRQ  delta\n'
PREV=$IRQ0
for c in $(seq 1 "$CYCLES"); do
    sleep "$SECS_PER"
    NOW=$(irq)
    printf '%5d  %10d  %+6d\n' "$c" "$NOW" "$((NOW - PREV))"
    PREV=$NOW
done

GPU_ALIVE=$(pgrep -f -- "--type=gpu-process.*$PROFILE" >/dev/null && echo yes || echo NO)
kill -TERM $PID 2>/dev/null; sleep 2; kill -9 $PID 2>/dev/null

echo
echo "gpu process still alive at end : $GPU_ALIVE"
echo "rkvdec IRQ total               : $((PREV - IRQ0))"
echo "new /var/crash entries         : $(( $(crashes) - CR0 ))"
echo "crashpad dumps in profile      : $(find "$PROFILE" -name '*.dmp' 2>/dev/null | wc -l)"
echo "--- gpu crash / decode fallback lines ---"
grep -iE "GPU process exited|gpu process crash|Failed to (create|initialize).*(decoder|vaapi)|fallback|belongs to another decode session" "$LOG" | head -15
echo "--- vaapi decoder selection ---"
grep -c "VaapiVideoDecoder" "$LOG" | sed 's/^/VaapiVideoDecoder mentions: /'
rm -rf "$PROFILE" "$PAGE"
echo "(log: $LOG)"
