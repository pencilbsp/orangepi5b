#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
[[ $(id -u) == 0 ]] || { echo 'Run with sudo'; exit 1; }
# Keep all rootfs mounts private to this invocation.
if [[ ${OPI_BUILD_NAMESPACE:-0} != 1 ]]; then
 exec unshare --mount --propagation private env OPI_BUILD_NAMESPACE=1 bash "$0" "$@"
fi
JOBS=${JOBS:-8}
KREL=7.1.8-orangepi5b
R="$ROOT/build/rootfs"
K="$ROOT/sources/linux-7.1.8"
for c in aarch64-linux-gnu-gcc make flex bison bc openssl dtc qemu-aarch64-static parted mkfs.ext4 xz gpgv curl patch git depmod; do
 command -v "$c" >/dev/null || { echo "Missing: $c"; exit 1; }
done
mkdir -p output/logs output/images build
bash scripts/fetch-sources.sh
# Reset the pinned 7.1.8 tree on every build so the patch queue is deterministic.
rm -rf "$K"
mkdir -p "$K"
tar -xf "$ROOT/cache/sources/linux-7.1.8-25c76bea853d0db65b51fb4697a47cbfd9e35e76.tar" -C "$K"
while IFS= read -r patch_name; do
 [[ -n "$patch_name" && "$patch_name" != \#* ]] || continue
 p="$ROOT/config/patches/linux-7.1.8/$patch_name"
 echo "Applying kernel patch: $(basename "$p")"
 patch -d "$K" -p1 --batch --forward < "$p"
done < "$ROOT/config/patches/linux-7.1.8/series"
make -C "$K" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig

disable_kernel_symbols() {
 local list="$1"
 local symbol

 [[ -f "$list" ]] || return 0
 while IFS= read -r symbol; do
  symbol="${symbol%%#*}"
  symbol="${symbol//[[:space:]]/}"
  [[ -n "$symbol" ]] || continue
  "$K/scripts/config" --file "$K/.config" --disable "$symbol"
 done < "$list"
}

disable_enabled_group() {
 local prefix="$1"
 shift
 local symbol
 local keep
 local should_keep
 local -a symbols=()

 mapfile -t symbols < <(
  grep -E "^CONFIG_${prefix}[A-Z0-9_]*=[ym]$" "$K/.config" | cut -d= -f1
 )

 for symbol in "${symbols[@]}"; do
  should_keep=no
  for keep in "$@"; do
   if [[ "$symbol" == "CONFIG_${keep}" ]]; then
    should_keep=yes
    break
   fi
  done
  if [[ "$should_keep" == no ]]; then
   "$K/scripts/config" --file "$K/.config" --disable "${symbol#CONFIG_}"
  fi
 done
}

# Make media feature toggles visible before applying the slim profile.
"$K/scripts/config" --file "$K/.config" --enable MEDIA_SUPPORT_FILTER
disable_kernel_symbols "$ROOT/config/kernel-slim.disable"
disable_enabled_group NET_VENDOR_ NET_VENDOR_STMICRO
disable_enabled_group WLAN_VENDOR_ WLAN_VENDOR_BROADCOM
disable_enabled_group DRM_PANEL_ \
 DRM_PANEL_SIMPLE \
 DRM_PANEL_EDP \
 DRM_PANEL_BRIDGE \
 DRM_PANEL_ORIENTATION_QUIRKS
disable_enabled_group SND_SOC_ \
 SND_SOC_GENERIC_DMAENGINE_PCM \
 SND_SOC_COMPRESS \
 SND_SOC_TOPOLOGY \
 SND_SOC_USB \
 SND_SOC_I2C_AND_SPI \
 SND_SOC_ROCKCHIP_I2S \
 SND_SOC_ROCKCHIP_I2S_TDM \
 SND_SOC_ROCKCHIP_SAI \
 SND_SOC_ROCKCHIP_SPDIF \
 SND_SOC_BT_SCO \
 SND_SOC_HDMI_CODEC \
 SND_SOC_ES8328 \
 SND_SOC_ES8328_I2C \
 SND_SOC_SIMPLE_AMPLIFIER \
 SND_SOC_SIMPLE_MUX \
 SND_SOC_SPDIF
disable_enabled_group RTC_DRV_ \
 RTC_DRV_HYM8563 \
 RTC_DRV_RK808
disable_enabled_group PHY_ \
 PHY_PACKAGE \
 PHY_ROCKCHIP_EMMC \
 PHY_ROCKCHIP_INNO_HDMI \
 PHY_ROCKCHIP_INNO_USB2 \
 PHY_ROCKCHIP_INNO_DSIDPHY \
 PHY_ROCKCHIP_NANENG_COMBO_PHY \
 PHY_ROCKCHIP_PCIE \
 PHY_ROCKCHIP_SAMSUNG_HDPTX \
 PHY_ROCKCHIP_SNPS_PCIE3 \
 PHY_ROCKCHIP_TYPEC \
 PHY_ROCKCHIP_USBDP

# Re-assert Orange Pi 5B essentials after the slim profile so an accidental
# future disable entry fails safely instead of producing a silent black screen.
"$K/scripts/config" --file "$K/.config" \
 --enable IKCONFIG \
 --enable IKCONFIG_PROC \
 --enable STMMAC_ETH \
 --enable DWMAC_ROCKCHIP \
 --enable MOTORCOMM_PHY \
 --module CFG80211 \
 --module MAC80211 \
 --module RFKILL \
 --module BRCMFMAC \
 --enable BRCMFMAC_PROTO_BCDC \
 --enable BRCMFMAC_PROTO_MSGBUF \
 --enable BRCMFMAC_PCIE \
 --module BT \
 --module BT_BCM \
 --module BT_HCIUART \
 --enable BT_HCIUART_SERDEV \
 --enable BT_HCIUART_H4 \
 --enable BT_HCIUART_BCM \
 --module BT_RFCOMM \
 --module BT_BNEP \
 --module BT_HIDP \
 --module SND_USB_AUDIO \
 --module SND_SIMPLE_CARD \
 --module SND_AUDIO_GRAPH_CARD \
 --module SND_SOC_ROCKCHIP_I2S \
 --module SND_SOC_ROCKCHIP_I2S_TDM \
 --module SND_SOC_HDMI_CODEC \
 --module SND_SOC_ES8328 \
 --module SND_SOC_ES8328_I2C \
 --module SND_SOC_SIMPLE_AMPLIFIER \
 --module SND_SOC_BT_SCO \
 --module RTC_DRV_HYM8563 \
 --module RTC_DRV_RK808 \
 --enable DRM \
 --module DRM_ROCKCHIP \
 --module DRM_DW_HDMI_QP \
 --module DRM_DISPLAY_CONNECTOR \
 --module DRM_PANTHOR \
 --module VIDEO_HANTRO \
 --module VIDEO_ROCKCHIP_RGA \
 --module PHY_ROCKCHIP_SAMSUNG_HDPTX \
 --enable FW_LOADER_COMPRESS \
 --enable FW_LOADER_COMPRESS_ZSTD \
 --enable DEBUG_INFO_NONE \
 --disable DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT \
 --disable DEBUG_INFO_DWARF5 \
 --disable DEBUG_INFO_COMPRESSED_NONE \
 --enable FRAMEBUFFER_CONSOLE \
 --set-val CMA_SIZE_MBYTES 512
sed -i '/^CONFIG_LOCALVERSION=/d;/^# CONFIG_LOCALVERSION_AUTO is not set/d;/^CONFIG_LOCALVERSION_AUTO=/d' "$K/.config"
printf 'CONFIG_LOCALVERSION="-orangepi5b"\n# CONFIG_LOCALVERSION_AUTO is not set\n' >> "$K/.config"
make -C "$K" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make -s -C "$K" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- syncconfig
rm -f "$K/include/config/kernel.release"
for symbol in STMMAC_ETH MOTORCOMM_PHY FRAMEBUFFER_CONSOLE; do
 grep -qx "CONFIG_${symbol}=y" "$K/.config" || { echo "Required built-in missing: $symbol"; exit 1; }
done
for symbol in DWMAC_ROCKCHIP DRM_ROCKCHIP DRM_DW_HDMI_QP PHY_ROCKCHIP_SAMSUNG_HDPTX; do
 grep -Eq "^CONFIG_${symbol}=[ym]$" "$K/.config" || { echo "Required display driver missing: $symbol"; exit 1; }
done
for symbol in DRM_PANTHOR VIDEO_HANTRO VIDEO_ROCKCHIP_RGA; do
 grep -Eq "^CONFIG_${symbol}=[ym]$" "$K/.config" || { echo "Required GPU/media driver missing: $symbol"; exit 1; }
done
for symbol in CFG80211 MAC80211 RFKILL BRCMFMAC BT BT_BCM BT_HCIUART BT_RFCOMM BT_BNEP BT_HIDP; do
 grep -Eq "^CONFIG_${symbol}=[ym]$" "$K/.config" || { echo "Required Wi-Fi/Bluetooth driver missing: $symbol"; exit 1; }
done
for symbol in BRCMFMAC_PCIE BT_HCIUART_SERDEV BT_HCIUART_H4 BT_HCIUART_BCM; do
 grep -qx "CONFIG_${symbol}=y" "$K/.config" || { echo "Required AP6275P bus/protocol support missing: $symbol"; exit 1; }
done
for symbol in SND_USB_AUDIO SND_SIMPLE_CARD SND_AUDIO_GRAPH_CARD SND_SOC_ROCKCHIP_I2S SND_SOC_ROCKCHIP_I2S_TDM SND_SOC_HDMI_CODEC SND_SOC_ES8328 SND_SOC_ES8328_I2C SND_SOC_SIMPLE_AMPLIFIER SND_SOC_BT_SCO; do
 grep -Eq "^CONFIG_${symbol}=[ym]$" "$K/.config" || { echo "Required audio driver missing: $symbol"; exit 1; }
done
for symbol in RTC_DRV_HYM8563 RTC_DRV_RK808; do
 grep -Eq "^CONFIG_${symbol}=[ym]$" "$K/.config" || { echo "Required RTC/PMIC driver missing: $symbol"; exit 1; }
done
for symbol in FW_LOADER_COMPRESS FW_LOADER_COMPRESS_ZSTD; do
 grep -qx "CONFIG_${symbol}=y" "$K/.config" || { echo "Required compressed firmware support missing: $symbol"; exit 1; }
done
for symbol in SND_HDA DRM_NOUVEAU DRM_PANFROST DRM_POWERVR KVM VFIO CORESIGHT GNSS CHROME_PLATFORMS CROS_EC SCSI_HISI_SAS SCSI_MPT3SAS VIDEO_SYNOPSYS_HDMIRX VIDEO_CADENCE_CSI2RX VIDEO_ROCKCHIP_CIF KEYBOARD_ATKBD HW_RANDOM_VIRTIO RPMSG_VIRTIO; do
 if grep -Eq "^CONFIG_${symbol}=[ym]$" "$K/.config"; then
  echo "Unexpected driver left enabled in slim image: $symbol"
  exit 1
 fi
done
if grep -Eq '^CONFIG_DEBUG_INFO=[ym]$' "$K/.config"; then
 echo "Debug info should be disabled in slim image"
 exit 1
fi
grep -Fqx 'CONFIG_NET_VENDOR_STMICRO=y' "$K/.config" || { echo "Required Ethernet vendor missing: STMICRO"; exit 1; }
while IFS= read -r vendor_line; do
 if [[ "$vendor_line" != CONFIG_NET_VENDOR_STMICRO=y ]]; then
  echo "Unexpected Ethernet vendor left enabled: $vendor_line"
  exit 1
 fi
done < <(grep -E '^CONFIG_NET_VENDOR_[A-Z0-9_]+=[ym]$' "$K/.config" || true)
grep -Fqx 'CONFIG_WLAN_VENDOR_BROADCOM=y' "$K/.config" || { echo "Required WLAN vendor missing: BROADCOM"; exit 1; }
while IFS= read -r vendor_line; do
 if [[ "$vendor_line" != CONFIG_WLAN_VENDOR_BROADCOM=y ]]; then
  echo "Unexpected WLAN vendor left enabled: $vendor_line"
  exit 1
 fi
done < <(grep -E '^CONFIG_WLAN_VENDOR_[A-Z0-9_]+=[ym]$' "$K/.config" || true)
while IFS= read -r panel_line; do
 case "$panel_line" in
  CONFIG_DRM_PANEL_SIMPLE=m|CONFIG_DRM_PANEL_EDP=m|CONFIG_DRM_PANEL_BRIDGE=y|CONFIG_DRM_PANEL_ORIENTATION_QUIRKS=y) ;;
  *) echo "Unexpected DRM panel left enabled: $panel_line"; exit 1 ;;
 esac
