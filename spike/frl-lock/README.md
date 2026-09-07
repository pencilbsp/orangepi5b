# FRL link-lock spike

Spike một ngày, trả lời đúng một câu hỏi:

> **RK3588S HDMI TX trên board này có train được Fixed Rate Link với màn hình đang cắm không?**

Không nhằm ra hình đẹp, không nhằm mở mode mới, không nhằm ship.

## Đã chạy — và kết quả phải đọc lại

Orange Pi 5B + Dell U2725QE, 2026-09-07.

**Bản đầu của spike dừng ở LTS3 và báo PASS. Đó là kết luận sai.** Sau khi
implement nốt LTSP (pha bắt đầu stream), hoá ra sink không hề yêu cầu bắt đầu
stream — nó yêu cầu **train lại**:

```text
frl-spike: LTS3 passed at TxFFE level 0
frl-spike: sink asked to retrain, round 2
frl-spike: FAIL (-5)          ← DDC chết ở vòng train thứ hai, ngay tại FRL3
```

Bằng chứng đi kèm, lấy trong 60 giây giữ link ở bản trước đó:

```text
sink[0x40]=40   FLT_ready bật
[0x41]=00 [0x42]=00   không lane nào còn đòi LTP
[0x10]=21       UPDATE_0 = FLT_update + Status_update — sink liên tục xin train lại
ctrl_scdc=00000000   SCDC_STATUS0 bit4 (FRL_start) KHÔNG BAO GIỜ bật
```

Màn hình báo "no signal" trong suốt 60 giây đó, hoàn toàn nhất quán: handshake
chạy, nhưng **FRL stream chưa bao giờ bắt đầu**.

### Hệ quả: chưa từng có link FRL nào hoạt động, ở bất kỳ rate nào

"FRL3 PASS" trước đây chỉ có nghĩa là *dừng đo sớm*. Khi chạy tiếp một vòng nữa,
FRL3 hỏng đúng kiểu FRL4/5/6 — `i2c read error` ~50 ms sau khi phát LTP.

Vì vậy bảng "trần 24 Gbps" bên dưới **không phải là trần băng thông**. Nó chỉ ghi
lại *handshake đi được bao nhiêu bước trước khi gặp cùng một lỗi*:

### Handshake đi được bao xa

| FRL | lanes × Gbps/lane | Aggregate | Đi tới đâu |
|---|---|---|---|
| FRL2 | 3 × 6 | 18 Gbps | LTS3 pass, chưa thử vòng LTSP |
| FRL3 | 4 × 6 | 24 Gbps | LTS3 pass → sink xin retrain → DDC chết ở vòng 2 |
| FRL4 | 4 × 8 | 32 Gbps | DDC chết ngay trong LTS3 vòng 1 |
| FRL5 | 4 × 10 | 40 Gbps | như FRL4 |
| FRL6 | 4 × 12 | 48 Gbps | như FRL4 |

Cùng một lỗi (`i2c read error` ~50 ms sau lệnh ghi `FLT_CONFIG1`), chỉ khác ở
việc rate thấp thì sống thêm được một vòng. **Không có bằng chứng nào cho một
trần 24 Gbps.**

### Những gì đã loại trừ

| Giả thuyết | Kết luận |
|---|---|
| Cáp | **Loại.** MacBook M4 chạy 4K120 30-bit qua đúng sợi cáp, OSD báo `Stream Info: 12Gbps 4-Lane` = FRL6. |
| Thiếu TxFFE | **Loại.** Đã implement đầy đủ (phy op `set_ffe`, `phy_configure_opts_hdmi.frl.ffe_lv/ffe_mode`, bậc thang động trong LTS3) và khai báo max level 3 với sink. Không đổi gì: sink chết trước khi kịp đòi FFE. |
| Sai thứ tự opmode | **Loại.** Đã sửa cho khớp BSP (đặt `OPMODE_FRL` trước `phy_power_on`). Không đổi gì. |
| Lỗi port ở PHY | **Loại.** Diff toàn bộ đường FRL với BSP `develop-6.1`: `rk_hdptx_common_cmn_init_seq` (69 writes), `common_lane_init_seq` (52), `common_sb_init_seq` (4), `frl_lane_init_seq` (28), `frl_lcpll_cmn_init_seq` (42), `frl_lcpll_ropll_cmn_init_seq` (56), `frl_lntop_init_seq` (7) — **giống hệt từng byte**, cùng với bảng `rk_hdptx_frl_lcpll_cfg[]` và `rk_hdptx_frl_lcpll_cmn_config()`. |
| Thiếu `link_clk` | **Loại.** BSP `clk_set_rate(link_clk, lanes × rate × 100 MHz)` chính là cách BSP lập trình PLL của PHY qua CCF; upstream làm việc đó qua `phy_configure()` với đúng con số ấy. Tương đương. |

