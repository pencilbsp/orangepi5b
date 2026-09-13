# Orange Pi 5B — Ubuntu 26.04 GNOME boot/display baseline

Mục tiêu của dự án này là một image mới, gọn và dễ kiểm chứng cho Orange Pi 5B:

- Ubuntu Base 26.04.1 arm64.
- Linux 7.1.8, pinned tại commit `25c76bea853d0db65b51fb4697a47cbfd9e35e76`.
- Bootloader Rockchip đã validated từ dự án cũ: DDR v1.20, BL31 v1.45, U-Boot/Radxa artifact đã boot nhanh trên phần cứng thật.
- GNOME desktop tối thiểu với GDM và first-boot setup wizard.
- HDMI tự chọn mode theo EDID và có patch tối thiểu để hỗ trợ đường 4K120 qua HDMI 2.0/SCDC/YCbCr 4:2:0.
- HDR10 qua HDMI bật được từ Settings → Displays: BT.2020 + PQ ở RGB 10 bpc trên link FRL.
- SSH bật sẵn và SSH host keys được sinh lại trên thiết bị khi boot; tài khoản người dùng được tạo bằng GNOME Initial Setup ở lần boot đầu.
- Panthor/Mali firmware được ship qua `linux-firmware-misc`; kernel bật firmware loader `.zst` để GNOME Wayland có render node ngay từ boot.
- AP6275P Wi-Fi/Bluetooth được bật bằng DT board patch, firmware Broadcom riêng và BlueZ/NetworkManager userspace.
- IBus Bamboo là bộ gõ tiếng Việt mặc định, với bàn phím US làm nguồn nhập dự phòng.
- Chrome dùng VA-API/V4L2 Request để decode phần cứng H.264, HEVC, VP9 Profile 0 và AV1 Profile 0 (NV12 8-bit/P010 10-bit).
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

Launcher hệ thống đi qua `/usr/local/bin/google-chrome-orangepi5b`, đặt đúng
VA driver và bật đường decode Linux GL/zero-copy cùng driver override cho
RK3588S. Launcher ép `--ozone-platform=wayland`; Chrome 152 vẫn chọn X11 khi
để Ozone ở `auto`, khiến lần mở đầu không dựng được đường VAAPI trên phiên
GNOME Wayland. Desktop entry cùng application ID nằm dưới `/usr/local/share`, nên
cập nhật gói Chrome không làm mất launcher phần cứng này.

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

Danh sách đầy đủ và thứ tự patch nằm trong
`config/patches/linux-7.1.8/series`. Năm patch đầu thuộc display path:

```text
config/patches/linux-7.1.8/0001-rk3588s-orangepi5b-hdmi-4k120.patch
config/patches/linux-7.1.8/0002-arm64-dts-rockchip-enable-orangepi5b-ap6275p.patch
config/patches/linux-7.1.8/0003-rk3588s-orangepi5b-hdmi-frl.patch
config/patches/linux-7.1.8/0004-rk3588s-orangepi5b-vop2-4k120.patch
config/patches/linux-7.1.8/0005-rk3588s-orangepi5b-hdmi-hdr.patch
```

Các patch media tiếp theo giữ V4L2 Request H.264/HEVC ổn định, thêm H.264
encode, VP9 và AV1. Riêng AV1 cần patch `0013` để gắn entropy CDF theo frame
thay vì `refresh_frame_flags` mà VA-API không cung cấp; thiếu patch này decoder
vẫn trả đủ frame nhưng nội dung inter frame không bit-exact. Patch `0014` giới
hạn buffer postprocessor riêng của AV1 theo chín slot reference và cấp chúng
trước pool CAPTURE, tránh cạn/phân mảnh CMA ở 1440p và 4K.

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

Patch VOP2 gỡ trần pixel clock 600 MHz của video port: chia đôi dclk và gửi hai
pixel mỗi clock, theo đúng thông số clocking đo được từ kernel vendor trên bo
này. Không có nó thì 4K120 bị từ chối trước khi tới HDMI.

Patch HDR nối nốt đường HDR10, xem `## HDR10 qua HDMI` bên dưới:

- `drm_bridge_connector`: attach property `Colorspace` (và `Broadcast RGB` đi
  kèm) cho connector HDMI dựng trên bridge.
- `drm_hdmi_state_helper`: coi đổi colorimetry hoặc HDR metadata là
  mode-changing, để InfoFrame mới thực sự ra tới dây; đồng thời ép đường RGB
  của Rockchip về full range khi userspace để `Broadcast RGB=Automatic`.

Firmware Broadcom bắt buộc nằm trong `config/firmware/brcm`. Ubuntu 26.04
`linux-firmware-broadcom-wireless` chưa ship đúng `brcmfmac43752-pcie.*` cho
board này, nên image tự copy firmware riêng và tạo alias `xunlong,orangepi-5b`.


## HDR10 qua HDMI

Đã chạy trên phần cứng thật (Dell U2725QE, 2026-09-08): 3840x2160p120, RGB
10 bpc qua FRL6, EOTF PQ, colorimetry BT.2020. Bật bằng toggle HDR trong
Settings → Displays; mutter 50 chỉ hiện toggle đó khi connector có đủ
`Colorspace`, `HDR_OUTPUT_METADATA` và `max bpc`.

Phần lớn thứ HDR cần đã có sẵn trong 7.1.8 — helper HDMI dựng và gửi InfoFrame
Dynamic Range and Mastering, `dw-hdmi-qp` có sẵn phần ghi packet, và patch 0003
đã đưa link lên 10 bpc. Thiếu đúng hai mảnh, cả hai nằm ở patch 0005:

- Sink chỉ rời SDR khi AVI InfoFrame **cũng** khai BT.2020, mà bit colorimetry
  đó lấy từ property `Colorspace` — không connector HDMI nào dựng trên bridge
  attach nó. Không có property thì userspace không có gì để đặt, mutter báo
  output không hỗ trợ HDR, và `HDR_OUTPUT_METADATA` một mình không đổi gì trên
  màn hình.
- Bridge ghi InfoFrame từ `atomic_enable`, nên commit nào đổi colorimetry hoặc
  HDR metadata mà không đổi timing thì không sinh modeset và không bao giờ ra
  tới dây. Bật HDR đúng là commit như thế.

Kiểm tra trên bo:

```bash
sudo od -An -tx1 /sys/kernel/debug/dri/0/HDMI-A-1/infoframes/hdr_drm
```

Đang bật HDR thì `hdr_drm` bắt đầu bằng `87 01 1a` (type/version/length) và
byte dữ liệu đầu tiên sau checksum là `02` = SMPTE ST 2084 (PQ); trong `avi`,
byte 3 có `C` = extended và `EC` = 6 = BT.2020 (`0x64`).

Patch 0005 cũng xử lý dải lượng tử cho board này. `Broadcast RGB=Automatic`
vẫn có thể được mutter ghi lại sau login, nhưng helper HDMI giờ resolve Auto
thành full-range cho RGB trên đường Rockchip thay vì đi theo mặc định CTA
limited-range của mode CEA. State khỏe khi quay về SDR là
`colorspace=Default`, `HDR_OUTPUT_METADATA` rỗng, `hdr_drm` rỗng và
`is_limited_range=n`.

Mutter cũng không gửi mastering display luminance — blob `HDR_OUTPUT_METADATA`
chỉ có EOTF, phần còn lại bằng 0, nên sink dùng mặc định của nó. Đó là hành vi
của mutter, không phải thiếu sót phía kernel.

Known issue đã xác nhận trên cả HDMI và DisplayPort: sau khi chuyển HDR → SDR,
một số dialog của Google Chrome Wayland 152 vẫn rực như HDR cho tới khi hover
chuột làm dialog repaint.
Tại thời điểm đó kernel đã về SDR sạch (`color-mode=0`, `colorspace=Default`,
`hdr_drm` rỗng, `is_limited_range=n`), nên đây là lỗi stale surface/repaint ở
userspace chứ không phải packet HDR còn sót. Launcher board tắt
`WaylandWpColorManagerV1`, là workaround A/B đã xác nhận loại bỏ lỗi chuyển
HDR → SDR. Đổi lại Chrome không còn gắn color description HDR qua protocol này;
HDR của output và các ứng dụng khác không bị tắt. `scripts/display-color-mode.py`
và `scripts/chrome-hdr-ab-test.sh` dùng để tái hiện và đối chứng:

