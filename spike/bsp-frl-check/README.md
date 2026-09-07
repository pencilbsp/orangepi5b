# Phép thử đối chứng: BSP có chạy được FRL trên bo này không?

Một câu hỏi, và nó quyết định có nên đầu tư tiếp vào port FRL hay không:

> **Kernel BSP của Rockchip có dựng được link FRL trên chính Orange Pi 5B này không?**

- **Có** → bo mạch làm được, lỗi nằm ở phần mềm của ta. Và ta có một tham chiếu
  chạy được để diff, thay vì đoán. Mọi giả thuyết còn lại kiểm chứng trong vài
  phút thay vì một ngày.
- **Không** → giới hạn nằm ở bo/silicon integration. Dừng đầu tư, biết chắc thay
  vì nghi ngờ.

Sau một đêm mà năm trên sáu giả thuyết chết khi kiểm chứng, đây là phép thử cho
nhiều thông tin nhất trên mỗi giờ bỏ ra.

## Vì sao nó an toàn tuyệt đối

Image BSP nằm trên **thẻ microSD**, không đụng một byte nào vào eMMC. Board hiện
boot từ eMMC (`mmcblk0`, không có thẻ SD nào cắm). Rút thẻ ra là về nguyên trạng.

Không cần backup, không cần rollback, không có đường nào hỏng image hiện tại.

> Cảnh báo duy nhất: **không chắc BootRom của RK3588 ưu tiên SD hơn eMMC.** Nếu
> board vẫn boot vào hệ thống eMMC cũ thì phép thử chưa chạy được — lúc đó phải
> tìm cách khác (ví dụ tạm đổi `extlinux.conf`), chứ không phải là hỏng hóc gì.

## Image dùng

```text
ubuntu-24.04-preinstalled-desktop-arm64-orangepi-5b.img.xz
Joshua-Riek/ubuntu-rockchip v2.4.0
sha256 746cc21d39427003fecc7615c76e67e0715e58bdcf8b4b14bc5eb39ace59f81f
```

Ubuntu 24.04 với **kernel Rockchip 6.1** — đúng nhánh BSP mà toàn bộ việc diff
trong `spike/frl-lock/` đã dựa vào. Bản desktop chứ không phải server, để chọn
mode trong GNOME và đọc OSD của màn hình.

## Chuẩn bị

Cần một thẻ microSD **≥ 16 GB**. Cắm vào board — nó sẽ hiện ra thành
`/dev/mmcblk1`. Không cần format trước; sẽ bị ghi đè toàn bộ.

Image được tải sẵn về `~/bsp/` trên chính board rồi ghi thẳng sang thẻ, nên không
phải chuyển 1.86 GB qua mạng hai lần.

## Ghi thẻ

```bash
xzcat ~/bsp/ubuntu-24.04-orangepi-5b.img.xz | \
  sudo dd of=/dev/mmcblk1 bs=16M status=progress conv=fsync
sync
```

Rồi `sudo systemctl poweroff`, bật lại nguồn.

## Phép thử trên BSP

Đăng nhập image BSP (user `ubuntu`, mật khẩu `ubuntu`, sẽ bị bắt đổi lần đầu).

**Bước 1 — ép sang FRL.** Settings → Displays → chọn **3840x2160 @ 120 Hz**.

Vì sao mode này: `hdmi_select_link_config()` của BSP chỉ chọn FRL khi
`tmdsclk >= HDMI20_MAX_RATE || mode.clock >= HDMI20_MAX_RATE`. 4K120 có
`mode.clock` = 1188000 kHz, vượt xa 600000 → **buộc phải đi FRL**. Cách khác là
4K60 ở 10-bit (`tmdsclk` = 742500 kHz), cũng buộc FRL.

**Bước 2 — đọc OSD màn hình.** Menu → Display Info → dòng **`Stream Info`**.