### Giả thuyết đã chết (ghi lại để không ai đi lại)

- **DDC hand-polling.** Từng nghi spike nhồi I2C mỗi ~500 µs làm sập DDC. Kiểm
  chứng: BSP `dw_hdmi_qp_flt_lts3()` làm **y hệt** — `usleep_range(400,500)` +
  `drm_scdc_readb(SCDC_UPDATE_0)`. Và `SCDC_CONFIG0` chỉ xuất hiện một lần trong
  cả file BSP, ở IRQ handler, để *tắt* poll engine. Không phải khác biệt.
- **VOP bị PLL của PHY kéo theo.** Ở 4K60 8-bit `vop2_crtc_atomic_enable` có thể
  reparent `vp->dclk` sang `pll_hdmiphy0`, và spike reprogram PLL đó sang 2.4 GHz.
  Kiểm chứng qua `clk_summary`: `dclk_vop0` đang lấy từ `dclk_vop0_src` (CRU hệ
  thống), `clk_hdmiphy_pixel0` rate 0. Không xảy ra.

### Khác biệt cấu trúc còn lại — và nó lớn

BSP gọi `dw_hdmi_qp_setup()` (modeset FRL đầy đủ: video timing, packet scheduler,
infoframe, op mode) **rồi mới** `queue_work(flt_work)`. Spike thì nóng-tráo một
pipeline đang chạy TMDS sang FRL: controller vẫn mang cấu hình video của TMDS.

Đây là thứ một spike không vá được. Muốn qua chỗ này phải làm **bản port thật**
— tích hợp FRL vào đường modeset, tức khối ~1000–1300 dòng đã ước lượng từ đầu.

### Còn lại

Register programming khớp driver của chính Rockchip từng byte, nhưng >6 Gbps/lane
không lock. Hai khả năng:

1. **Giới hạn analog của bo mạch.** Orange Pi 5B là bo nhỏ, tối ưu giá; 8–12
   Gbps/lane đòi hỏi routing tốt. MacBook chứng minh cáp và màn hình, không
   chứng minh PCB của Orange Pi.
2. **Một bước hiệu chỉnh nằm ngoài các chuỗi register đã so.** BSP là driver
   5452 dòng; tôi mới so được phần PHY và phần FLT.

Có một chi tiết nghiêng về khả năng 2: khi không lock được, theo spec sink phải
xin hạ rate (LTP `0xf`). Nó không xin — nó **chết im lặng**. Tín hiệu yếu-nhưng-
hợp-lệ sẽ sinh ra yêu cầu hạ rate; tín hiệu *không hợp lệ* mới làm receiver treo
như vậy. Điều đó nghiêng về sai cấu hình PHY hơn là thiếu margin.

Hệ quả cho mục tiêu:

| Mục tiêu | Link cần | FRL cần | Trạng thái |
|---|---|---|---|
| 4K60 RGB 10-bit | ~20.1 Gbps | FRL3 | ✅ **nằm trong vùng đã chứng minh** |
| 4K60 RGB 12-bit | ~24.1 Gbps | FRL3 (sát trần) | ⚠️ ngay ranh giới |
| 4K120 RGB 10-bit | ~33.6 Gbps | FRL5 | ❌ chưa train được (và VOP2 cũng chặn) |

Thư mục này **không** nằm trong `config/patches/linux-7.1.8/series`, nên
`scripts/build-boot-image.sh` không bao giờ đưa nó vào image.

## Vì sao spike này không làm hỏng boot

Patch không đụng vào đường modeset bình thường. FRL chỉ chạy khi có người ghi
vào sysfs:

```
/sys/devices/platform/fde80000.hdmi/frl_spike
```

Boot xong màn hình vẫn lên TMDS như hiện tại. Nếu thí nghiệm hỏng, chỉ mất hình
ở thời điểm kích hoạt — SSH không liên quan tới HDMI nên vẫn vào được để đọc log
và reboot.