```bash
/tmp/display-color-mode.py hdr --no-kernel-dump
/tmp/chrome-hdr-ab-test.sh no-wpcolor
/tmp/display-color-mode.py sdr --no-kernel-dump
```

Mode `normal` vẫn được giữ để kiểm tra lại khi Chrome/Mutter sửa đường repaint.


## HDR10 qua USB-C DisplayPort

Đã chạy trên Dell U2725QE (2026-09-12) qua USB-C DP Alt Mode: HBR3 8,1 Gbit/s
x4, 10 bpc, HDR10/PQ ở cả 3840x2160p60 và 3840x2160p120. 4K60 dùng RGB 10-bit;
4K120 dùng YCbCr 4:2:2 10-bit vì DP 1.4 HBR3x4 chỉ có 25,92 Gbit/s payload,
thấp hơn khoảng 35,64 Gbit/s của mode RGB10 nhưng đủ cho khoảng 23,76 Gbit/s
của 4:2:2 10-bit.

Patch `0007zb` attach `Colorspace` và `HDR_OUTPUT_METADATA`, gửi VSC SDP
BT.2020 cùng HDR Static Metadata SDP. Khi link chọn 4:2:2, glue Rockchip cũng
đổi VOP2 sang ma trận RGB→YCbCr BT.2020 và driver phát colorimetry BT.2020 YCC.
Nếu chỉ đổi packet mà VOP2 vẫn dùng CSC BT.709, hoặc phát BT.2020 RGB trên dữ
liệu YCbCr, 4K120 HDR vẫn có hình nhưng toàn màn hình bị nhợt màu.

`0007c-drm-dw-dp-report-aux-detect-errors.patch` là patch chẩn đoán tùy chọn,
cố ý không nằm trong `series` và không được ship trong image production. Chỉ
áp dụng thủ công khi cần phân biệt lỗi detect do PHY power-on, AUX timeout hay
DPCD/link parsing; bỏ patch sau khi điều tra để tránh làm log kernel ồn.


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

## Dump SCDC từ userspace

`scripts/scdc-dump.py` đọc thẳng SCDC của màn hình qua `/dev/i2c-*` (địa chỉ
0x54 trên bus DDC). Không đụng driver, không cần patch, không cần reboot —
`CONFIG_I2C_CHARDEV=y` là đủ.

Dùng để trả lời câu hỏi đang treo về hiện tượng nháy ở 4K120/FRL6: khi màn hình
nháy, **sink có báo lỗi link nào không**.

```bash
scp scripts/scdc-dump.py board:/tmp/
ssh board 'sudo python3 /tmp/scdc-dump.py --once'                 # dump một lần
ssh board 'sudo python3 /tmp/scdc-dump.py --log /tmp/scdc.log'    # theo dõi liên tục
```

Chế độ mặc định poll mỗi giây và **chỉ in ra byte nào thay đổi**. Đọc kết quả:

- Có byte nào đó bò lên đều đặn khi đang chạy → đó là bộ đếm lỗi, link đang lỗi
  thật → vấn đề nằm ở analog/margin ở 12 Gbps mỗi lane.
- Nháy mà không byte nào trong cả dải nhúc nhích → không phải lỗi link, phải
  tìm chỗ khác.

Bản đồ thanh ghi SCDC cho FRL không được tài liệu hoá rõ và driver Rockchip
cũng không đọc chúng, nên công cụ cố tình **không đoán**: nó dump cả dải
0x00–0x5F và để số liệu tự chỉ ra thanh ghi nào có ý nghĩa. Chỉ những trường đã
chắc chắn mới được chú thích tên.

Hai lưu ý: một số bộ đếm lỗi SCDC là read-to-clear (đừng chạy song song với
công cụ khác cũng đọc SCDC), và mỗi lần poll là thêm lưu lượng trên bus DDC —
bus này đã tỏ ra mong manh trên phần cứng đây, nên giữ nhịp poll vừa phải.
