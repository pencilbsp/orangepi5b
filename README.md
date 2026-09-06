# Orange Pi 5B — Ubuntu 26.04 GNOME boot/display baseline

Mục tiêu của dự án này là một image mới, gọn và dễ kiểm chứng cho Orange Pi 5B:

- Ubuntu Base 26.04.1 arm64.
- Linux 7.1.8, pinned tại commit `25c76bea853d0db65b51fb4697a47cbfd9e35e76`.
- Bootloader Rockchip đã validated từ dự án cũ: DDR v1.20, BL31 v1.45, U-Boot/Radxa artifact đã boot nhanh trên phần cứng thật.
- GNOME desktop tối thiểu với GDM và first-boot setup wizard.
- HDMI tự chọn mode theo EDID và có patch tối thiểu để hỗ trợ đường 4K120 qua HDMI 2.0/SCDC/YCbCr 4:2:0.
- SSH bật sẵn và SSH host keys được sinh lại trên thiết bị khi boot; tài khoản người dùng được tạo bằng GNOME Initial Setup ở lần boot đầu.
- Panthor/Mali firmware được ship qua `linux-firmware-misc`; kernel bật firmware loader `.zst` để GNOME Wayland có render node ngay từ boot.
- AP6275P Wi-Fi/Bluetooth được bật bằng DT board patch, firmware Broadcom riêng và BlueZ/NetworkManager userspace.


## Build

Chuẩn bị host Ubuntu x86_64 có toolchain ARM64, QEMU user, chroot/mount và các công cụ filesystem:

```bash
sudo apt-get install build-essential gcc-aarch64-linux-gnu bc bison flex \
  libssl-dev libelf-dev device-tree-compiler python3-pyelftools \
  qemu-user-static binfmt-support git curl xz-utils rsync parted \
  e2fsprogs dosfstools kmod udev util-linux
```

Build image:

```bash
sudo env JOBS=8 bash scripts/build-boot-image.sh 2>&1 | tee output/build.log
```

Image mặc định sinh ra:

```text
output/images/ubuntu-26.04.1-linux-7.1.8-orangepi5b-desktop-validated-fw.img.xz
```

Flash vào eMMC hoặc microSD bằng đúng block device của bạn:

```bash
xzcat output/images/ubuntu-26.04.1-linux-7.1.8-orangepi5b-desktop-validated-fw.img.xz | sudo dd of=/dev/XXX bs=16M status=progress conv=fsync
```

## Boot layout

Image dùng một root partition ext4 bắt đầu tại 16 MiB, label `opi5b-root`.
Bootloader validated được ghi theo layout Rockchip đã kiểm chứng:

- `idbloader.img` tại LBA 64.
- `u-boot.itb` tại LBA 16384.

Kernel/extlinux:

```text
/boot/vmlinuz-7.1.8-orangepi5b
/boot/initrd.img-7.1.8-orangepi5b
/boot/dtb/rk3588s-orangepi-5b.dtb
/boot/extlinux/extlinux.conf
```

Kernel command line giữ serial recovery trên `ttyS2,1500000n8`, bật splash cho desktop, và đặt `cma=512M` để đủ headroom cho display cao.

## First boot setup

Image không embed tài khoản người dùng mặc định. GDM bật
`InitialSetupEnable=true`, nên lần boot đầu sẽ mở GNOME Initial Setup trên HDMI
để chọn locale/keyboard/timezone/network và tạo user/password.

SSH service vẫn được bật và host keys vẫn được sinh lại khi boot, nhưng SSH chỉ
login được sau khi wizard đã tạo tài khoản người dùng.

## Kernel slim profile

Build dùng `config/kernel-slim.disable` để cắt bớt driver không cần cho baseline Orange Pi 5B desktop:

- Giữ Rockchip boot/storage, onboard LAN STMMAC/Motorcomm, USB, HDMI/DRM Rockchip, Panthor GPU, Rockchip media codecs/RGA và Broadcom Wi-Fi family.
- Tắt các vendor Ethernet/WLAN khác, tuner/DVB/radio/SDR, GPU/DRM/bridge/panel không liên quan, XEN/CXL/Infiniband, filesystem cổ/cluster và test/debug suites.
- Build script có guard để fail sớm nếu mất driver bắt buộc hoặc nếu vendor/panel thừa bị bật lại ngoài ý muốn.

## Patch policy

Patch queue hiện có hai patch local:

```text
config/patches/linux-7.1.8/0001-rk3588s-orangepi5b-hdmi-4k120.patch
config/patches/linux-7.1.8/0002-arm64-dts-rockchip-enable-orangepi5b-ap6275p.patch
```

Patch HDMI chỉ chạm display path:

- DW HDMI QP bridge SCDC/scrambling.
- Rockchip HDMI QP YCbCr 4:2:0 fallback cho mode băng thông cao.
- Validate fallback trước khi nhận mode.
- Tăng VOP ACLK request cho Orange Pi 5/5B.

Patch AP6275P chỉ chạm DTS của Orange Pi 5B:

- Bật PCIe 2.0 lane 2 cho BCM43752 Wi-Fi.
- Bật UART9/serdev cho BCM4362A2 Bluetooth.
- Mô tả regulator, reset, wake GPIO và 32.768 kHz LPO clock.

Firmware Broadcom bắt buộc nằm trong `config/firmware/brcm`. Ubuntu 26.04
`linux-firmware-broadcom-wireless` chưa ship đúng `brcmfmac43752-pcie.*` cho
board này, nên image tự copy firmware riêng và tạo alias `xunlong,orangepi-5b`.


## Runtime checks

Sau khi boot, kiểm tra nhanh:

```bash
uname -a
systemd-analyze
journalctl -k -b -o short-monotonic | grep -Ei 'drm|hdmi|vop|rockchip'
cat /sys/class/drm/card0-HDMI-A-1/modes 2>/dev/null || true
systemctl status ssh --no-pager
```

Kiểm tra Wi-Fi/Bluetooth:

```bash
lspci -nn | grep -i '14e4:449d'
ip -br link
rfkill list
systemctl status bluetooth --no-pager
bluetoothctl list
journalctl -k -b -o short-monotonic | grep -Ei 'brcm|bcm|hci|bluetooth|pcie|firmware'
```

Nếu cần kiểm tra SSH host keys:

```bash
ls -l /etc/ssh/ssh_host_*_key
sudo systemctl restart ssh
```