Điều đó **không** làm serial console thành thừa. Rủi ro còn lại là module mới
gây oops lúc probe, khi đó board không lên mạng và không có UART thì chỉ còn cách
tháo eMMC/thẻ nhớ ra flash lại. Có UART thì nên cắm.

## Điều kiện tiên quyết

- Host đã build image ít nhất một lần, tức `sources/linux-7.1.8` còn nguyên và
  đã build xong (có `vmlinux`). Spike build tăng dần trên cây đó.
- `target.json` trỏ đúng board, SSH vào được.
- **Không chạy `scripts/build-boot-image.sh` trong lúc spike.** Script đó
  `rm -rf sources/linux-7.1.8` mỗi lần chạy, mất cả patch lẫn build tăng dần.

## Quy trình

```bash
bash spike/frl-lock/build.sh
```

Áp patch (idempotent) và build lại ba module bị đụng: `dw-hdmi-qp.ko`,
`rockchipdrm.ko` và `phy-rockchip-samsung-hdptx.ko`. Kết quả nằm trong
`spike/frl-lock/out/`.

```bash
bash spike/frl-lock/deploy.sh
```

Copy module sang board, backup bản gốc vào `/root/frl-spike-backup` (chỉ lần
đầu), cài `probe.sh` thành `/usr/local/bin/frl-probe`, rồi reboot.

> Script chỉ chép module vào `/lib/modules`. Nhưng initramfs của image mang bản
> copy riêng của cả ba module (để có hình trước Plymouth),
> và boot nạp từ đó — nên sau reboot vẫn là module **cũ**. Đó là tính năng chứ
> không phải lỗi: nó biến reboot thành rollback tuyệt đối. Nạp bản spike ở bước
> tiếp theo, lúc runtime.

```bash
ssh pencil@<board> 'sudo systemctl stop gdm3; sleep 4; \
  sudo modprobe -r rockchipdrm dw_hdmi_qp && \
  sudo modprobe -r phy_rockchip_samsung_hdptx && \
  sudo modprobe phy_rockchip_samsung_hdptx && sudo modprobe rockchipdrm'
ssh pencil@<board> 'sudo systemctl start gdm3'
ssh pencil@<board> 'ls -l /sys/devices/platform/fde80000.hdmi/frl_spike'
```

Phải dừng GDM trước, nếu không DRM device còn bị giữ và `modprobe -r` fail. Bật
lại GDM để có modeset thật — spike từ chối chạy nếu không có modeset active.
Thấy file `frl_spike` xuất hiện là đã nạp đúng bản spike.

```bash
ssh pencil@<board> 'sudo frl-probe 6'
```

Kích hoạt FRL3 (6 Gbps/lane × 4 lane = 24 Gbps) trên đúng mode đang chạy. Script
in kết quả và lưu `dmesg` + trạng thái DRM vào `/root/frl-probe-<timestamp>/`.

Ghi thẳng vào sysfs cho phép chọn số lane: `echo 3:6 > .../frl_spike` chạy FRL2
(3 lane × 6 Gbps). Đó là cách tách bạch trần *mỗi lane* khỏi trần *băng thông*.

## Đọc kết quả

| Kết quả | Log | Nghĩa là gì |
|---|---|---|
| **PASS** | `frl-spike: PASS` | Sink báo cả 4 lane train xong. Câu hỏi của spike đã trả lời: FRL khả thi. Phần còn lại của port là việc thẳng. |
| **FAIL-LTS2** | `sink never raised FLT_ready` | Sink không nhận ra source đã vào FRL. Sai ở phía enable: GRF `HDMI21` bit, `OPMODE_FRL`, hoặc PHY chưa thực sự ra FRL. Đây là loại lỗi *dễ* sửa — đáng bỏ thêm nửa ngày. |
| **FAIL-LTS3** | `LTS3 timed out` / `requested more TxFFE` | Sink có train nhưng không đạt. TxFFE động đã được implement (max level 3), nên nếu vẫn thấy dòng này thì đã hết headroom thật. |
| `i2c read error` sau LTP đầu tiên | — | Signature của trần >6 Gbps/lane trên bo này. Sink chết im lặng thay vì xin hạ rate. |
| `requested a rate change` | LTS4 | Sink muốn hạ rate. Cần LTS4 (~39 dòng BSP), chưa implement. |

