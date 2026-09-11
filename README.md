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
- IBus Bamboo là bộ gõ tiếng Việt mặc định, với bàn phím US làm nguồn nhập dự phòng.
- Ptyxis là Terminal app duy nhất; `gnome-terminal` bị purge sau khi cài desktop để tránh hai launcher cùng tên.


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

Tài khoản mới nhận IBus Bamboo (Telex/Unicode) làm nguồn nhập mặc định. Bàn
phím US vẫn có trong danh sách và có thể chuyển qua lại bằng `Super+Space`;
đây là giá trị mặc định có thể thay đổi trong Settings, không phải policy bị
khóa. Gói ARM64 được cài từ PPA chính thức của Bamboo Engine qua khóa
`Signed-By` riêng trong `/etc/apt/keyrings`.

## Package trim profile

Google Chrome Stable ARM64 `152.0.7977.82-1` là trình duyệt mặc định cho
HTTP/HTTPS và HTML. Phiên bản và SHA-256 được ghim trong `config/chrome.env`
(xác minh ngày 2026-09-07). `scripts/fetch-chrome.sh` tải URL chứa phiên bản
cụ thể, kiểm tra checksum/package/version/architecture và giữ gói trong
`cache/debs/`. Các lần build sau luôn cài đúng gói này, không dùng URL `current`
hay tự chuyển sang bản mới. Hãy lưu giữ cache cho build lâu dài: nếu Google gỡ
gói và cache không còn, build sẽ fail thay vì cài phiên bản khác.
Muốn đổi bản ship phải chủ động cập nhật version và checksum trong file lock.
Việc ghim áp dụng cho bản ship; người dùng vẫn có thể cập nhật Chrome qua APT
sau khi boot.

GNOME Resources được build lại khi cần bằng `scripts/build-resources-cross-package.sh`, chạy `dpkg-buildpackage -aarm64` trong cross chroot amd64 26.04 thay vì compile dưới qemu.
Patch local trong `config/patches/resources-1.10.2/` bổ sung nhận diện thermal
zone RK3588/RK3588S (`package-thermal`, `bigcore0-thermal`,
`bigcore2-thermal`, `littlecore-thermal`) để Orange Pi 5B hiển thị CPU
temperature thay vì `N/A`. RAM properties không được synthesize vì target không
expose DMI/SPD hoặc DMC/devfreq clock đáng tin cậy; Resources vẫn đọc usage từ
`/proc/meminfo`, còn properties giữ `N/A` thay vì hardcode. Artifact patched đặt
tại `output/debs/` và `scripts/install-rootfs.sh` sẽ cài đè gói đó trong rootfs
nếu có.

`config/boot.packages` là danh sách package cần cài. Sau khi desktop package đã resolve dependency/recommends, `scripts/install-rootfs.sh` purge thêm các package trong `config/boot.purge-packages`. Hiện tại danh sách này bỏ `gnome-terminal`/`gnome-terminal-data` vì Ubuntu 26.04 desktop-minimal đã kéo `ptyxis`, và `ptyxis` cung cấp `x-terminal-emulator`. Cuối bước rootfs, script chạy `apt-get clean` và xoá apt lists/pkgcache để không ship cache build vào image runtime.

## Kernel slim profile

Build dùng `config/kernel-slim.disable` để cắt bớt driver không cần cho baseline Orange Pi 5B desktop:

- Giữ Rockchip boot/storage, onboard LAN STMMAC/Motorcomm, USB host/storage/HID, USB-C/Type-C FUSB302, HDMI/DRM Rockchip, Panthor GPU, Rockchip media codecs/RGA, Broadcom Wi-Fi/Bluetooth, HDMI/ES8328/USB/Bluetooth audio và RTC HYM8563/RK808.
- Tắt các ARM64 platform family ngoài Rockchip, vendor Ethernet/WLAN khác, tuner/DVB/radio/SDR, camera CSI/HDMI-RX capture, VM/VFIO/KVM/virtio leaf drivers, ChromeOS EC, GNSS, SAS/server storage, GPU/DRM/bridge/panel không liên quan, XEN/CXL/Infiniband, filesystem cổ/cluster và test/debug suites.
- Build script có guard để fail sớm nếu mất driver bắt buộc hoặc nếu vendor/panel/audio/RTC/PHY thừa bị bật lại ngoài ý muốn.

## Patch policy

Patch queue hiện có ba patch local:

