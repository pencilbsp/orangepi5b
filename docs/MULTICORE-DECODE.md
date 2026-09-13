# Multi-core decode trên RK3588S — hướng đi và liệu có đáng làm

RK3588S có **hai** core rkvdec. Kernel chỉ dùng một. Tài liệu này ghi lại đã
kiểm chứng được gì, hướng thêm multi-core là gì, và — phần quan trọng hơn —
**liệu nó có đáng làm cho dự án này không**.

Phần phân tích khung ban đầu đến từ project tham chiếu; mọi dữ kiện dưới đây
đã được kiểm lại trên cây `linux-7.1.8` của repo này.

## Đây là việc của kernel, không phải của VA driver

Nói ngay để khỏi ai đi nhầm đường: `libva-v4l2-request` **không đóng góp gì**
được vào việc này, và cũng **không cần sửa gì** khi nó xong.

## Phần cứng có sẵn hai core đầy đủ

`rk3588-base.dtsi`, đã kiểm trên cây này:

| | `vdec0` (dòng 1357) | `vdec1` (dòng 1389) |
|---|---|---|
| thanh ghi | `0xfdc38000` | `0xfdc40000` |
| ngắt | `GIC_SPI 95` | `GIC_SPI 97` |
| clock | `ACLK_RKVDEC0`, `CLK_RKVDEC0_CORE`… | bộ `RKVDEC1` |
| IOMMU | `vdec0_mmu` | `vdec1_mmu` |
| power domain | `RK3588_PD_RKVDEC0` | `RK3588_PD_RKVDEC1` |
| SRAM | `vdec0_sram` | `vdec1_sram` |

**Mỗi core được cấp tài nguyên độc lập hoàn toàn.** Không có gì trong mô tả
phần cứng ngăn hai core chạy cùng lúc.

## Vì sao userspace không với tới core thứ hai

`drivers/media/platform/rockchip/rkvdec/rkvdec.c`:

```c
static int rkvdec_disable_multicore(struct rkvdec_dev *rkvdec)
{
	...
	is_first_core = (rkvdec->dev->of_node == node);
	if (!is_first_core) {
		dev_info(rkvdec->dev, "missing multi-core support, ignoring this instance\n");
		return -ENODEV;
	}
```

Probe của core thứ hai trả `-ENODEV`, không đăng ký V4L2 device, nên không có
`/dev/video*` để mở.

Chú thích ngay trên hàm đó nêu rõ thiết kế mà upstream muốn — nên theo, đừng tự
nghĩ kiểu khác:

> Exposing separate devices for each core to userspace is bad, since that does
> not allow scheduling tasks properly (and creates ABI). … Once the driver
> gains multi-core support, the same technique for detecting the first core can
> be used to cluster all cores together.

Tức là: **một V4L2 device, các core gom lại phía sau, điều phối bên trong
driver.**

## Framework không phải chướng ngại

`v4l2-mem2mem` chạy đúng một job mỗi device (`curr_ctx` là số ít), nên thoạt
nhìn tưởng phải sửa core framework. **Không phải.**

MediaTek đã lái hai khối phần cứng trong mainline mà không đụng framework —
`drivers/media/platform/mediatek/vcodec/decoder/`, có sẵn trong cây này:

```
device_run() -> chạy tầng LAT -> v4l2_m2m_job_finish() NGAY
                              -> queue_work(core_workqueue) -> tầng CORE
```

Job m2m chỉ bao tầng đầu. Vì `job_finish` được gọi sớm, m2m rảnh để bắt đầu
frame kế trong khi khối kia còn đang làm frame trước.

rkvdec hiện **giữ job mở tới khi phần cứng xong** — `rkvdec_job_finish()` chỉ
được gọi từ IRQ handler. Đó là chỗ phải đổi.

**Nhưng mô hình song song thì không chuyển sang được:**

| | MediaTek | RK3588 |
|---|---|---|
| hai khối | **pipeline** — LAT parse, CORE dựng ảnh | **hai decoder đầy đủ giống hệt** |
| song song đến từ | frame liên tiếp gối nhau qua hai tầng | frame khác nhau chạy trên core khác nhau |
| theo dõi phụ thuộc frame | không cần | **bắt buộc** |

## Giới hạn thật của phần thưởng

Hai core giống hệt thì decode **frame khác nhau**. Frame N+1 chỉ bắt đầu được
khi các frame nó tham chiếu đã xong. Với nội dung IPPP thuần, **mọi frame đều
tham chiếu frame ngay trước**, nên không có gì để gối — và hai core **không mua
được gì cả**.

Lợi ích chỉ xuất hiện với frame độc lập (B-pyramid, nhiều reference) và với
nhiều stream đồng thời.