Nói cách khác: chỉ **FAIL-LTS2** mới là tin xấu thật sự.

## Quay lại trạng thái cũ

**Chỉ có một cách: reboot.** Và nó sạch — module lúc boot đến từ initramfs chứ
không phải `/lib/modules`, nên board tự về đúng bản gốc và GDM lên lại bình
thường, không cần làm gì thêm.

> Đã học bằng cách làm sai. Bản đầu của spike có `frl-probe 0` để "trả về TMDS".
> Nó **treo cứng board**, không oops, không một dòng log nào sau `-22`. Lý do:
> sau khi FRL train xong, atomic_check kế tiếp fail `-EINVAL` (DRM vẫn hỏi PHY
> một TMDS char rate trong khi PHY đang ở FRL mode), DRM tear down CRTC rồi
> `phy_power_off`. Đường restore lúc đó ghi `FLT_CONFIG1`/`LINK_CONFIG0` và đẩy
> I2C qua DDC vào một controller đã mất clock — bus access treo chết máy.
>
> Đường đó đã bị gỡ khỏi patch. `dw_hdmi_qp_frl_spike()` giờ cũng từ chối chạy
> nếu không có modeset active (`hdmi->curr_conn == NULL`), và `frl-probe 0`
> báo lỗi thay vì thử.

> Lần treo thứ hai, cùng một bài học. Phần quan sát sau PASS ban đầu đọc cả
> `FRL_RSFEC_STATUS0` (0xa30) và `FRL_PKTZ_STATUS1` (0xa54). Board treo cứng
> trên lần chạy lại FRL3 — journal của boot đó chỉ có vài dòng early-boot rồi
> hết. Hai register đó nằm trong sub-block không chắc được cấp clock, nên đã bị
> gỡ. Chưa xác nhận 100% là thủ phạm, nhưng đó là thứ duy nhất mới. Quy tắc rút
> ra: **trong spike này, chỉ đọc những register mà link training vừa dùng thành
> công.**

Gỡ hẳn spike khỏi `/lib/modules` (không bắt buộc — initramfs vẫn thắng):

```bash
ssh pencil@<board> 'sudo cp /root/frl-spike-backup/rockchipdrm.ko \
  /lib/modules/$(uname -r)/kernel/drivers/gpu/drm/rockchip/ && \
  sudo cp /root/frl-spike-backup/dw-hdmi-qp.ko \
  /lib/modules/$(uname -r)/kernel/drivers/gpu/drm/bridge/synopsys/ && \
  sudo depmod -a'
```

Trên host, gỡ patch khỏi cây kernel:

```bash
patch -d sources/linux-7.1.8 -p1 -R < spike/frl-lock/0001-hdmi-qp-frl-link-training-spike.patch
```

## Spike này cố tình không làm

- **LTS4** (sink đòi đổi rate) — báo lỗi thay vì đàm phán.
- **TxFFE** — khai báo max FFE level 0, từ chối yêu cầu tăng. Vì vậy link margin
  thấp; đây là nguyên nhân số một nếu FAIL-LTS3.
- **Vòng giám sát LTSP** — không phát hiện sink rớt link sau khi train xong.
- **Audio, DSC, hotplug, suspend/resume.**
- **Đường về TMDS** — một chiều, xem mục rollback.
- **DRM core** — không đụng `drm_hdmi_state_helper.c`, nên không có mode mới nào
  xuất hiện với userspace. Spike chạy FRL trên mode hiện tại.

Vì vậy PASS **không** có nghĩa là 4K60 10-bit chạy được ngay. Nó chỉ có nghĩa là
tầng khó nhất và ít tài liệu nhất của port đã được chứng minh trên phần cứng thật.

## Nguồn tham chiếu

Sequence FLT được rút từ Rockchip BSP `develop-6.1`,
`drivers/gpu/drm/bridge/synopsys/dw-hdmi-qp.c`, các hàm `dw_hdmi_qp_flt_lts1`
đến `dw_hdmi_qp_flt_ltsp`. Offset SCDC sink (0x31 / 0x35 / 0x42) lấy từ
`dw-hdmi-qp.h` của BSP; upstream `include/drm/display/drm_scdc.h` chưa có.