```text
config/patches/linux-7.1.8/0001-rk3588s-orangepi5b-hdmi-4k120.patch
config/patches/linux-7.1.8/0002-arm64-dts-rockchip-enable-orangepi5b-ap6275p.patch
config/patches/linux-7.1.8/0003-rk3588s-orangepi5b-hdmi-frl.patch
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

Patch FRL mở đường Fixed Rate Link, thứ mà upstream hoàn toàn không có:

- `drm_hdmi_state_helper`: cho mode vượt trần TMDS đi tiếp khi hai đầu tải được
  qua FRL, và chọn rate FRL **thấp nhất đủ tải** (Rockchip thì luôn xin rate tối
  đa của sink rồi clamp xuống 40 Gbps).
- `drm_bridge`/`drm_connector`: mang capability FRL phía source.
- `dw-hdmi-qp`: FRL op mode, link training HDMI 2.1 (LTS1..LTS3 và LTSP với bắt
  tay FRL_START), bậc thang TxFFE khi sink đòi thêm cân bằng.
- `dw_hdmi_qp-rockchip`: chọn TMDS hay FRL trong atomic_check, lập trình PHY
  tương ứng, bật bit GRF HDMI21.
- `phy-rockchip-samsung-hdptx`: áp TxFFE khi dựng lane FRL.

Thứ tự lấy theo driver vendor: controller vào FRL op mode **trước** khi cấp
nguồn PHY, và link training chạy **sau** khi mode đã lập trình xong. Train trên
một pipeline dựng dở thì đạt "mọi lane trained" rồi sink xin train lại mãi mãi
mà không ra hình.

Chưa bao gồm: VOP2 vẫn từ chối pixel clock trên 600 MHz, nên 4K120 cần thêm một
thay đổi nữa (chia đôi dclk, chạy 2 pixel mỗi clock).

Firmware Broadcom bắt buộc nằm trong `config/firmware/brcm`. Ubuntu 26.04
`linux-firmware-broadcom-wireless` chưa ship đúng `brcmfmac43752-pcie.*` cho
board này, nên image tự copy firmware riêng và tạo alias `xunlong,orangepi-5b`.


## FRL spike

`spike/frl-lock/` chứa một thí nghiệm một ngày, không nằm trong patch queue và
không bao giờ vào image: nó kiểm tra xem HDMI TX có train được Fixed Rate Link
với màn hình đang cắm hay không. FRL là điều kiện bắt buộc để ra 10-bit RGB ở
4K60, vì driver upstream hiện chỉ chạy TMDS (trần 600 MHz TMDS char rate).

Patch chỉ thêm một trigger sysfs, không đụng đường modeset, nên boot vẫn lên
TMDS như bình thường. Xem `spike/frl-lock/README.md` để biết quy trình, tiêu chí
PASS/FAIL và cách rollback.

Kết quả đo trên phần cứng thật (Dell U2725QE, 2026-09-07): link training chạy
tới LTS3 ở FRL3, nhưng FRL stream chưa bao giờ bắt đầu — sink liên tục xin train
lại và màn hình báo no signal. Chưa có link FRL nào hoạt động ở bất kỳ rate nào.
Chi tiết và danh sách giả thuyết đã loại trừ nằm trong runbook của spike.

`spike/bsp-frl-check/` là phép thử đối chứng, và nó đã cho kết quả: kernel BSP
Rockchip trên chính bo này chạy **4K120 4:4:4 qua FRL5 (10 Gbps/lane, 40 Gbps)**.
Không có trần 24 Gbps, không có trần 6 Gbps/lane, VOP2 kéo được 4K120 — mọi giới
hạn gặp phải đều nằm ở phần mềm upstream, không phải phần cứng. Runbook đó cũng
ghi lại thông số clocking đo được để dùng làm spec cho bản port.


## Vòng lặp thử nghiệm display

Build lại cả image để thử một thay đổi driver là quá chậm. Mọi thứ trong đường
display đều là module (`DRM_DISPLAY_HELPER`, `DRM_ROCKCHIP`, `DRM_DW_HDMI_QP`,
`PHY_ROCKCHIP_SAMSUNG_HDPTX`), nên có hai mức nhanh hơn nhiều.

**Mức 1 — chỉnh lúc chạy, vài giây, không rủi ro.** Driver FRL có một tham số
ghi được lúc chạy:

| Tham số | Ý nghĩa |
|---|---|
| `frl_max_ffe_lv` | Mức TxFFE tối đa khai báo với sink (0–3, mặc định 3) |

```bash
echo 2 | sudo tee /sys/module/dw_hdmi_qp/parameters/frl_max_ffe_lv
```

Đổi xong thì chuyển độ phân giải qua lại để ép train lại.

**Mức 2 — thay module, ~2 phút.**

```bash
bash scripts/deploy-modules.sh
```

Build tăng dần, copy bốn module sang board trong `target.json`, `update-initramfs`
rồi reboot. Quay lại bản gốc:

```bash
bash scripts/deploy-modules.sh --restore
```

> **CẢNH BÁO: cách này KHÔNG an toàn với boot.** Đã làm bo không khởi động được
> và phải flash lại.
>
> Lý do: script chạy `update-initramfs`, tức **ghi lại một artifact boot-critical**.
> Và image này cố tình nhét module display vào initramfs (xem
> `config/rootfs/orangepi5b-display.conf`, để có hình trước Plymouth), nên module
> mới chạy ở **giai đoạn initramfs, trước khi mount root**. Lỗi ở đó là chết boot,
> không phải chỉ mất hình.
>
> Tính chất "reboot = rollback" chỉ đúng khi initramfs còn giữ module **gốc**.
> Ngay khi script regenerate initramfs, lưới an toàn đó biến mất.
>
> Muốn có vòng lặp nhanh mà vẫn an toàn thì phải **bỏ module display khỏi
> initramfs trước** (đánh đổi: mất đồ hoạ sớm/Plymouth), để module lỗi chỉ làm
> mất hình ở userspace còn boot và SSH vẫn sống. Chưa làm.

Script reboot chứ không `rmmod`/`insmod`. Nạp lại DRM stack lúc đang chạy đã làm
sập bo này nhiều lần — worker giám sát, modeset và teardown chạy chồng nhau trên
một controller có thể đã mất clock. Reboot mất ~30 giây và tất định.

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
