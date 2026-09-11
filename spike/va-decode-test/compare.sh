#!/usr/bin/env bash
# Board side. Decode each stream twice with the SAME ffmpeg binary -- once
# through VA-API, once in software -- and compare frame by frame.
#
# Using one binary for both sides means any difference is the driver's, not a
# version or build difference between two machines.
set -uo pipefail
DIR="${1:-$HOME/streams}"
export LIBVA_DRIVER_NAME=v4l2_request
DEV=/dev/dri/renderD128
FFMPEG_TIMEOUT=${FFMPEG_TIMEOUT:-120}

pass=0; fail=0; skip=0

for f in "$DIR"/*.h264 "$DIR"/*.h265 "$DIR"/*.ivf; do
	[ -e "$f" ] || continue
	name=$(basename "$f")
	case "$name" in
		vp9-profile2-*)
			printf '  %-26s SKIP  (Profile 2 intentionally not advertised)\n' "$name"
			skip=$((skip+1))
			continue
			;;
	esac
	case "$name" in
		av1-10bit*) frame_format=p010 ;;
		*) frame_format=nv12 ;;
	esac

	sw=$(mktemp); hw=$(mktemp); log=$(mktemp)

	timeout -k 5 "$FFMPEG_TIMEOUT" ffmpeg -nostdin -y -hide_banner \
		-loglevel error -i "$f" \
		-pix_fmt "$frame_format" -f framemd5 "$sw" 2>"$log"
	if [ $? -ne 0 ]; then
		printf '  %-26s SKIP  (software decode failed)\n' "$name"
		skip=$((skip+1)); rm -f "$sw" "$hw" "$log"; continue
	fi

	timeout -k 5 "$FFMPEG_TIMEOUT" ffmpeg -nostdin -y -hide_banner \
		-loglevel error \
		-hwaccel vaapi -hwaccel_device "$DEV" -hwaccel_output_format vaapi \
		-i "$f" -vf "hwdownload,format=$frame_format" \
		-f framemd5 "$hw" 2>"$log"
	rc=$?

	sw_frames=$(grep -c '^[0-9]' "$sw" 2>/dev/null)
	hw_frames=$(grep -c '^[0-9]' "$hw" 2>/dev/null)

	if [ $rc -ne 0 ] || [ "$hw_frames" -eq 0 ]; then
		printf '  %-26s FAIL  hw decode error: %s\n' "$name" \
			"$(grep -v '^v4l2-request:' "$log" | head -1 | cut -c1-60)"
		fail=$((fail+1))
	elif [ "$sw_frames" != "$hw_frames" ]; then
		printf '  %-26s FAIL  frame count sw=%s hw=%s\n' \
			"$name" "$sw_frames" "$hw_frames"
		fail=$((fail+1))
	elif cmp -s <(grep '^[0-9]' "$sw") <(grep '^[0-9]' "$hw"); then
		printf '  %-26s PASS  %s frames bit-identical\n' "$name" "$hw_frames"
		pass=$((pass+1))
	else
		diffs=$(diff <(grep '^[0-9]' "$sw") <(grep '^[0-9]' "$hw") | grep -c '^<')
		first=$(diff <(grep '^[0-9]' "$sw") <(grep '^[0-9]' "$hw") \
			| grep -m1 '^<' | awk '{print $1}')
		printf '  %-26s FAIL  %s/%s frames differ, first at frame %s\n' \
			"$name" "$diffs" "$hw_frames" "${first:-?}"
		fail=$((fail+1))
	fi

	rm -f "$sw" "$hw" "$log"
done

echo
echo "pass=$pass fail=$fail skip=$skip"
[ "$fail" -eq 0 ]
