#!/usr/bin/env bash
# Package an already configured rootfs without loop devices or mounts.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
R="$ROOT/build/rootfs"
KREL=7.1.8-orangepi5b
JOBS=${JOBS:-8}
IMAGE_VARIANT=${OPI_IMAGE_VARIANT:-desktop-validated-fw}
IDBLOADER=${OPI_IDBLOADER:-$ROOT/config/bootloader/validated/idbloader.img}
UBOOT_ITB=${OPI_UBOOT_ITB:-$ROOT/config/bootloader/validated/u-boot.itb}
IMG_BASE="ubuntu-26.04.1-linux-7.1.8-orangepi5b"
if [[ -n "$IMAGE_VARIANT" ]]; then
 IMG_BASE="$IMG_BASE-$IMAGE_VARIANT"
fi
IMG="$ROOT/output/images/$IMG_BASE.img"
for c in python3 mkfs.ext4 e2fsck parted dd sha256sum xz findmnt du awk stat; do command -v "$c" >/dev/null; done
for f in boot/vmlinuz-$KREL boot/initrd.img-$KREL boot/dtb/rk3588s-orangepi-5b.dtb boot/extlinux/extlinux.conf etc/fstab; do
 [[ -s "$R/$f" ]] || { echo "Missing rootfs input: $f"; exit 1; }
done
[[ -s "$IDBLOADER" ]] || { echo "Missing idbloader: $IDBLOADER"; exit 1; }
[[ -s "$UBOOT_ITB" ]] || { echo "Missing U-Boot FIT: $UBOOT_ITB"; exit 1; }
[[ $(stat -c %s "$IDBLOADER") -lt $(((16384-64)*512)) ]] || { echo "idbloader is too large"; exit 1; }
[[ $(stat -c %s "$UBOOT_ITB") -lt $(((32768-16384)*512)) ]] || { echo "u-boot.itb is too large"; exit 1; }
while IFS= read -r target; do
 case "$target" in "$R"|"$R"/*) echo "Unmount rootfs first: $target"; exit 1;; esac
done < <(findmnt -rn -o TARGET)
mkdir -p "$ROOT/output/images" "$ROOT/build"
for f in "$IMG" "$IMG.xz" "$IMG.sha256" "$IMG.xz.sha256"; do
 if [[ -e "$f" ]]; then mv "$f" "$f.previous-$(date +%s%N)"; fi
done
rootfs_bytes=$(du -sx --block-size=1 "$R" | awk '{print $1}')
gib=$((1024 * 1024 * 1024))
calculated_bytes=$((rootfs_bytes * 135 / 100 + 2 * gib))
calculated_gib=$(((calculated_bytes + gib - 1) / gib))
if ((calculated_gib < 8)); then calculated_gib=8; fi
image_size_gib=${IMAGE_SIZE_GIB:-$calculated_gib}
STAGE=$(mktemp -d "$ROOT/build/image-stage.XXXXXX")
trap 'rm -f "$STAGE/rootfs.ext4" "$STAGE/disk.img"; rmdir "$STAGE" 2>/dev/null || true' EXIT
DISK="$STAGE/disk.img"
FS="$STAGE/rootfs.ext4"
truncate -s "${image_size_gib}G" "$DISK"
parted -s "$DISK" mklabel gpt mkpart rootfs ext4 16MiB 100% name 1 rootfs
read -r START SECTORS < <(parted -ms "$DISK" unit s print | awk -F: '$1==1 {gsub(/s/,"",$2);gsub(/s/,"",$4);print $2,$4}')
[[ "$START" == 32768 && "$SECTORS" =~ ^[0-9]+$ ]]
truncate -s "$((SECTORS*512))" "$FS"
mkfs.ext4 -F -b 4096 -L opi5b-root -m 0 -d "$R" "$FS"
e2fsck -fn "$FS"
python3 - "$FS" "$DISK" "$START" <<'PY'
import os,sys,errno
src,dst,start=sys.argv[1:]; offset=int(start)*512
with open(src,'rb',buffering=0) as s, open(dst,'r+b',buffering=0) as d:
 size=os.fstat(s.fileno()).st_size; pos=0
 while pos<size:
  try: begin=os.lseek(s.fileno(),pos,os.SEEK_DATA)
  except OSError as e:
   if e.errno==errno.ENXIO: break
   raise
  end=min(os.lseek(s.fileno(),begin,os.SEEK_HOLE),size)
  s.seek(begin); d.seek(offset+begin)
  left=end-begin
  while left:
   data=s.read(min(left,4*1024*1024))
   if not data: raise RuntimeError('Unexpected end of filesystem')
   d.write(data); left-=len(data)
  pos=end
 os.fsync(d.fileno())
PY
dd if="$IDBLOADER" of="$DISK" bs=512 seek=64 conv=notrunc,fsync status=none
dd if="$UBOOT_ITB" of="$DISK" bs=512 seek=16384 conv=notrunc,fsync status=none
parted -s "$DISK" unit s print
mv "$DISK" "$IMG"
sha256sum "$IMG" > "$IMG.sha256"
xz -T"$JOBS" -3 -k "$IMG"
sha256sum "$IMG.xz" > "$IMG.xz.sha256"
echo "Created $IMG; boot on physical board remains unverified."
