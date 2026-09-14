#!/usr/bin/env bash
# Lay out build/mesa-patches/{stock,patched} from .deb packages instead of from
# build.sh, so measure-avatar.sh and measure-modifiers.sh test the libraries the
# image actually ships rather than the minimal panfrost-only tree.
#
# build.sh cross-builds a stripped Mesa -- panfrost + EGL, no LLVM -- because
# that was the only way to get a patched Mesa at all before
# scripts/build-mesa-package.sh existed. The patched code is the same either
# way, but "same code" is an argument, not a measurement. This script removes
# the argument: patched comes from output/debs, stock comes from the Ubuntu
# archive, and both are full builds.
#
# Writes exactly the layout the two measure scripts expect, so they run
# unchanged afterwards. It overwrites whatever build.sh left there.
#
# Runs on macOS through Apple's `container`, for the same reason
# scripts/build-mesa-package.sh does: the .deb files are arm64 and the probe
# has to be compiled for arm64 without emulating a compiler. See CLAUDE.md.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

STOCK_VERSION="${MESA_STOCK_VERSION:-26.0.8-1ubuntu0.3}"
IMAGE="${MESA_BUILD_IMAGE:-ubuntu:26.04}"
OUT="$ROOT/build/mesa-patches"
CACHE="$ROOT/cache/mesa-arm64"

command -v container >/dev/null || {
  echo "Apple's container CLI is not installed: brew install container" >&2
  exit 1; }
container system status >/dev/null 2>&1 || container system start

# The patched half is whatever scripts/build-mesa-package.sh last produced.
shopt -s nullglob
gallium_debs=( "$ROOT"/output/debs/mesa-libgallium_*+orangepi5b*_arm64.deb )
((${#gallium_debs[@]})) || {
  echo "No patched mesa in output/debs: run scripts/build-mesa-package.sh" >&2
  exit 1; }
IFS=$'\n' gallium_debs=($(printf '%s\n' "${gallium_debs[@]}" | sort -V)); unset IFS
# macOS ships bash 3.2, which has no negative array subscripts.
PATCHED_VERSION=$(basename "${gallium_debs[$((${#gallium_debs[@]} - 1))]}")
PATCHED_VERSION=${PATCHED_VERSION#mesa-libgallium_}
PATCHED_VERSION=${PATCHED_VERSION%_arm64.deb}
echo "patched: $PATCHED_VERSION"
echo "stock:   $STOCK_VERSION"

mkdir -p "$OUT" "$CACHE/apt" "$CACHE/stock-debs"

cat > "$OUT/stage-in-container.sh" <<'GUESTEOF'
#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
[[ "$(dpkg --print-architecture)" == arm64 ]] || {
  echo "Guest is not arm64 -- that would be emulation." >&2; exit 1; }

apt_opts=(-o APT::Sandbox::User=root -o Dir::Cache::archives=/cache/apt)
apt-get "${apt_opts[@]}" update -qq
# libc6-dev is a recommend of gcc, not a depend; without it there is no stdio.h.
apt-get "${apt_opts[@]}" install -y --no-install-recommends gcc libc6-dev libvulkan-dev

PKGS=(libegl-mesa0 libgbm1 mesa-libgallium mesa-vulkan-drivers)

# Stock: straight from the Ubuntu archive, cached so a rerun does not refetch.
cd /cache/stock-debs
for p in "${PKGS[@]}"; do
  [[ -s "${p}_${STOCK_VERSION}_arm64.deb" ]] ||
    apt-get "${apt_opts[@]}" download "${p}=${STOCK_VERSION}"
done

# Assemble one variant: unpack the four packages, then flatten to the shape the
# measure scripts drive with LD_LIBRARY_PATH / LIBGL_DRIVERS_PATH /
# VK_DRIVER_FILES. libgallium is both libEGL's private backend and the DRI
# module, so the pair has to travel together.
assemble() {
  local variant=$1 debdir=$2 version=$3
  local root=/tmp/root-$variant out=/out/$variant lib
  rm -rf "$root" "$out"; mkdir -p "$root" "$out/dri"
  for p in "${PKGS[@]}"; do
    dpkg-deb -x "$debdir/${p}_${version}_arm64.deb" "$root"
  done
  lib=$root/usr/lib/aarch64-linux-gnu

  cp -a "$lib/libEGL_mesa.so.0.0.0" "$out/"
  ln -sf libEGL_mesa.so.0.0.0 "$out/libEGL_mesa.so.0"
  cp -a "$lib/libgbm.so.1.0.0" "$out/"
  ln -sf libgbm.so.1.0.0 "$out/libgbm.so.1"
  cp -a "$lib/libvulkan_panfrost.so" "$out/"

  local gallium
  gallium=$(cd "$lib" && ls libgallium-*.so | head -1)
  cp -a "$lib/$gallium" "$out/"
  ln -sf "../$gallium" "$out/dri/panfrost_dri.so"

  # Our own ICD manifest so VK_DRIVER_FILES can aim a single process here
  # without touching /usr/share/vulkan on the board.
  cat > "$out/panvk_icd.json" <<ICD
{
    "ICD": { "api_version": "1.4.335", "library_path": "libvulkan_panfrost.so" },
    "file_format_version": "1.0.1"
}
ICD
  echo "== $variant ($version)"
  ls -l "$out" "$out/dri"
}

assemble stock  /cache/stock-debs "$STOCK_VERSION"
assemble patched /patched-debs    "$PATCHED_VERSION"

# The probe is ours, not Mesa's, and links only against the loader.
gcc -O2 -Wall -o /out/modifier-probe /spike/modifier-probe.c -lvulkan
echo "probe: /out/modifier-probe"
GUESTEOF
chmod 0755 "$OUT/stage-in-container.sh"

container run --rm -a arm64 \
  -e "STOCK_VERSION=$STOCK_VERSION" \
  -e "PATCHED_VERSION=$PATCHED_VERSION" \
  -v "$OUT:/out" \
  -v "$ROOT/output/debs:/patched-debs" \
  -v "$ROOT/spike/mesa-patches:/spike" \
  -v "$CACHE/apt:/cache/apt" \
  -v "$CACHE/stock-debs:/cache/stock-debs" \
  "$IMAGE" /out/stage-in-container.sh

echo
echo "Staged from .deb. measure-avatar.sh and measure-modifiers.sh now test"
echo "the shipped packages; rerun build.sh to go back to the minimal tree."
