#!/usr/bin/env bash
# Cross-build a panfrost-only Mesa twice -- once stock, once with the whole
# config/patches/mesa-26.0.8 queue applied -- so each patch can be measured
# against its own control on the board.
#
# Why a spike build and not the Ubuntu source package: mesa's debian/control
# has no :native annotations, and its arm64 build-deps collide head-on with
# the host tools a cross build needs. Two hard conflicts, neither avoidable
# from outside the packaging:
#
#   bindgen:amd64 -> libclang-21-dev:amd64, which Conflicts libclang-21-dev:arm64
#   llvm-21-dev:arm64 -> llvm-21-tools:arm64 -> python3-yaml:arm64,
#     which Conflicts the native python3-yaml mesa's own scripts need
#
# Neither has anything to do with the patch. The patched code is in
# libEGL_mesa.so (EGL core, src/egl/drivers/dri2/egl_dri2.c) and does not
# depend on llvm, rusticl or any driver, so a minimal panfrost-only build
# exercises exactly the same code. Both variants come out of one tree with one
# set of options, so the only difference between them is the patch.
#
# Deploy nothing: run the test binary against these libraries with
# LD_LIBRARY_PATH and LIBGL_DRIVERS_PATH. The board's own Mesa stays untouched.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
# Cross-build on the host; never run a compiler under qemu. See CLAUDE.md.
# shellcheck source=../../scripts/lib/arm64-cross.sh
source "$ROOT/scripts/lib/arm64-cross.sh"

C="$ROOT/build/cross-chroot"
PATCH_DIR="$ROOT/config/patches/mesa-26.0.8"
TARBALL="cache/sources/mesa_26.0.8.orig.tar.xz"
OUT="$ROOT/build/mesa-patches"
JOBS="${MESA_BUILD_JOBS:-$(nproc)}"

require_cross_prereqs
[[ -s "$ROOT/$TARBALL" ]] || { echo "Missing $TARBALL"; exit 1; }
[[ -s "$PATCH_DIR/series" ]] || { echo "Missing $PATCH_DIR/series"; exit 1; }
cleanup() { set +e; unmount_chroot; }
trap cleanup EXIT

export DEBIAN_FRONTEND=noninteractive
# LLVM here is amd64 only, for the native stage below. Installing it for arm64
# is what deadlocks the Debian packaging route, and the cross stage does not
# need it.
prepare_cross_chroot - \
 meson ninja-build pkgconf bison flex python3-mako python3-yaml \
 libwayland-bin wayland-protocols \
 llvm-21-dev libclang-21-dev libclang-cpp21-dev libllvmspirvlib-21-dev \
 libclc-21-dev llvm-spirv-21 spirv-tools-dev libdrm-dev libudev-dev \
 libdrm-dev:arm64 libexpat1-dev:arm64 zlib1g-dev:arm64 libzstd-dev:arm64 \
 libwayland-dev:arm64 libwayland-egl-backend-dev:arm64

