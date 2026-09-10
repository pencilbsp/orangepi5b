#!/usr/bin/env bash
# Generate the H.264 / HEVC / VP9 coverage set used to prove the VA driver
# decodes bit-identically to software.
#
# Runs in the amd64 cross chroot, which has ffmpeg: encoding is host work and
# has nothing to do with the target architecture.
#
# The set is chosen to exercise what actually broke in prior implementations,
# not to be a broad codec conformance suite. In particular several streams
# carry REPEATED IDRs: the DPB-slot bug that this design guards against leaves
# the first GOP perfect and corrupts everything after the second IDR, so a
# short single-GOP clip proves nothing.
set -euo pipefail
OUT="${1:-/build/streams}"
GENERATE_VP9_PROFILE2="${GENERATE_VP9_PROFILE2:-0}"
mkdir -p "$OUT"

src() { # frames, size
	printf -- '-f lavfi -i testsrc2=size=%s:rate=30 -frames:v %s' "$2" "$1"
}

enc() { # name, extra x264 opts, frames, size
	local name="$1" opts="$2" frames="$3" size="$4"
	# shellcheck disable=SC2046
	ffmpeg -hide_banner -loglevel error -y $(src "$frames" "$size") \
		-pix_fmt yuv420p -c:v libx264 $opts "$OUT/$name.h264"
	printf '  %-34s %s\n' "$name.h264" "$(stat -c%s "$OUT/$name.h264") bytes"
}

enc265() {
	local name="$1" opts="$2" frames="$3" size="$4"
	# shellcheck disable=SC2046
	ffmpeg -hide_banner -loglevel error -y $(src "$frames" "$size") \
		-pix_fmt yuv420p -c:v libx265 -x265-params log-level=error:"$opts" \
		"$OUT/$name.h265"
	printf '  %-34s %s\n' "$name.h265" "$(stat -c%s "$OUT/$name.h265") bytes"
}

encvp9() {
	local name="$1" opts="$2" frames="$3" size="$4"
	# IVF keeps VP9 frame boundaries explicit, matching VA's one-frame slice.
	# shellcheck disable=SC2046
	ffmpeg -hide_banner -loglevel error -y $(src "$frames" "$size") \
		-pix_fmt yuv420p -c:v libvpx-vp9 -deadline good -cpu-used 4 \
		-g 30 $opts -f ivf "$OUT/$name.ivf"
	printf '  %-34s %s\n' "$name.ivf" "$(stat -c%s "$OUT/$name.ivf") bytes"
}

encvp9p2() {
	local name="$1" opts="$2" frames="$3" size="$4"
	# These deferred fixtures remain useful for raw V4L2 and a future native
	# NV15 VA/Chrome path. They are not part of the current VA regression run.
	# shellcheck disable=SC2046
	ffmpeg -hide_banner -loglevel error -y $(src "$frames" "$size") \
		-vf format=yuv420p10le -pix_fmt yuv420p10le \
		-c:v libvpx-vp9 -profile:v 2 -deadline good -cpu-used 4 \
		-g 30 $opts -f ivf "$OUT/$name.ivf"

	local codec profile pix_fmt width height
	codec=$(ffprobe -v error -select_streams v:0 \
		-show_entries stream=codec_name -of default=nw=1:nk=1 \
		"$OUT/$name.ivf")
	profile=$(ffprobe -v error -select_streams v:0 \
		-show_entries stream=profile -of default=nw=1:nk=1 "$OUT/$name.ivf")
	pix_fmt=$(ffprobe -v error -select_streams v:0 \
		-show_entries stream=pix_fmt -of default=nw=1:nk=1 "$OUT/$name.ivf")
	width=$(ffprobe -v error -select_streams v:0 \
		-show_entries stream=width -of default=nw=1:nk=1 "$OUT/$name.ivf")
	height=$(ffprobe -v error -select_streams v:0 \
		-show_entries stream=height -of default=nw=1:nk=1 "$OUT/$name.ivf")
	if [ "$codec" != "vp9" ] || [ "$profile" != "Profile 2" ] ||
	   [ "$pix_fmt" != "yuv420p10le" ] ||
	   [ "${width}x${height}" != "$size" ]; then
		printf 'unexpected %s: codec=%s profile=%s pix_fmt=%s size=%sx%s\n' \
			"$name.ivf" "$codec" "$profile" "$pix_fmt" \
			"$width" "$height" >&2
		return 1
	fi

	printf '  %-34s %s\n' "$name.ivf" "$(stat -c%s "$OUT/$name.ivf") bytes"
}

