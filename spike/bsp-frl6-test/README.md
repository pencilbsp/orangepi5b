# BSP FRL6 validation image

Mục tiêu của phép thử này là kiểm chứng xem HDMI/PHY BSP của Rockchip có chạy
ổn định ở **FRL6, 4 lane × 12 Gbps** trên Orange Pi 5B hay không.

Thay đổi kernel duy nhất nằm trong:

```text
drivers/gpu/drm/rockchip/dw_hdmi-rockchip.c
```

Patch xóa block clamp:

```c
/* in the current version, support max 40G frl */
if (hdmi->link_cfg.rate_per_lane >= 10) {
        hdmi->link_cfg.frl_lanes = 4;
        hdmi->link_cfg.rate_per_lane = 10;
}
```

Vì selector của BSP vốn dùng FRL capability tối đa từ EDID, màn hình Dell
U2725QE quảng cáo FRL6 thì 4K120 sẽ đi thẳng lên `12Gbps 4-Lane`.

## Artifact đang dùng: FRL6 clean

```text
output/images/ubuntu-26.04.1-bsp-6.1.75+-orangepi5b-frl6clean.img.xz
sha256 66adf72ff744dcad3bd98a3b2e9a3de65f9de5f4b8ce967f1700f32d975e23d1
size 1405955360 bytes
```

Image dùng rootfs Ubuntu 26.04 của dự án và kernel/DTB/modules BSP Rockchip
`6.1.75+` build từ `Joshua-Riek/linux-rockchip` nhánh `noble`, commit
`e21cf49ee9a4`. Thay đổi kernel duy nhất là bỏ clamp FRL 40 Gbps trong
`drivers/gpu/drm/rockchip/dw_hdmi-rockchip.c`.

User debug `pencil` đã được tạo với mật khẩu lấy từ `target.json`, để vẫn SSH
được nếu HDMI mất hình.

## Artifact cũ

```text
output/images/ubuntu-26.04.1-bsp-6.1.0-1027-rockchip-orangepi5b-frl6test.img.xz
sha256 31e48956bb3ce584d364e22c3080a8399e230c57cefb0fbe194f5148bfd2738c
```

Đây là bản FRL6 test ban đầu. Nó cũng chỉ bỏ clamp FRL6, nhưng kernel release
trong image là `6.1.0-1027-rockchip`.

## BAD artifact: không dùng lại

```text
output/images/ubuntu-26.04.1-bsp-6.1.75+-orangepi5b-frl6debug.img.xz
sha256 df6a8b1258c4be782dcd8ffa18a7e8bd34dfc131e547004239a17871e5fa1150
```

Bản `frl6debug` thêm SCDC/CED polling trong `dw-hdmi-qp.c`. Khi chuyển lên
4K120, màn hình chuyển xanh và board mất SSH/treo hoặc reset. Không dùng bản này
để kiểm chứng nháy FRL6 nữa. Patch đã được giữ lại chỉ để tham chiếu tại
`0002-BAD-bsp-frl6-scdc-debug-green-hang.patch`.

## Flash

Thay `/dev/XXX` bằng block device thật:

```bash
xzcat output/images/ubuntu-26.04.1-bsp-6.1.75+-orangepi5b-frl6clean.img.xz | \
  sudo dd of=/dev/XXX bs=16M status=progress conv=fsync
sync
```

## Kiểm tra sau khi boot

```bash
uname -a
cat /etc/frl6test-build-info
dmesg | grep -iE 'frl|flt|hdptx|hdmi' | tail -120
```

Chọn `3840x2160 @ 120 Hz`, rồi đọc OSD màn hình:

- Nếu OSD hiện `12Gbps 4-Lane` và log có `rate:48000000` / `flt success`, test
  FRL6 đã thật sự chạy.
- Nếu FRL6 trên BSP vẫn nháy đen ngẫu nhiên, khả năng cao vấn đề nằm ở
  PHY/signal-integrity/board/cable/sink margin tại 12 Gbps mỗi lane.
- Nếu BSP FRL6 ổn định nhưng bản port 7.1.8 vẫn nháy, lỗi nằm trong phần port:
  init PHY/controller, clocking, FFE, SCDC hoặc pipeline timing.