Thông tin để quyết định thì có sẵn: decoder stateless nhận reference của mỗi
frame dưới dạng timestamp, mà driver đã phân giải ra CAPTURE buffer. Một frame
dispatch được khi mọi buffer nó tham chiếu đã hoàn tất.

## Đo trên board này — và vì sao tôi nghiêng về KHÔNG làm

Số đo của repo này, một core, decode thuần:

| codec | độ phân giải | fps |
|---|---|---|
| H.264 | 7680x4320 | 18 |
| HEVC | 7680x4320 | 19 |
| H.264 / HEVC | 3840x2160 | vượt xa 60 (chưa đo chính xác) |

Project tham chiếu đo 23.8 / 24.0 fps ở 8K và **60+ fps ở 4K cho mọi codec**.
Hai phép đo độc lập cùng chỉ một trần.

**Ba lý do phần thưởng nhỏ hơn vẻ ngoài:**

1. **4K đã dư trên một core.** Chrome phát YouTube 4K, GRD encode ở 1080p —
   không mục tiêu nào của dự án này chạm trần một core.
2. **Một core đã phục vụ được hai stream 4K đồng thời.** Reference đo: AV1 4K
   cộng VP9 4K hết 8.9 s, chạy lần lượt hết 9.5 s. Cái biện minh cho core thứ
   hai **không phải** là hai stream thường.
3. **8K@60 một stream cần tới Stage 3** (theo dõi phụ thuộc frame), và chính
   nó là phần có thể **không mang lại gì** tuỳ nội dung.

## Nếu vẫn làm: ba giai đoạn

**Stage 1 — tái cấu trúc cho N core, vẫn chạy một. Rủi ro thấp.**
Tách tài nguyên theo core (`regs`, `link`, clock, IRQ, IOMMU domain, SRAM pool,
watchdog, power domain) khỏi `struct rkvdec_dev` sang mảng `struct rkvdec_core`.
Bỏ `rkvdec_disable_multicore()` để core thứ hai đăng ký được, và vẫn dispatch
mọi job vào core 0.
*Thành công*: mọi codec vẫn bit-identical, hành vi không đổi.
*Vì sao trước*: thuần refactor, có sẵn bộ regression (`spike/va-decode-test/`)
để kiểm, và đây là chỗ chứa phần lớn khối lượng code.

**Stage 2 — dispatch qua nhiều core.**
Theo mẫu MediaTek: kết thúc job m2m ngay khi một core đã nhận frame, và điều
khiển hoàn tất bằng workqueue của driver với IRQ handler theo từng core.
*Thành công*: hai stream độc lập làm bận cả hai core — thấy được bằng ngắt trên
cả hai IRQ line cùng lúc.

**Stage 3 — theo dõi phụ thuộc frame.**
Chỉ dispatch một frame khi mọi buffer nó tham chiếu đã hoàn tất. Đây là phần
mới mẻ, và là phần quyết định **một stream đơn** có được lợi hay không.
*Tiêu chí dừng*: nếu nội dung 8K thật hoá ra tuần tự tới mức không bao giờ gối
được, thì dừng — phần còn lại không có phần thưởng.

## Trạng thái thượng nguồn

Collabora — chính nhóm đã đưa VDPU381/383 vào mainline — **đang làm multi-core
cho RK3588**, và nêu rõ vướng mắc: điều phối phải nằm trong kernel, mà V4L2
**chưa có scheduler cho codec**.

Điều này đổi tính toán: nếu tự làm, ta gánh một fork kernel lớn cho thứ thượng
nguồn sắp có. **Theo dõi thượng nguồn hợp lý hơn là tự viết**, trừ khi có nhu
cầu 8K@60 gấp.

## Cách kiểm chứng

```bash
# core bị từ chối, lúc boot
dmesg | grep -i "multi-core"

# chỉ một node rkvdec tồn tại dù DT khai hai instance
for d in /sys/class/video4linux/*; do echo "$(basename $d) $(cat $d/name)"; done

# ngắt theo từng core — một ngắt mỗi frame hoàn tất
grep -iE "vdpu|rkvdec|vdec" /proc/interrupts

# throughput một core, mỗi codec
spike/va-decode-test/compare.sh   # bit-exactness
ffmpeg -hwaccel vaapi ... -f null -   # fps
```

## Câu hỏi còn mở

- Một core có kham nổi **nội dung 8K thật** ở frame rate của nó không? Số đo
  hiện tại lấy từ `testsrc2` mã hoá `ultrafast` — chi tiết cao, bitrate cao,
  khó hơn nội dung 8K thông thường. **Đo bằng nội dung thật trước khi cam kết.**
- Nội dung 8K thật có đủ frame độc lập để Stage 3 mang lại gì không?