rm -rf "$C/build/mesa" && mkdir -p "$C/build/mesa"
tar -xf "$ROOT/$TARBALL" -C "$C/build/mesa"
mkdir -p "$C/build/mesa/patches"
while IFS= read -r name; do
 [[ -n "$name" && "$name" != \#* ]] || continue
 [[ -f "$PATCH_DIR/$name" ]] || { echo "Patch missing: $PATCH_DIR/$name"; exit 1; }
 install -m 0644 "$PATCH_DIR/$name" "$C/build/mesa/patches/$name"
done < "$PATCH_DIR/series"
install -m 0644 "$(dirname "$0")/modifier-probe.c" "$C/build/mesa/modifier-probe.c"

cat > "$C/build/mesa/aarch64.cross" <<'CROSS'
[binaries]
c = 'aarch64-linux-gnu-gcc'
cpp = 'aarch64-linux-gnu-g++'
ar = 'aarch64-linux-gnu-ar'
strip = 'aarch64-linux-gnu-strip'
pkg-config = 'pkg-config'

# Ubuntu ships no <triplet>-pkg-config wrapper, so point the native binary at
# the target architecture's .pc files. Same trick as build-va-driver-package.sh.
[properties]
pkg_config_libdir = '/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
CROSS

# Panfrost compiles part of itself with OpenCL C, so a cross build needs three
# native helpers -- mesa_clc, vtn_bindgen2 and panfrost_compile. That is what
# -Dinstall-mesa-clc / -Dinstall-precomp-compiler exist for. Build them for the
# host machine first; only this stage needs LLVM, and only for amd64.
NATIVE_OPTS=(
 --prefix=/usr/local
 -Dgallium-drivers=panfrost -Dvulkan-drivers=
 -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles2=disabled
 -Dllvm=enabled -Dmesa-clc=enabled -Dprecomp-compiler=enabled
 -Dinstall-mesa-clc=true -Dinstall-precomp-compiler=true
 -Dbuild-tests=false -Dtools=
)
chroot "$C" meson setup /build/mesa/native /build/mesa/mesa-26.0.8 "${NATIVE_OPTS[@]}"
chroot "$C" ninja -C /build/mesa/native -j "$JOBS" \
 src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 \
 src/panfrost/clc/panfrost_compile
mkdir -p "$C/build/mesa/nativebin"
for t in src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 src/panfrost/clc/panfrost_compile; do
 install -m 0755 "$C/build/mesa/native/$t" "$C/build/mesa/nativebin/"
done

# Only what the avatar path touches: panfrost GL through EGL on Wayland.
# No llvm for arm64, so no llvmpipe/zink/rusticl and none of the knots above.
MESON_OPTS=(
 --cross-file /build/mesa/aarch64.cross
 --prefix=/usr --libdir=lib/aarch64-linux-gnu --buildtype=release
 -Dgallium-drivers=panfrost -Dvulkan-drivers=panfrost
 -Dplatforms=wayland -Dglx=disabled -Degl=enabled -Dgbm=enabled -Dgles2=enabled
 -Dllvm=disabled -Dgallium-rusticl=false -Dvideo-codecs=
 -Dmesa-clc=system -Dprecomp-compiler=system
 -Dgallium-va=disabled
 -Dbuild-tests=false -Dtools=
)

rm -rf "$OUT" && mkdir -p "$OUT"
for variant in stock patched; do
 tree="/build/mesa/mesa-26.0.8"
 work="/build/mesa/$variant"
 rm -rf "$C$work"
 cp -a "$C$tree" "$C$work"
 if [[ "$variant" == patched ]]; then
  while IFS= read -r name; do
   [[ -n "$name" && "$name" != \#* ]] || continue
   chroot "$C" sh -c "cd $work && patch -p1 --fuzz=0 < /build/mesa/patches/$name"
  done < "$PATCH_DIR/series"
 fi
 chroot "$C" env PATH=/build/mesa/nativebin:/usr/sbin:/usr/bin:/sbin:/bin \
  meson setup "$work/out" "$work" "${MESON_OPTS[@]}"
 chroot "$C" env PATH=/build/mesa/nativebin:/usr/sbin:/usr/bin:/sbin:/bin \
  ninja -C "$work/out" -j "$JOBS"

 mkdir -p "$OUT/$variant/dri"
 # libEGL_mesa carries the fix; libgallium is its private counterpart and the
 # DRI module both at once, so the pair has to travel together.
 find "$C$work/out" \( -name 'libEGL_mesa.so*' -o -name 'libgallium*.so' \
   -o -name 'libgbm.so.1*' -o -name 'libvulkan_panfrost.so' \) -type f |
  while read -r f; do cp -a "$f" "$OUT/$variant/"; done
 # An ICD manifest of our own, so VK_DRIVER_FILES can point a single process at
 # this build without touching /usr/share/vulkan on the board.
 cat > "$OUT/$variant/panvk_icd.json" <<ICD
{
    "ICD": { "api_version": "1.4.335", "library_path": "libvulkan_panfrost.so" },
    "file_format_version": "1.0.1"
}
ICD
 [[ -e "$OUT/$variant/libEGL_mesa.so.0.0.0" ]] || {
  ln -sf "$(cd "$OUT/$variant" && ls libEGL_mesa.so* | head -1)" "$OUT/$variant/libEGL_mesa.so.0.0.0"; }
 ln -sf libEGL_mesa.so.0.0.0 "$OUT/$variant/libEGL_mesa.so.0"
 ln -sf libgbm.so.1.0.0 "$OUT/$variant/libgbm.so.1" 2>/dev/null || true
 gallium=$(cd "$OUT/$variant" && ls libgallium*.so | head -1)
 ln -sf "../$gallium" "$OUT/$variant/dri/panfrost_dri.so"
done

echo
for variant in stock patched; do
 echo "== $variant"
 ls -l "$OUT/$variant" "$OUT/$variant/dri"
done

# The Vulkan probe is ours, not Mesa's, and links only against the loader.
chroot "$C" aarch64-linux-gnu-gcc -O2 -Wall -o /build/mesa/modifier-probe \
 /build/mesa/modifier-probe.c -lvulkan
install -m 0755 "$C/build/mesa/modifier-probe" "$OUT/modifier-probe"
echo "probe: $OUT/modifier-probe"
