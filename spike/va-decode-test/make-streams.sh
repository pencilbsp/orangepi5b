#!/usr/bin/env bash
# Generate the H.264 / HEVC coverage set used to prove the VA driver decodes
# bit-identically to software.
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

echo
echo "Streams in $OUT"
