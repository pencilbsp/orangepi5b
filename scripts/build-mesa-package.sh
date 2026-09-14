#!/usr/bin/env bash
# Build Mesa 26.0.8 with the config/patches/mesa-26.0.8 queue applied, natively
# on arm64, inside an Apple `container` VM on an Apple Silicon Mac.
#
# Why this one package does not cross-build like every other package here:
# mesa's arm64 build-deps cannot be resolved in a multiarch apt universe.
#
#   llvm-21-dev:arm64 -> llvm-21-tools:arm64 -> python3-yaml:arm64 -> python3:arm64
#
# and python3:amd64 Conflicts python3:arm64, so pulling LLVM for the target
# evicts the native interpreter that mesa's own codegen scripts run on. The
# knot is in llvm-21-tools' and python3-yaml's packaging, not mesa's, so no
# amount of :native annotation on mesa's debian/control unties it. llvmpipe is
# not optional for the image, so LLVM cannot simply be dropped. Derivation in
# config/patches/mesa-26.0.8/README.md.
#
# On an arm64 host there is exactly one architecture in the apt universe, so
# `apt-get build-dep mesa` resolves plainly, Ubuntu's debian/ is used untouched
# and nothing needs patching -- this is how Ubuntu's own buildd builds mesa.
#
# This is native compilation, not emulation. The container is a real arm64
# Linux VM on arm64 silicon; the compiler is an arm64 binary emitting arm64
# code. CLAUDE.md forbids running a compiler under qemu, and this is not that.
# The script refuses to run if the guest is not aarch64 -- amd64 under Rosetta
# WOULD be emulation, and is exactly what the rule is about.
#
# Runs on macOS. Every other script in scripts/ runs on the x86 build host;
# this one does not, and cannot.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

PATCH_DIR="${MESA_PATCH_DIR:-$ROOT/config/patches/mesa-26.0.8}"
UPSTREAM_VERSION="26.0.8-1ubuntu0.3"
PACKAGE_VERSION="${MESA_PACKAGE_VERSION:-${UPSTREAM_VERSION}+orangepi5b1}"
IMAGE="${MESA_BUILD_IMAGE:-ubuntu:26.04}"
# The host has 12 cores; leaving a couple to macOS keeps the machine usable and
# costs little, since the build is not perfectly parallel anyway.
CPUS="${MESA_BUILD_CPUS:-10}"
MEMORY="${MESA_BUILD_MEMORY:-16g}"

WORK="$ROOT/build/mesa-arm64"
OUT="$ROOT/output/debs"
# apt archives and the source tarball survive between runs; re-downloading
# 44MB of mesa plus ~600MB of build-deps on every attempt is pure waste.
CACHE="$ROOT/cache/mesa-arm64"

[[ $(uname -s) == Darwin && $(uname -m) == arm64 ]] || {
  echo "This script builds mesa natively and needs an Apple Silicon Mac." >&2
  echo "On the x86 build host there is no native arm64 route; see the header." >&2
  exit 1; }

command -v container >/dev/null || {
  echo "Apple's container CLI is not installed: brew install container" >&2
  exit 1; }

[[ -s "$PATCH_DIR/series" ]] || { echo "Missing $PATCH_DIR/series" >&2; exit 1; }

container system status >/dev/null 2>&1 || container system start

mkdir -p "$WORK" "$OUT" "$CACHE/apt" "$CACHE/src"

# The guest half lives in a file rather than inline in a `container run -c`
# string so that neither half has to escape the other's quoting.
cat > "$WORK/build-in-container.sh" <<'GUESTEOF'
#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

# Refuse emulation. An amd64 guest here would be Rosetta, which is the thing
# CLAUDE.md rules out; the whole point of this route is that it is native.
arch=$(dpkg --print-architecture)
[[ "$(uname -m)" == aarch64 && "$arch" == arm64 ]] || {
  echo "Guest is $(uname -m)/$arch, not aarch64/arm64 -- that would be emulation." >&2
  exit 1; }

# apt drops privileges to _apt and cannot read a cache directory owned by the
# host user over virtiofs; same class of trap as APT::Sandbox::User=root in the
# cross chroot, different cause.
apt_opts=(-o APT::Sandbox::User=root -o Dir::Cache::archives=/cache/apt)

sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources
apt-get "${apt_opts[@]}" update -qq
apt-get "${apt_opts[@]}" install -y --no-install-recommends devscripts dpkg-dev

# nocheck spares the test suite and its build-deps; the image never runs them.
apt-get "${apt_opts[@]}" build-dep -y -P nocheck mesa

mkdir -p /build && cd /build
# Unpack from the cached tarball when it is already there, so a rerun after a
# compile failure does not re-fetch 44MB.
(cd /cache/src && apt-get "${apt_opts[@]}" source --download-only "mesa=$UPSTREAM_VERSION")
dpkg-source -x /cache/src/mesa_"${UPSTREAM_VERSION}".dsc /build/mesa

cd /build/mesa
have=$(dpkg-parsechangelog -S Version)
[[ "$have" == "$UPSTREAM_VERSION" ]] || {
  echo "Ubuntu shipped mesa $have, not the $UPSTREAM_VERSION this queue targets" >&2
  exit 1; }

# Append to Ubuntu's series, never replace it: the five patches already there
# stay, ours run last. See config/patches/mesa-26.0.8/README.md.
while IFS= read -r name; do
  [[ -n "$name" && "$name" != \#* ]] || continue
  [[ -f "/patches/$name" ]] || { echo "Patch missing: $name" >&2; exit 1; }
  [[ -e "debian/patches/$name" ]] && { echo "Name collides with Ubuntu's: $name" >&2; exit 1; }
  install -m 0644 "/patches/$name" "debian/patches/$name"
  printf '%s\n' "$name" >> debian/patches/series
done < /patches/series

DEBEMAIL='pencil.bsp@gmail.com' DEBFULLNAME='Orange Pi 5B Builder' \
  dch --newversion "$PACKAGE_VERSION" --distribution resolute \
  'Check the EGL dmabuf export fd query and answer panvk modifier query v2.'

DEB_BUILD_OPTIONS="parallel=$JOBS nocheck" DEB_BUILD_PROFILES=nocheck \
  dpkg-buildpackage -b -uc -us

# The six binaries travel together: they are pinned to each other through
# mesa-libgallium (= ${binary:Version}), so installing a subset is not possible.
cd /build
missing=0
for p in libegl-mesa0 libgbm1 libgl1-mesa-dri libglx-mesa0 mesa-libgallium \
         mesa-vulkan-drivers; do
  f="${p}_${PACKAGE_VERSION}_arm64.deb"
  [[ -s "$f" ]] || { echo "Package not produced: $f" >&2; missing=1; continue; }
  install -m 0644 "$f" "/out/$f"
done
[[ $missing == 0 ]] || exit 1

# A mesa built without the panfrost gallium driver or without panvk installs
# cleanly and leaves the board on llvmpipe, saying nothing about it. Both
# patches are pointless without those two, so refuse such a build here rather
# than discover it on the board.
# List to a file rather than piping into grep -q: grep stops at the first hit,
# dpkg-deb dies of SIGPIPE, and pipefail turns a passing check into a failure.
for check in "mesa-libgallium:libgallium-.*\.so:no gallium library" \
             "mesa-vulkan-drivers:libvulkan_panfrost\.so:no panvk driver"; do
  pkg=${check%%:*}; rest=${check#*:}; want=${rest%%:*}; what=${rest#*:}
  dpkg-deb -c "${pkg}_${PACKAGE_VERSION}_arm64.deb" > /tmp/contents.txt
  grep -qE "$want" /tmp/contents.txt || {
    echo "$pkg has $what -- the board would silently fall back to llvmpipe" >&2
    exit 1; }
done
GUESTEOF
chmod 0755 "$WORK/build-in-container.sh"

container run --rm -a arm64 -c "$CPUS" -m "$MEMORY" \
  -e "UPSTREAM_VERSION=$UPSTREAM_VERSION" \
  -e "PACKAGE_VERSION=$PACKAGE_VERSION" \
  -e "JOBS=$CPUS" \
  -v "$PATCH_DIR:/patches" \
  -v "$CACHE/apt:/cache/apt" \
  -v "$CACHE/src:/cache/src" \
  -v "$WORK:/work" \
  -v "$OUT:/out" \
  "$IMAGE" /work/build-in-container.sh

echo
echo "Built mesa $PACKAGE_VERSION (native arm64):"
ls -l "$OUT"/*_"${PACKAGE_VERSION}"_arm64.deb
