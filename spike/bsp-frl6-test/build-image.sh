#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC="$ROOT/worktrees/linux-rockchip-bsp-frl6"
BUILD="$ROOT/build/bsp-frl6test/build"
KINSTALL="$ROOT/build/bsp-frl6test/install"
BASE_ROOTFS="$ROOT/build/rootfs"
STAGE_ROOTFS="$ROOT/build/rootfs-bsp-frl6test.$(date +%Y%m%d-%H%M%S)"
OUT_DIR="$ROOT/output/images"
IDBLOADER="$ROOT/config/bootloader/validated/idbloader.img"
UBOOT_ITB="$ROOT/config/bootloader/validated/u-boot.itb"

KREL=$(cat "$BUILD/include/config/kernel.release")
BOARD_DTB="$BUILD/arch/arm64/boot/dts/rockchip/rk3588s-orangepi-5b.dtb"
IMAGE_FLAVOR=${IMAGE_FLAVOR:-frl6test}
IMG_BASE="ubuntu-26.04.1-bsp-${KREL}-orangepi5b-${IMAGE_FLAVOR}"
IMG="$OUT_DIR/$IMG_BASE.img"

for c in python3 rsync chroot mkfs.ext4 e2fsck parted dd sha256sum xz findmnt du awk stat depmod; do
	command -v "$c" >/dev/null || { echo "Missing: $c"; exit 1; }
done

[[ -d "$SRC/.git" ]] || { echo "Missing BSP source clone: $SRC"; exit 1; }
[[ -s "$BUILD/arch/arm64/boot/Image" ]] || { echo "Missing built kernel Image"; exit 1; }
[[ -s "$BUILD/System.map" ]] || { echo "Missing System.map"; exit 1; }
[[ -s "$BUILD/.config" ]] || { echo "Missing kernel .config"; exit 1; }
[[ -d "$KINSTALL/lib/modules/$KREL" ]] || { echo "Missing modules for $KREL"; exit 1; }
[[ -s "$BOARD_DTB" ]] || { echo "Missing board DTB: $BOARD_DTB"; exit 1; }
[[ -s "$BASE_ROOTFS/etc/os-release" ]] || { echo "Missing base rootfs"; exit 1; }
[[ -s "$IDBLOADER" ]] || { echo "Missing idbloader: $IDBLOADER"; exit 1; }
[[ -s "$UBOOT_ITB" ]] || { echo "Missing U-Boot FIT: $UBOOT_ITB"; exit 1; }

