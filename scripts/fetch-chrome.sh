#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/config/chrome.env"
name="google-chrome-stable_${CHROME_VERSION}_arm64.deb"
deb="$ROOT/cache/debs/$name"
mkdir -p "$ROOT/cache/debs"
if [[ ! -f "$deb" ]]; then
  tmp=$(mktemp "$deb.XXXXXX")
  trap 'rm -f "$tmp"' EXIT
  curl -fL --retry 3 "https://dl.google.com/linux/chrome/deb/pool/main/g/google-chrome-stable/$name" -o "$tmp"
  printf '%s  %s\n' "$CHROME_SHA256" "$tmp" | sha256sum -c -
  mv "$tmp" "$deb"
fi
printf '%s  %s\n' "$CHROME_SHA256" "$deb" | sha256sum -c -
[[ $(dpkg-deb -f "$deb" Package) == google-chrome-stable ]]
[[ $(dpkg-deb -f "$deb" Version) == "$CHROME_VERSION" ]]
[[ $(dpkg-deb -f "$deb" Architecture) == arm64 ]]