done < <(grep -E '^CONFIG_DRM_PANEL_[A-Z0-9_]+=[ym]$' "$K/.config" || true)
make -C "$K" -j"$JOBS" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image modules rockchip/rk3588s-orangepi-5b.dtb
[[ $(make -s -C "$K" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- kernelrelease) == "$KREL" ]]
if [[ ! -f "$R/etc/os-release" ]]; then
 mkdir -p "$R"
 tar --numeric-owner -xzf cache/sources/ubuntu-base-26.04.1-base-arm64.tar.gz -C "$R"
fi
# Register ARM64 emulation if the host has not already done so.
if [[ ! -f /proc/sys/fs/binfmt_misc/qemu-aarch64 ]]; then
 mountpoint -q /proc/sys/fs/binfmt_misc || mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
 cat /usr/lib/binfmt.d/qemu-aarch64.conf > /proc/sys/fs/binfmt_misc/register
fi
chroot "$R" /bin/true
unshare --mount --propagation private bash scripts/install-rootfs.sh
cleanup_rootfs() {
 for p in sys proc dev; do
  if mountpoint -q "$R/$p"; then umount -R "$R/$p"; fi
 done
}
trap cleanup_rootfs EXIT
mount --rbind /dev "$R/dev"
mount --make-rslave "$R/dev"
mount -t proc proc "$R/proc"
mount -t sysfs sysfs "$R/sys"
make -C "$K" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- INSTALL_MOD_PATH="$R" INSTALL_MOD_STRIP=1 DEPMOD=true modules_install
rm -f "$R/lib/modules/$KREL/build" "$R/lib/modules/$KREL/source"
depmod -b "$R" "$KREL"
mkdir -p "$R/boot/extlinux" "$R/boot/dtb" "$R/etc/netplan"
cp "$K/arch/arm64/boot/Image" "$R/boot/vmlinuz-$KREL"
cp "$K/arch/arm64/boot/dts/rockchip/rk3588s-orangepi-5b.dtb" "$R/boot/dtb/rk3588s-orangepi-5b.dtb"
cp "$K/.config" "$R/boot/config-$KREL"
if [[ -s "$K/System.map" ]]; then cp "$K/System.map" "$R/boot/System.map-$KREL"; fi
printf 'RESUME=none\n' > "$R/etc/initramfs-tools/conf.d/resume"
printf 'phy_rockchip_samsung_hdptx\ndw_hdmi_qp\nrockchipdrm\n' > "$R/etc/initramfs-tools/modules"
if [[ -f "$R/boot/initrd.img-$KREL" ]]; then
 chroot "$R" update-initramfs -u -k "$KREL"
else
 chroot "$R" update-initramfs -c -k "$KREL"
fi
cat > "$R/boot/extlinux/extlinux.conf" <<CFG
DEFAULT ubuntu
TIMEOUT 1
LABEL ubuntu
 MENU LABEL Ubuntu 26.04 desktop stable RK3588S
 LINUX /boot/vmlinuz-$KREL
 INITRD /boot/initrd.img-$KREL
 FDT /boot/dtb/rk3588s-orangepi-5b.dtb
 APPEND root=LABEL=opi5b-root rootwait rw rootfstype=ext4 console=ttyS2,1500000n8 quiet splash cma=512M consoleblank=0
CFG
printf 'LABEL=opi5b-root / ext4 defaults,noatime 0 1\n' > "$R/etc/fstab"
printf 'orangepi5b\n' > "$R/etc/hostname"
printf '127.0.0.1 localhost\n127.0.1.1 orangepi5b\n::1 localhost\n' > "$R/etc/hosts"
cleanup_rootfs
trap - EXIT
bash "$ROOT/scripts/assemble-image.sh"