while IFS= read -r target; do
	case "$target" in
		"$BASE_ROOTFS"|"$BASE_ROOTFS"/*) echo "Unmount base rootfs first: $target"; exit 1;;
	esac
done < <(findmnt -rn -o TARGET)

mkdir -p "$OUT_DIR" "$(dirname "$STAGE_ROOTFS")"
rsync -aHAX --numeric-ids "$BASE_ROOTFS/" "$STAGE_ROOTFS/"

find "$STAGE_ROOTFS/boot" -maxdepth 1 -type f \( -name 'vmlinuz-*' -o -name 'initrd.img-*' -o -name 'config-*' -o -name 'System.map-*' \) -delete
find "$STAGE_ROOTFS/lib/modules" -mindepth 1 -maxdepth 1 -type d -exec rm -rf {} +

rsync -aHAX --numeric-ids "$KINSTALL/lib/modules/$KREL" "$STAGE_ROOTFS/lib/modules/"
depmod -b "$STAGE_ROOTFS" "$KREL"

install -D -m 0644 "$BUILD/arch/arm64/boot/Image" "$STAGE_ROOTFS/boot/vmlinuz-$KREL"
install -D -m 0644 "$BUILD/System.map" "$STAGE_ROOTFS/boot/System.map-$KREL"
install -D -m 0644 "$BUILD/.config" "$STAGE_ROOTFS/boot/config-$KREL"
install -D -m 0644 "$BOARD_DTB" "$STAGE_ROOTFS/boot/dtb/rk3588s-orangepi-5b.dtb"

printf 'RESUME=none\n' > "$STAGE_ROOTFS/etc/initramfs-tools/conf.d/resume"
: > "$STAGE_ROOTFS/etc/initramfs-tools/modules"
rm -f "$STAGE_ROOTFS/usr/share/initramfs-tools/modules.d/orangepi5b-display.conf"

cat > "$STAGE_ROOTFS/boot/extlinux/extlinux.conf" <<CFG
DEFAULT ubuntu
TIMEOUT 1
LABEL ubuntu
 MENU LABEL Ubuntu 26.04 BSP 6.1 Rockchip ${IMAGE_FLAVOR}
 LINUX /boot/vmlinuz-$KREL
 INITRD /boot/initrd.img-$KREL
 FDT /boot/dtb/rk3588s-orangepi-5b.dtb
 APPEND root=LABEL=opi5b-root rootwait rw rootfstype=ext4 console=ttyS2,1500000n8 quiet splash cma=512M consoleblank=0
CFG

printf 'LABEL=opi5b-root / ext4 defaults,noatime 0 1\n' > "$STAGE_ROOTFS/etc/fstab"
printf 'orangepi5b-%s\n' "$IMAGE_FLAVOR" > "$STAGE_ROOTFS/etc/hostname"
printf '127.0.0.1 localhost\n127.0.1.1 orangepi5b-%s\n::1 localhost\n' "$IMAGE_FLAVOR" > "$STAGE_ROOTFS/etc/hosts"
install -d -m 0755 "$STAGE_ROOTFS/etc/gdm3"
cat > "$STAGE_ROOTFS/etc/gdm3/custom.conf" <<'GDM'
# GDM configuration storage

[daemon]
InitialSetupEnable=false

[security]

[debug]
GDM

cleanup_chroot_mounts() {
	for p in sys proc dev; do
		if mountpoint -q "$STAGE_ROOTFS/$p"; then
			umount -R "$STAGE_ROOTFS/$p"
		fi
	done
}
trap cleanup_chroot_mounts EXIT
mount --rbind /dev "$STAGE_ROOTFS/dev"
mount --make-rslave "$STAGE_ROOTFS/dev"
mount -t proc proc "$STAGE_ROOTFS/proc"
mount -t sysfs sysfs "$STAGE_ROOTFS/sys"

password=$(python3 - "$ROOT/target.json" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    print(json.load(f)["password"])
PY
)

if ! chroot "$STAGE_ROOTFS" id pencil >/dev/null 2>&1; then
	chroot "$STAGE_ROOTFS" useradd -m -s /bin/bash -G sudo,adm,video,audio,render,plugdev pencil
fi
printf '%s:%s\n' pencil "$password" | chroot "$STAGE_ROOTFS" chpasswd
unset password
install -d -m 0755 "$STAGE_ROOTFS/etc/sudoers.d"
printf 'pencil ALL=(ALL) ALL\n' > "$STAGE_ROOTFS/etc/sudoers.d/90-pencil"
chmod 0440 "$STAGE_ROOTFS/etc/sudoers.d/90-pencil"

cat > "$STAGE_ROOTFS/etc/frl6test-build-info" <<INFO
Purpose: BSP Rockchip FRL6 validation image for Orange Pi 5B.
Flavor: $IMAGE_FLAVOR
Kernel: $KREL
Source: Joshua-Riek/linux-rockchip noble, commit $(git -C "$SRC" rev-parse --short=12 HEAD)
Patch 0001: removed the Rockchip HDMI QP clamp that forced FRL to 4 lanes x 10Gbps.
Applied patch files in build workspace: spike/bsp-frl6-test/0001-bsp-disable-frl5-clamp.patch
Not applied: spike/bsp-frl6-test/0002-BAD-bsp-frl6-scdc-debug-green-hang.patch
Expected check: Dell OSD should show 12Gbps 4-Lane at 3840x2160 120Hz if the sink advertises FRL6.
INFO

chroot "$STAGE_ROOTFS" update-initramfs -c -k "$KREL"
rm -f "$STAGE_ROOTFS/usr/bin/qemu-aarch64-static"
cleanup_chroot_mounts
trap - EXIT

for f in "boot/vmlinuz-$KREL" "boot/initrd.img-$KREL" "boot/dtb/rk3588s-orangepi-5b.dtb" "boot/extlinux/extlinux.conf" "etc/fstab"; do
	[[ -s "$STAGE_ROOTFS/$f" ]] || { echo "Missing staged rootfs input: $f"; exit 1; }
done

[[ $(stat -c %s "$IDBLOADER") -lt $(((16384-64)*512)) ]] || { echo "idbloader is too large"; exit 1; }
[[ $(stat -c %s "$UBOOT_ITB") -lt $(((32768-16384)*512)) ]] || { echo "u-boot.itb is too large"; exit 1; }

for f in "$IMG" "$IMG.xz" "$IMG.sha256" "$IMG.xz.sha256"; do
	if [[ -e "$f" ]]; then mv "$f" "$f.previous-$(date +%s%N)"; fi
done

rootfs_bytes=$(du -sx --block-size=1 "$STAGE_ROOTFS" | awk '{print $1}')
gib=$((1024 * 1024 * 1024))
calculated_bytes=$((rootfs_bytes * 135 / 100 + 2 * gib))
calculated_gib=$(((calculated_bytes + gib - 1) / gib))
if ((calculated_gib < 8)); then calculated_gib=8; fi
image_size_gib=${IMAGE_SIZE_GIB:-$calculated_gib}

IMAGE_STAGE=$(mktemp -d "$ROOT/build/image-stage-bsp-frl6.XXXXXX")
cleanup_image_stage() {
	rm -f "$IMAGE_STAGE/rootfs.ext4" "$IMAGE_STAGE/disk.img"
	rmdir "$IMAGE_STAGE" 2>/dev/null || true
}
trap cleanup_image_stage EXIT

DISK="$IMAGE_STAGE/disk.img"
FS="$IMAGE_STAGE/rootfs.ext4"
truncate -s "${image_size_gib}G" "$DISK"
parted -s "$DISK" mklabel gpt mkpart rootfs ext4 16MiB 100% name 1 rootfs
read -r START SECTORS < <(parted -ms "$DISK" unit s print | awk -F: '$1==1 {gsub(/s/,"",$2);gsub(/s/,"",$4);print $2,$4}')
[[ "$START" == 32768 && "$SECTORS" =~ ^[0-9]+$ ]]
truncate -s "$((SECTORS*512))" "$FS"
mkfs.ext4 -F -b 4096 -L opi5b-root -m 0 -d "$STAGE_ROOTFS" "$FS"
e2fsck -fn "$FS"
python3 - "$FS" "$DISK" "$START" <<'PY'
import errno
import os
import sys

src, dst, start = sys.argv[1:]
offset = int(start) * 512
with open(src, "rb", buffering=0) as s, open(dst, "r+b", buffering=0) as d:
    size = os.fstat(s.fileno()).st_size
    pos = 0
    while pos < size:
        try:
            begin = os.lseek(s.fileno(), pos, os.SEEK_DATA)
        except OSError as e:
            if e.errno == errno.ENXIO:
                break
            raise
        end = min(os.lseek(s.fileno(), begin, os.SEEK_HOLE), size)
        s.seek(begin)
        d.seek(offset + begin)
        left = end - begin
        while left:
            data = s.read(min(left, 4 * 1024 * 1024))
            if not data:
                raise RuntimeError("Unexpected end of filesystem")
            d.write(data)
            left -= len(data)
        pos = end
    os.fsync(d.fileno())
PY

dd if="$IDBLOADER" of="$DISK" bs=512 seek=64 conv=notrunc,fsync status=none
dd if="$UBOOT_ITB" of="$DISK" bs=512 seek=16384 conv=notrunc,fsync status=none
parted -s "$DISK" unit s print
mv "$DISK" "$IMG"
sha256sum "$IMG" > "$IMG.sha256"
xz -T"$(getconf _NPROCESSORS_ONLN)" -3 -k "$IMG"
sha256sum "$IMG.xz" > "$IMG.xz.sha256"

echo "Created: $IMG.xz"
echo "SHA256: $(cut -d' ' -f1 "$IMG.xz.sha256")"
echo "Staged rootfs kept at: $STAGE_ROOTFS"
