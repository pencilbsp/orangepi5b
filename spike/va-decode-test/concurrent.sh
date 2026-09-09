#!/usr/bin/env bash
# Board side. Two decoders on one VADisplay, in one process, at the same time.
#
# This is the case per-context device handles exist for. v4l2-mem2mem keys a
# decode session off the open file handle, so contexts sharing one handle
# reprogram each other's queues: the second stream to start pulls the first
# one's buffers out from under it. The symptom is not a clean failure -- it is
# one or both streams decoding wrong, or "CAPTURE pool exhausted at 0 buffers"
# on a stream that was working a moment ago.
#
# Each stream is also compared against its own software decode, so "both ran"
# is not mistaken for "both were correct".
set -uo pipefail
DIR="${1:-$HOME/streams}"
A="${2:-$DIR/h264-long-repeated.h264}"
B="${3:-$DIR/hevc-main-bframes.h265}"
export LIBVA_DRIVER_NAME=v4l2_request
DEV=/dev/dri/renderD128

sw_a=$(mktemp); sw_b=$(mktemp); hw_a=$(mktemp); hw_b=$(mktemp); log=$(mktemp)

ffmpeg -y -hide_banner -loglevel error -i "$A" -pix_fmt nv12 -f framemd5 "$sw_a"
ffmpeg -y -hide_banner -loglevel error -i "$B" -pix_fmt nv12 -f framemd5 "$sw_b"

# One process, one VADisplay, two decoders: this is what a browser does with
# two videos on a page, and it is where sharing a handle falls apart.
ffmpeg -y -hide_banner -loglevel error \
	-init_hw_device vaapi=va:"$DEV" \
	-hwaccel vaapi -hwaccel_device va -hwaccel_output_format vaapi -i "$A" \
	-hwaccel vaapi -hwaccel_device va -hwaccel_output_format vaapi -i "$B" \
	-map 0:v -vf hwdownload,format=nv12 -f framemd5 "$hw_a" \
	-map 1:v -vf hwdownload,format=nv12 -f framemd5 "$hw_b" 2>"$log"
rc=$?

echo "=== concurrent decode, one process, one VADisplay ==="
if [ $rc -ne 0 ]; then
	echo "  FAIL  ffmpeg exited $rc"
	grep -v '^v4l2-request: device' "$log" | head -5
fi

for pair in "A:$sw_a:$hw_a:$(basename "$A")" "B:$sw_b:$hw_b:$(basename "$B")"; do
	IFS=: read -r tag sw hw name <<<"$pair"
	n_sw=$(grep -c '^[0-9]' "$sw"); n_hw=$(grep -c '^[0-9]' "$hw")
	if [ "$n_hw" -eq 0 ]; then
		printf '  %-28s FAIL  no frames decoded\n' "$name"
	elif [ "$n_sw" != "$n_hw" ]; then
		printf '  %-28s FAIL  frames sw=%s hw=%s\n' "$name" "$n_sw" "$n_hw"
	elif cmp -s <(grep '^[0-9]' "$sw") <(grep '^[0-9]' "$hw"); then
		printf '  %-28s PASS  %s frames bit-identical\n' "$name" "$n_hw"
	else
		d=$(diff <(grep '^[0-9]' "$sw") <(grep '^[0-9]' "$hw") | grep -c '^<')
		printf '  %-28s FAIL  %s/%s frames differ\n' "$name" "$d" "$n_hw"
	fi
done

rm -f "$sw_a" "$sw_b" "$hw_a" "$hw_b" "$log"
