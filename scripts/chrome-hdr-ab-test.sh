#!/usr/bin/env bash
# Run on the Orange Pi desktop session to A/B Chromium's Wayland HDR color path.
# The page opens an alert after launch; use it while toggling HDR -> SDR.
# Usage:
#   scripts/chrome-hdr-ab-test.sh normal
#   scripts/chrome-hdr-ab-test.sh no-wpcolor
set -euo pipefail
CHROME_BIN=${CHROME_BIN:-/usr/bin/google-chrome-stable}
mode=${1:-no-wpcolor}
case "$mode" in
  normal)
    profile=/tmp/chrome-hdr-ab-normal
    title="NORMAL Chrome HDR to SDR repaint test"
    flags=()
    ;;
  no-wpcolor)
    profile=/tmp/chrome-hdr-ab-no-wpcolor
    title="NO-WPCOLOR Chrome HDR to SDR repaint test"
    flags=(--disable-features=WaylandWpColorManagerV1)
    ;;
  *)
    echo "usage: $0 {normal|no-wpcolor}" >&2
    exit 2
    ;;
esac
html=$(python3 - "$title" "$mode" <<'PY'
import html
import sys

title = html.escape(sys.argv[1])
mode = html.escape(sys.argv[2])
print(f'''<!doctype html><meta charset="utf-8"><title>{title}</title>
<body style="margin:0;font:32px sans-serif;background:#f2f2f2;color:#111">
<div style="padding:48px">
<h1>{title}</h1>
<p>Mode: <b>{mode}</b></p>
<p>Open this while output is HDR, then switch the display back to SDR.</p>
<p>If the dialog stays over-saturated until hover, Chrome kept a stale color-managed surface.</p>
<div style="width:780px;height:180px;background:linear-gradient(90deg,#000,#777,#fff);border:4px solid #444"></div>
<script>
setTimeout(() => alert('{mode}: alert opened while output was HDR. After switching back to SDR, watch whether this dialog stays over-saturated until hover.'), 1200);
</script>
</div></body>''')
PY
)
url=$(printf '%s' "$html" | python3 -c 'import base64, sys; print("data:text/html;base64," + base64.b64encode(sys.stdin.buffer.read()).decode())')
mkdir -p "$profile"
exec "$CHROME_BIN" \
  --user-data-dir="$profile" \
  --no-first-run \
  --no-default-browser-check \
  --ozone-platform=wayland \
  "${flags[@]}" \
  --new-window "$url"