| Hiện gì | Kết luận |
|---|---|
| `12Gbps 4-Lane` / `10Gbps 4-Lane` / `8Gbps 4-Lane` | **Bo mạch chạy FRL được.** Lỗi ở phần mềm của ta. |
| `6Gbps 4-Lane` | FRL chạy nhưng bo chỉ lên tới FRL3 — giả thuyết trần 24 Gbps đúng. |
| `-` hoặc trống | BSP đang chạy TMDS, chưa vào FRL. Xem bước 3 để biết vì sao. |
| Không có tín hiệu | BSP cũng không dựng nổi FRL ở mode này. |

**Bước 3 — đối chứng phía kernel.** BSP log rõ ràng khi FLT thành công:

```bash
dmesg | grep -iE "frl|flt"        # "flt success" = link training xong
sudo cat /sys/kernel/debug/dri/0/summary
```

`flt success` là chuỗi BSP in trong `dw_hdmi_qp_flt_ltsp()` ngay sau khi nhận
FRL_START và mở video datapath — đúng bước mà spike của ta không bao giờ tới.

## KẾT QUẢ: bo mạch chạy được FRL5

Chạy ngày 2026-09-07, kernel `6.1.0-1025-rockchip`, cùng bo / cùng cáp / cùng màn hình.

OSD màn hình: **`Stream Info: 10Gbps 4-Lane`** ở `3840x2160, 120Hz 24-bit`.
Tức **FRL5, 40 Gbps aggregate, 10 Gbps mỗi lane**, và 24-bit nghĩa là 4:4:4 đầy
đủ chứ không phải 4:2:0.

Kernel log xác nhận:

```text
hdptx_lcpll_cmn_config rate:40000000        PLL cho 40 Gbps
bus_width:0x42625a00, bit_rate:40000000     0x42625a00 & 0xfffffff = 40000000 kHz
                                            bit30 = HDMI_FRL_MODE
final tmdsclk = 1188000000
goto ltsp
flt success
```

### Cả hai giả thuyết về giới hạn phần cứng đều SAI

| Giả thuyết | Phán quyết |
|---|---|
| "Bo bị trần 24 Gbps aggregate" | **Sai** — chạy 40 Gbps |
| "Bo bị trần 6 Gbps mỗi lane" | **Sai** — chạy 10 Gbps/lane |
| "VOP2 không kéo nổi 4K120 4:4:4" | **Sai** — đang chạy |

**Mọi giới hạn gặp phải đêm qua đều là phần mềm của chúng ta.**

### Thông số tham chiếu đo được (dùng làm spec cho bản port)

| Tầng | BSP ở 4K120 FRL5 | Upstream hiện tại |
|---|---|---|
| `dclk_vop0` | **594 MHz** = v_pixclk/2, parent = `dclk_vop0_src` (CRU hệ thống) | `dclk_rate = v_pixclk` = 1188 MHz → vượt trần 600 MHz, bị từ chối |
| Pixel mỗi clock | 2 | 1 (chỉ chia đôi khi YUV420) |
| `clk_hdmiphy_pixel0` | **4 GHz** (FRL rate / 10) | dùng làm parent của dclk cho mode ≤4K60 |
| VOP output_mode | `AAAA` (0xf) | AAAA / YUV420 |
| bus_format | `YUV8_1X24` (4:4:4) | RGB888 / UYYVYY8 (4:2:0) |
| Link | FRL 4 lane × 10 Gbps | chỉ TMDS |
| FLT | LTS1→LTS3→LTSP→**FRL_START**→`flt success` | spike dừng trước FRL_START |

### Vì sao BSP dừng ở FRL5 chứ không phải FRL6

Không phải phần cứng, không phải sink, không phải đàm phán hạ rate. **Rockchip
clamp bằng phần mềm, có chủ ý**, trong `dw_hdmi-rockchip.c:2606` của đúng kernel
đang chạy (`Joshua-Riek/linux-rockchip` nhánh `noble`):

```c
if (hdmi->link_cfg.frl_mode) {
        /* in the current version, support max 40G frl */
        if (hdmi->link_cfg.rate_per_lane >= 10) {
                hdmi->link_cfg.frl_lanes = 4;
                hdmi->link_cfg.rate_per_lane = 10;
        }
```

Chuỗi bằng chứng đầy đủ:

| Mắt xích | Kết quả |
|---|---|
| EDID màn hình | `hf_scds[7] = 0x63` → max_frl_rate = 6 → **FRL6** |
| `get_max_frl_rate(6)` của BSP | 4 lane × **12** Gbps |
| `hdmi_select_link_config()` | đặt `rate_per_lane = 12` |
| **clamp ở dòng 2606** | **hạ xuống 10, cố định 4 lane** |
| Bảng PLL của PHY | có entry `48000000` → **phần cứng làm được FRL6** |
| Log thực tế | `rate:40000000` ngay lần đầu, không bao giờ thấy 48000000 |

Nghĩa là FRL6 **chưa được chứng minh là không chạy được trên bo này** — nó chỉ
chưa bao giờ được thử, vì driver của Rockchip tự chặn. Comment "in the current
version" cho thấy chính họ coi đây là hạn chế tạm thời.

### Trần 40 Gbps mua được gì

| Mode | Băng thông cần | Trong 40 Gbps? |
|---|---|---|
| 4K120 4:4:4 8-bit | 32.1 Gbps | ✅ (đang chạy) |
| 4K60 4:4:4 10-bit | 20.1 Gbps | ✅ thoải mái |
| 4K60 4:4:4 12-bit | 24.1 Gbps | ✅ |
| 4K120 4:2:0 10-bit | 20.1 Gbps | ✅ |
| **4K120 4:4:4 10-bit** | **40.1 Gbps** | ❌ **thiếu 0.1 Gbps** |

4K120 10-bit đầy đủ chroma trượt khỏi FRL5 đúng một sợi tóc. Muốn có nó thì phải
bỏ clamp và chạy FRL6 — khả thi về phần cứng, chưa ai kiểm chứng về signal
integrity ở 12 Gbps/lane trên bo này.

Bản port của ta **không bắt buộc phải copy clamp đó**.

### Vì sao spike của ta hỏng, đọc từ trace

Trace LTSP của BSP, giải mã từng lần đọc SCDC:

```text
read UPDATE_0 = 0x23   FLT_update
read FLAGS_1  = 0x00
read FLAGS_2  = 0x00   mọi lane pass  →  goto ltsp
write (clear)
read UPDATE_0 = 0x03   chưa có gì
read UPDATE_0 = 0x13   bit4 = FRL_START  ←  sink bảo "bắt đầu đi"
write (clear FRL_START)
flt success
```

Spike của ta ở cùng chỗ đó đọc được `0x21` — bit5 `FLT_update`, tức **"train lại"**.
Cùng một logic LTSP, phản ứng ngược nhau. Khác biệt không nằm ở FLT mà ở thứ ta
đưa cho sink: BSP đưa một FRL stream hợp lệ (VOP đúng clock, đúng output mode,
modeset FRL đầy đủ trước khi train), còn spike đưa một pipeline TMDS đội lốt FRL.

## Lưu ý: image BSP đã được flash vào eMMC, không phải thẻ SD

Runbook này thiết kế cho thẻ SD để không đụng eMMC. Trên thực tế image BSP đã
được ghi thẳng vào **eMMC** (`/dev/mmcblk0p1`, có `boot0/boot1/rpmb`; không có
`mmcblk1` nào tồn tại), nên image 7.1.8 của dự án **đã bị ghi đè**.

Không mất vĩnh viễn — `scripts/build-boot-image.sh` dựng lại toàn bộ; chỉ mất
trạng thái runtime (tài khoản, cấu hình GNOME). Nhưng hệ quả cần nhớ:

- Không còn ổ dự phòng. Flash một kernel tự build mà không boot được là board
  nằm im, phải cứu bằng thẻ SD hoặc maskrom.
- Bootloader dùng extlinux có menu (`prompt 1`, `timeout 20`, `default l0`), nên
  kernel thử nghiệm nên cài thành **entry thứ hai** để boot hỏng thì lần sau tự
  về entry gốc.

## Source tham chiếu

`Joshua-Riek/linux-rockchip` nhánh `noble` là đúng cây nguồn của kernel
`6.1.0-1025-rockchip` đang chạy. Đã clone về `/root/linux-rockchip-bsp` để đối
chiếu khi port.
