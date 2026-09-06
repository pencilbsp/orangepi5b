#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p cache/sources sources
fetch() { [[ -s "cache/sources/$2" ]] || curl -fL --retry 3 "$1" -o "cache/sources/$2"; }
fetch https://cdimage.ubuntu.com/ubuntu-base/releases/26.04/release/ubuntu-base-26.04.1-base-arm64.tar.gz ubuntu-base-26.04.1-base-arm64.tar.gz
(cd cache/sources; sha256sum -c ../../config/source-checksums.sha256)
LINUX_REV=25c76bea853d0db65b51fb4697a47cbfd9e35e76
LINUX_SEED=${LINUX_SEED:-/root/orangepi5b/upstream/linux}
LINUX_ARCHIVE=cache/sources/linux-7.1.8-${LINUX_REV}.tar
if [[ ! -s "$LINUX_ARCHIVE" ]]; then
 if [[ -e "$LINUX_SEED/.git" ]]; then
  git \
   -c safe.directory="$LINUX_SEED" \
   -c safe.directory=/root/orangepi5b/.git/modules/upstream/linux \
   -C "$LINUX_SEED" archive --format=tar -o "$ROOT/$LINUX_ARCHIVE" "$LINUX_REV"
 else
  tmp_git="$ROOT/cache/sources/linux-7.1.8.git"
  if [[ ! -d "$tmp_git/.git" ]]; then
   git clone https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git "$tmp_git"
  fi
  git -C "$tmp_git" fetch --tags origin v7.1.8
  git -C "$tmp_git" archive --format=tar -o "$ROOT/$LINUX_ARCHIVE" "$LINUX_REV"
 fi
fi
sha256sum -c config/bootloader/validated/SHA256SUMS