echo "H.264:"
# Profile and entropy coder coverage.
enc h264-baseline-cavlc "-profile:v baseline -bf 0 -coder 0 -g 30"        60  1280x720
enc h264-main-cavlc     "-profile:v main -coder 0 -g 30"                  60  1280x720
enc h264-high-cabac     "-profile:v high -bf 2 -g 30"                     60  1280x720
# Reference handling: several references and B-pyramids across many GOPs.
enc h264-refs5-bf3      "-profile:v high -refs 5 -bf 3 -g 60"            120  1280x720
# The one that matters most: IDRs every few frames, so DPB slots are released
# and re-used many times over.
enc h264-idr-every-5    "-profile:v high -bf 2 -g 5 -keyint_min 5"       120  1280x720
enc h264-long-repeated  "-profile:v high -bf 2 -g 30 -keyint_min 30"     300  1280x720
# Custom quantisation matrices exercise the 8x8 scaling-list mapping.
enc h264-cqm-jvt        "-profile:v high -x264-params cqm=jvt -g 30"     120  1920x1080
# Multiple slices per frame.
enc h264-4slices        "-profile:v high -x264-params slices=4 -g 30"     60  1280x720
# Not macroblock aligned: exercises cropping.
enc h264-854x482        "-profile:v high -g 30"                           60  854x482
# 1080p at a real size.
enc h264-1080p          "-profile:v high -bf 2 -g 30"                    120  1920x1080

echo "HEVC:"
enc265 hevc-main-basic    "keyint=30"                                     60  1280x720
enc265 hevc-main-bframes  "keyint=30:bframes=3"                          120  1280x720
enc265 hevc-idr-every-5   "keyint=5:min-keyint=5:bframes=2"              120  1280x720
enc265 hevc-1080p         "keyint=30:bframes=2"                          120  1920x1080

echo "VP9 Profile 0:"
# Repeated keyframes exercise probability-context reset and inherited state.
encvp9 vp9-profile0-repeated "-auto-alt-ref 1 -lag-in-frames 16"          180  1280x720
# More than one tile column exercises tile_info parsing and kernel layout.
encvp9 vp9-profile0-tiles    "-tile-columns 2 -row-mt 1"                  120  1920x1080

if [ "$GENERATE_VP9_PROFILE2" = 1 ]; then
	echo "VP9 Profile 2 deferred fixtures (10-bit 4:2:0):"
	encvp9p2 vp9-profile2-repeated "-auto-alt-ref 1 -lag-in-frames 16"      180  1280x720
	encvp9p2 vp9-profile2-tiles    "-tile-columns 2 -row-mt 1"              120  1920x1080
	encvp9p2 vp9-profile2-854x482  "-auto-alt-ref 0"                         60  854x482

	# Chrome needs a browser container. Remux without re-encoding and prove
	# that the VP9 packet payload did not change.
	ffmpeg -hide_banner -loglevel error -y \
		-i "$OUT/vp9-profile2-repeated.ivf" -c:v copy -an \
		"$OUT/vp9-profile2-chrome.webm"
	ivf_hash=$(ffmpeg -hide_banner -loglevel error \
		-i "$OUT/vp9-profile2-repeated.ivf" -map 0:v:0 -c copy \
		-f streamhash -hash sha256 -)
	webm_hash=$(ffmpeg -hide_banner -loglevel error \
		-i "$OUT/vp9-profile2-chrome.webm" -map 0:v:0 -c copy \
		-f streamhash -hash sha256 -)
	if [ "$ivf_hash" != "$webm_hash" ]; then
		echo "Chrome WebM changed the VP9 packet payload" >&2
		exit 1
	fi
	printf '  %-34s %s\n' "vp9-profile2-chrome.webm" \
		"$(stat -c%s "$OUT/vp9-profile2-chrome.webm") bytes"
fi

echo
echo "Streams in $OUT"
