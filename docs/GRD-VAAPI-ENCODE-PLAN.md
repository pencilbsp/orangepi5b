# Kế hoạch: VA-API hardware encode cho GNOME Remote Desktop trên RK3588S

Ngày lập: 2026-09-08
Repo đích: `ubuntu-orangepi5b` — Ubuntu 26.04.1, kernel `7.1.8-orangepi5b`
Tham chiếu: `/root/orangepi5b` — `docs/GRD-VAAPI-RKVENC-ENCODE-PLAN.md`, `patches/`, `scripts/`, `tools/`

Chuỗi cần dựng:

```
GNOME Remote Desktop
  -> libva (VAEntrypointEncSlice)
  -> libva-v4l2-request, nhánh encode
  -> V4L2 stateful mem2mem H.264 encoder
  -> RKVENC / VEPU580 trên RK3588S
```

---

## 0. Kết luận trước khi đọc tiếp

**Đây không phải dự án nghiên cứu. Đây là một cuộc port.**

Project tham chiếu đã chứng minh toàn bộ chuỗi chạy end-to-end trên Ubuntu
24.04 ngày 2026-09-02, có số đo runtime:

```text
[HWAccel.VAAPI] Created VAAPI encode session for surface with size 1024x768
/dev/video2 -> fdbd0000.video-codec
RKVENC IRQ: 1964 -> 2186 trong khoảng 7 giây (~32 job/s)
gnome-remote-desktop --handover: khoảng 7% một CPU core
```

Phần khó nhất — driver kernel VEPU580 và nhánh encode của VA driver — **đã
viết xong và đã verify**. Việc còn lại trong repo này là: mang patch queue
sang, vá hai lỗ hổng kernel config, dựng lại VA driver cho libva 2.23, và
**đo lại** vài thứ mà việc lên 26.04 có thể đã tự sửa hoặc tự làm hỏng.

### Trạng thái triển khai — 2026-09-09

Đường GRD encode đã chạy end-to-end trên board với package
`gnome-remote-desktop 50.2-0ubuntu0.1+orangepi5b2`. Handover daemon nạp
`v4l2_request_drv_video.so` và giữ encoder `/dev/video5` cùng media/render
node. Phiên RDP trình bày mode 1920x1080; RKVENC vẫn nhận coded surface
1920x1088 theo macroblock alignment, còn patch SPS cropping loại tám dòng
padding khỏi visible frame.

---

## 1. Điều quan trọng nhất: 26.04 đã xoá phần lớn blocker của 24.04

Reference phải build `gnome-remote-desktop` 50.2 từ source và mang 9 patch,
vì 24.04 ship GRD 46.3 — bản **không có** đường VA-API — trên nền PipeWire
1.0.5. Repo này ship 26.04, và bức tranh đảo ngược:

| Thành phần | Reference (24.04) | Repo này (26.04) | Hệ quả |
|---|---|---|---|
| gnome-remote-desktop | 46.3, không có VA-API → phải build 50.2 | **50.2 từ archive** | Không cần build từ source |
| PipeWire | 1.0.5 (chặn cứng) | **1.6.2** | explicit sync / syncobj timeline có sẵn |
| libei | 1.2.1 | **1.5.0** | shim không cần |
| GDM | 46 | **50.1** | chữ ký `CreateRemoteDisplay` mới, handover là user unit |
| Mesa | 25.2.8 | **26.0.8** | hành vi modifier của PanVK phải đo lại |
| libva | 2.20 | **2.23** | VA driver phải build lại cho ABI mới |
| Decode H.264/HEVC | patch ngoài cây | **có sẵn trong 7.1.8** | Xem §2, thay đổi lớn |
| FreeRDP | 3.30 | 3.31 | vẫn `WITH_GFX_H264=OFF`, xem §4 |

### Đã kiểm chứng trong `build/rootfs`, không phải suy đoán

```text
$ readelf -d usr/libexec/gnome-remote-desktop-daemon
  NEEDED  libva.so.2   libva-drm.so.2   libvulkan.so.1
  NEEDED  libpipewire-0.3.so.0   libei.so.1   libfreerdp3.so.3

$ readelf --dyn-syms ... | grep '^va'
  vaCreateConfig  vaCreateContext  vaBeginPicture  vaRenderPicture
  vaEndPicture    vaExportSurfaceHandle           vaGetDisplayDRM ...
```

GRD từ archive **đã có sẵn toàn bộ đường VA-API + Vulkan**. Workstream D của
reference — phần tốn nhiều công nhất bên userspace — gần như biến mất.

### Số phận 9 patch GRD của reference

| Patch | Trên 26.04 |
|---|---|
| 0001 build cho PipeWire 1.0.5 | **BỎ** — đã có 1.6.2 |
| 0002 fallback libei 1.2.1 | **BỎ** — đã có 1.5.0 |
| 0003 chữ ký `CreateRemoteDisplay` của GDM 46 | **BỎ** — README của reference ghi rõ *"Drop this if the image ever moves to GDM 50 or newer"* |
| 0005 không xin explicit-sync trước 1.2 | **BỎ** |
| 0006 import implicit fence khi PipeWire cũ | **BỎ** |
| 0009 kết thúc greeter khi GDM 46 từ chối Logout | **BỎ**, nhưng verify lại đường greeter |
| 0004 fallback truy vấn modifier Vulkan v1 | **VẪN CẦN** — đã đo 2026-09-08, xem §1bis |
| 0007 log memory-type mismatch | Tuỳ chọn, chỉ để chẩn đoán |
| 0008 không đòi `HOST_CACHED` | **VẪN CẦN** — đã đo 2026-09-08, xem §1bis |

Mục tiêu ban đầu là ship thẳng package archive. **Phép đo đã bác bỏ điều đó:**
phải rebuild GRD từ source package của Ubuntu với 3 patch. Hai patch mở đường
tới phần cứng và patch thứ ba khai báo SPS cropping cho coded surface. Vẫn tốt hơn nhiều
so với 9 patch của reference, nhưng đây là một fork phải nuôi.

---

## 1bis. Ẩn số PanVK: đã đo, đã gỡ — 2026-09-08

Đo trên **chính board đích** (Ubuntu 26.04.1, kernel 7.1.8-orangepi5b, Mesa
26.0.8, PanVK / Mali-G610 MC4, Vulkan 1.4.335). Mã nguồn và kết quả đầy đủ:
phép đo PanVK đã chạy trên board.

### Kết quả

| Câu hỏi | Đáp án |
|---|---|
| Mesa 26 đã trả lời truy vấn DRM modifier bản v2 chưa? | **Chưa** → patch `0004` vẫn cần |
| PanVK đã expose `HOST_CACHED` cùng `HOST_COHERENT` chưa? | **Chưa** → patch `0008` vẫn cần |

```text
=== Mali-G610 MC4 / panvk / Mesa 26.0.8 / API 1.4.335 ===
  format                     v1       v2
  XR24 (XRGB8888)             1        0
  AR24 (ARGB8888)             1        0
  NV12                        1        0        ... cả 6 format đều v2 = 0

  type 1 : DEVICE_LOCAL HOST_VISIBLE HOST_CACHED
  type 2 : DEVICE_LOCAL HOST_VISIBLE HOST_COHERENT
  --> không type nào có đủ HOST_VISIBLE|HOST_COHERENT|HOST_CACHED
```

**PanVK trên Mesa 26.0.8 hành xử y hệt Mesa 25.2.8.** Bug chưa được sửa
thượng nguồn sau một năm.

### Hai điều làm kết quả này đáng tin

1. **llvmpipe trả lời cả v1 lẫn v2** (1/1 cho mọi format). Probe đúng; đây là
   lỗ hổng riêng của panvk, không phải lỗi phép đo.
2. **PanVK có đủ mọi extension khác GRD cần** — `VK_EXT_image_drm_format_modifier`,
   `external_memory_fd`, `external_memory_dma_buf`, `external_semaphore_fd`,
   `timeline_semaphore`, `queue_family_foreign`. Truy vấn modifier là chướng
   ngại **duy nhất** cho việc chọn physical device.

Và không có đường vòng: llvmpipe thiếu `VK_EXT_image_drm_format_modifier` lẫn
`VK_KHR_external_semaphore_fd` nên GRD bỏ qua nó.

### Hệ quả cho kế hoạch

**W4 rẽ sang nhánh "phải rebuild GRD".** Không ship được package archive
nguyên bản. Nhưng phạm vi nhỏ hơn nhiều so với lo ngại ban đầu:

| | Reference (24.04) | Repo này (26.04) |
|---|---|---|
| Số patch GRD | 9 | **3** (hai workaround phần cứng + SPS cropping) |
| Nguồn build | tarball upstream 50.2 | **source package Ubuntu 50.2-0ubuntu0.1** |
| Shim dependency | PipeWire, libei, GDM | **không cần cái nào** |

Dựng từ source package của Ubuntu nghĩa là giữ nguyên mọi thứ Ubuntu đã cấu
hình, chỉ thêm ba patch — nhẹ hơn nhiều so với build từ tarball upstream như
reference đã làm.

### Quyết định cuối: ship bằng patch GRD

Đã chốt không giữ nhánh patch/build Mesa trong repo này. Hướng ship hiện tại là patch GRD nhỏ, rebase rẻ, và chờ bản sửa PanVK đi qua Mesa/Ubuntu ở ngoài luồng image.

### Nguyên nhân gốc, đọc từ source

`panvk_GetPhysicalDeviceFormatProperties2()` trong
`src/panfrost/vulkan/panvk_physical_device.c` xử lý `VkFormatProperties3`,
`VkDrmFormatModifierPropertiesListEXT` và `VkSubpassResolvePerformanceQueryEXT`
— nhưng **không hề xử lý `VkDrmFormatModifierPropertiesList2EXT`**. Client chain
struct bản 2 vào đọc lại đúng số 0 nó tự khởi tạo. Truy vấn "thành công", chỉ là
không ai trả lời.

Patch tách phần thân thành helper điền được cả hai struct, theo đúng hình dạng
`nvk` dùng. **Không đổi tập modifier được báo** — AFBC vẫn nằm sau
`PANVK_DEBUG(WSI_AFBC)`, mặc định vẫn LINEAR, chỉ là giờ trả lời cả hai cách hỏi.

### Kết quả đo trên Mali-G610, Mesa 26.0.8

```text
                     TRƯỚC            SAU
format               v1    v2         v1    v2
XR24 (XRGB8888)       1     0    ->    1     1
AR24 / XB24 / AB24    1     0    ->    1     1
AB30 (ABGR2101010)    1     0    ->    1     1
NV12                  1     0    ->    1     1
```

Build sạch (`ninja` exit 0), memory type không đổi. **GRD patch `0004` thành
thừa.**

### Nhưng `0008` không sửa được ở Mesa — và đây là điều cần biết

Đòi hỏi thứ hai của GRD là memory type có đủ
`HOST_VISIBLE|HOST_COHERENT|HOST_CACHED`. Nhìn vào source, panvk **có** khai báo
type gộp cả hai — nhưng có điều kiện:

```c
if (device->kmod.dev->props.is_io_coherent) {
   /* If the device is coherent, we just have one memory type that's both
    * host-cached and host-coherent. */
```

Và trên board này GPU **thật sự không IO-coherent**: node DT `gpu@fb000000`
không có thuộc tính `dma-coherent`. **panvk đang nói đúng sự thật phần cứng, đây
không phải bug.** Ép nó quảng cáo `HOST_COHERENT` trên bộ nhớ cached mà không có
cache maintenance là nói dối, và có thể gây hỏng dữ liệu thật.

Nên đòi hỏi này phải sửa ở phía GRD: patch `0008`, bỏ yêu cầu `HOST_CACHED` —
vốn chỉ là **gợi ý hiệu năng**, không phải điều kiện đúng đắn. Đó cũng là một
bug đáng gửi lên GRD thượng nguồn.

### Cân đối lại chi phí — cần biết trước khi đi tiếp

Đã cân nhắc hướng patch Mesa để sửa PanVK ở gốc, nhưng quyết định không giữ nhánh đó trong repo này vì chi phí rebuild/rebase Mesa quá lớn so với mục tiêu image hiện tại. Hướng còn lại là patch GRD nhỏ, đóng gói rẻ hơn và ít kéo theo maintenance hơn.

## 1quater. Chi phí build Mesa: số đo, và vì sao không ship hướng đó

Đã build thật hai lần. Số liệu này là căn cứ cho quyết định ở §1bis.

### Lần 1 — trên board: **làm sập board**

`dpkg-buildpackage` với `parallel=8` trên RK3588S, 7.8GB RAM, **không swap**.
Đến phần `libaco` (shader compiler của AMD, C++ nặng) thì tám `cc1plus` đồng
thời làm cạn RAM. RAM tụt 3121MB → 907MB trong bốn lần poll, rồi OOM, SSH chết,
board sập và tự khởi động lại.

Board khởi động lại sạch — không lỗi EXT4, filesystem nguyên vẹn.

**Bài học ghi lại để không lặp:** board **không đủ RAM** cho việc build này, ở
bất kỳ mức `-j` nào đáng dùng. Và điều trớ trêu: thứ giết board là phần ta
không dùng — radeon, intel, nouveau, llvmpipe. Cái cần chỉ là
`libvulkan_panfrost.so`.

### Lần 2 — trên host, chroot arm64 qua qemu: **chạy được**

Đúng cách repo này vẫn làm (kernel cross-compile, rootfs qua chroot + qemu).

| Giai đoạn | Thời gian |
|---|---|
| Dựng chroot + 213 gói build-dep | ~10 phút (cache lại bằng `.mesa-build-ready`) |
| Compile 5988 target dưới qemu | **~80 phút** |
| RAM | 42–51GB trống suốt, không hề căng |

Host: AMD EPYC 4464P, 24 luồng, 62GB RAM.

### Hai lỗi gặp trên đường, đã sửa trong script

1. **Kiểm tra binfmt sai.** `[[ -s /proc/sys/fs/binfmt_misc/qemu-aarch64 ]]` luôn
   sai vì procfs báo mọi entry dài 0 byte. Phải kiểm tra nội dung.
2. **GNU tar chết dưới qemu.** `apt-get source` trong chroot fail:

   ```text
   tar: mesa-26.0.8/.ci-farms: Cannot mkdir: Function not implemented
   ```

   ENOSYS ở lần `mkdir` lồng đầu tiên, với qemu 8.2.2 (host 24.04) và guest
   glibc 2.43. Đã khoanh vùng: `mkdir` lồng chạy được, thư mục dấu chấm chạy
   được, `--no-xattrs --no-acls` không cứu — lỗi nằm trong đường extract của
   tar. **Cách xử lý:** tải trong chroot (chỉ nó có deb-src arm64), giải nén
   **trên host** bằng `dpkg-source` native. Cây source độc lập kiến trúc nên
   không mất gì.

   Lưu ý: `install-rootfs.sh` không dính lỗi này vì dpkg extract theo đường khác.

### Kết luận

80 phút mỗi lần rebase, cho một patch **không xoá được** fork GRD, là cái giá
không đáng trả cho việc ship. Vì vậy script/package/patch Mesa đã được loại khỏi repo.

---

## 2. Khoảng trống thật trong repo này

Repo này hiện **không có gì** thuộc đường encode. Năm khoảng trống, tất cả
đều đã kiểm chứng:

### 2.1 Không có driver RKVENC trong kernel

`config/patches/linux-7.1.8/series` có 7 patch, toàn bộ là HDMI/VOP/audio.
Không có patch nào cho media encode.

### 2.2 Không có VA driver

`output/packages.tsv` chỉ có `libva2` và `libva-drm2` — tức là *thư viện*
libva, **không có driver nào phía sau nó**. Image hiện tại không có cả
hardware decode lẫn encode qua VA-API.

### 2.3 Hai lỗ hổng kernel config

Đọc từ `sources/linux-7.1.8/.config` của bản build hiện tại:

```text
# CONFIG_DMABUF_HEAPS is not set        <- chặn cứng đường encode
# CONFIG_VIDEO_ROCKCHIP_VDEC is not set <- không có rkvdec
CONFIG_CMA_SIZE_MBYTES=512              <- đủ, giữ nguyên
```

- **`DMABUF_HEAPS`** — VA patch `0031` cấp surface input của encoder từ CMA
  dma-heap rồi export thành linear NV12 DMA-BUF. Đây chính là thứ đã sửa lỗi
  import của reference. Không có nó, `grd-rdp-render-context.c` fail ngay ở
  `g_assert (buffer_info->buffer_type == GRD_RDP_BUFFER_TYPE_DMA_BUF)`.
- **`VIDEO_ROCKCHIP_VDEC`** — decode H.264/HEVC cho RK3588, **đã có sẵn
  trong cây 7.1.8**, chỉ thiếu mỗi dòng config. Xem §2bis. Lưu ý tên symbol
  trong 7.1 là `VIDEO_ROCKCHIP_VDEC`, **không phải** `..._RKVDEC`.

### 2.4 User `gnome-remote-desktop` không thuộc group nào

```text
build/rootfs/etc/passwd: gnome-remote-desktop:x:978:978:...
build/rootfs/etc/group:  gnome-remote-desktop:x:978:
```

Đúng lỗi reference đã gặp: không mở được `/dev/dri/renderD128` và
`/dev/video*`. ACL `uaccess` của logind chỉ phủ session có seat; session
Remote Login là seatless.

### 2.5 Không có `LIBVA_DRIVER_NAME` cho system service

Node `/dev/dri/renderD128` là Panthor, nên libva sẽ đoán sai tên driver.
Reference đã ghi lại một cái bẫy: đặt trong `/usr/lib/environment.d` **không
có tác dụng** — systemd system service không đọc thư mục đó. Phải dùng
drop-in systemd.

---

## 2bis. Mainline đã cho không cái gì

Tháng 2/2026 Collabora đã merge hỗ trợ VDPU381 (RK3588) và VDPU383 (RK3576)
vào mainline — 17 patch gồm driver, DT node và dt-bindings. Kernel 7.1.8 mà
repo này ghim **đã có đủ**. Kiểm chứng thẳng trong cây nguồn, không qua bài báo:

```text
sources/linux-7.1.8/drivers/media/platform/rockchip/rkvdec/
  rkvdec-vdpu381-h264.c    rkvdec-vdpu381-hevc.c     <- RK3588
  rkvdec-vdpu383-h264.c    rkvdec-vdpu383-hevc.c     <- RK3576

rkvdec.c:1756   .compatible = "rockchip,rk3588-vdec"
                .data       = &vdpu381_variant

vdpu381_coded_fmts[] = { V4L2_PIX_FMT_HEVC_SLICE, V4L2_PIX_FMT_H264_SLICE }
capture: NV12, NV15, NV16, NV20        <- có cả 10-bit

rk3588-base.dtsi:1357  compatible = "rockchip,rk3588-vdec"   <- node đã có
rk3588-base.dtsi:1389  compatible = "rockchip,rk3588-vdec"   <- hai core
```

### Hệ quả trực tiếp

**Decode H.264/HEVC tốn 0 patch kernel.** Không cần driver, không cần DT node
— cả hai đã trong cây. Chỉ cần một dòng `CONFIG_VIDEO_ROCKCHIP_VDEC=m`.

Reference project mang 3 patch decode; với phạm vi H.264/HEVC thì:

| Patch reference | Số phận |
|---|---|
| `9007` vdpu381 VP9 backend (50 KB) | **Ngoài phạm vi** — `vdpu381_coded_fmts` in-tree chỉ có H264+HEVC; VP9 vẫn phải patch, nhưng ta không cần VP9 |
| `9009` hantro AV1 entropy context | **Ngoài phạm vi** — AV1 |
| `9008` cho phép format control trong request (2.3 KB) | **VẪN CẦN** — xem dưới |

### `9008` vẫn cần, và lý do rất cụ thể

Đây không phải patch codec, mà là patch cho *đường request của stateless
client*. Commit message của nó nói thẳng vấn đề:

> `rkvdec_s_ctrl()` từ chối mọi control như vậy với `-EBUSY` khi image format
> chưa được chốt, làm hỏng luồng request bình thường của GStreamer và
> **ngăn một VA-API bridge nộp SPS H.264 đầu tiên cùng lúc với frame**.

Tức là: không có `9008`, VA driver của ta không decode được frame đầu tiên.
Patch thu hẹp điều kiện thay vì bỏ hẳn — vẫn chặn khi capture format thật sự
đổi (NV20 cần bộ nhớ gấp 2.5 lần NV12, để decoder ghi tràn là hỏng).

### Dry-run: đã chạy thật trên cây 7.1.8 của repo này

```text
9008-...rkvdec-allow-stateless-request-format-controls   APPLIES CLEAN
9010-...rk3588-add-rkvenc-nodes                          APPLIES CLEAN
9011-...add-vepu580-h264-encoder                         APPLIES CLEAN
9012-...encode-hevc-on-the-vepu580                       FAILS (đúng như mong đợi)
```

`9012` "fails" chỉ vì thứ tự: nó sửa các file mà `9011` tạo ra
(`rkvenc/rkvenc-h264.c`, `rkvenc-hw.c`, `rkvenc.c`...). Áp sau `9011` là được.
**Không có xung đột thật nào.**

---

## 2ter. Phạm vi codec: mỗi client thật sự tiêu thụ gì

Yêu cầu là H.264 + HEVC, GRD encode và Chrome decode. Nhưng bốn ô của ma trận
đó **không ngang giá nhau**:

| | H.264 | HEVC |
|---|---|---|
| **Chrome decode** | mainline, 0 patch kernel | mainline, 0 patch kernel |
| **GRD encode** | `9010`+`9011`, VA `0023`–`0035` | `9012` + VA `0036` — **nhưng RDP không dùng** |

**Điểm cần nói rõ:** RDP Graphics Pipeline chỉ tiêu thụ H.264 — AVC420 và
AVC444. GNOME Remote Desktop **không có đường HEVC**. Cho nên HEVC *encode*
không có người dùng trong phạm vi GRD; nó chỉ đáng làm nếu sau này muốn
Chrome/WebRTC hoặc một client khác encode HEVC.

Đề nghị: **ship HEVC decode ngay** (miễn phí, mainline), **hoãn HEVC encode**
(`9012` + VA `0036`) sang sau khi H.264 end-to-end đã xanh. Bỏ hai thứ đó ra
khỏi đường tới hạn giảm được 55 KB patch kernel và 30 KB patch VA driver mà
không mất gì ở mục tiêu chính.

---

## 2quater. Cái KHÔNG cắt được: queue VA driver

Phản xạ tự nhiên là "chỉ cần H.264/HEVC thì bỏ các patch AV1/VP8/VP9 trong
queue 37 patch đi". **Đã kiểm tra, và làm vậy tốn công hơn là giữ.**

Patch `0018` (decode nhiều video cùng lúc, 111 KB) chạm cả `src/av1.c` và
`src/vp8.c`, nhắc tới av1/vp8 20 lần:

```text
0018 touches: av1.c buffer.c config.c context.c h264.c h265.c image.c
              picture.c request.c surface.c sync.c vp8.c vp9.c ...
```

Hai file đó do `0011` và `0015` *tạo ra*. Queue là diff tuần tự và đan vào
nhau — bỏ `0011`/`0015` là làm vỡ `0018` và mọi patch sau nó, tức phải tự
viết lại một phần queue.

**Kết luận: giữ nguyên cả 37 patch.** Chi phí runtime bằng 0 — patch `0013`
("advertise only the profiles this driver can decode") đã tự động không
quảng cáo codec nào không có thiết bị phía sau. Không bật VP9 (`9007`) và
không bật AV1 (`9009`) thì các đường đó đơn giản là không tìm thấy device và
im lặng.

Cái mà phạm vi H.264/HEVC thật sự cắt giảm **không phải là số patch, mà là
bề mặt phải test**: không phải validate AV1, VP8, VP9.

---

## 3. Workstreams

### W1 — Kernel: driver RKVENC + config

Decode không cần patch nào (§2bis). Mang 3 patch từ
`/root/orangepi5b/patches/linux/7.1/`, đánh số lại theo quy ước repo này:

| Nguồn | Đích | Nội dung |
|---|---|---|
| `9008-...allow-stateless-request-format-controls.patch` | `0008-media-rockchip-rkvdec-allow-stateless-request-format-controls.patch` | 2.3 KB — **decode**, VA bridge cần |
| `9010-...add-rkvenc-nodes.patch` | `0009-arm64-dts-rockchip-rk3588-add-rkvenc-nodes.patch` | DT node encoder, 4.5 KB |
| `9011-...add-vepu580-h264-encoder.patch` | `0010-media-rockchip-add-vepu580-h264-encoder.patch` | driver, 136 KB |
| `9012-...encode-hevc-on-the-vepu580.patch` | **hoãn** | HEVC encode, 55 KB; RDP không dùng (§2ter) |
| `9007` VP9, `9009` AV1 | **không mang** | ngoài phạm vi H.264/HEVC |

Thứ tự trong `series` phải giữ `9011` trước `9012` nếu sau này thêm HEVC
encode — `9012` sửa file do `9011` tạo.

**Rủi ro rebase thấp, đã kiểm chứng ba điều:**

1. Cùng cây nguồn. `scripts/fetch-sources.sh` ghim
   `LINUX_REV=25c76bea853d0db65b51fb4697a47cbfd9e35e76` và seed thẳng từ
   `/root/orangepi5b/upstream/linux` — đúng cây mà patch được viết trên đó.
2. Không đụng file nào. Patch hiện có chạm `drm/`, `phy/`, `sound/`,
   `rk3588s-orangepi-5.dtsi`, `rk3588s-orangepi-5b.dts`. RKVENC chạm
   `rk3588-base.dtsi` (hunk `@@ -1353,6 +1353,64 @@`) cộng thư mục mới
   `drivers/media/platform/rockchip/rkvenc/` và Kconfig/Makefile của
   `rockchip/`. Giao rỗng.
3. **Đã dry-run thật** trên `sources/linux-7.1.8`: `9008`, `9010`, `9011` đều
   `APPLIES CLEAN`.

**Sửa `scripts/build-boot-image.sh`:**

```bash
"$K/scripts/config" --file "$K/.config" \
  --enable  DMABUF_HEAPS \
  --enable  DMABUF_HEAPS_SYSTEM \
  --enable  DMABUF_HEAPS_CMA \
  --module  VIDEO_ROCKCHIP_VDEC \
  --module  VIDEO_ROCKCHIP_RKVENC
```

Đặt sau khối `disable_kernel_symbols`/`disable_enabled_group`, cùng chỗ với
các dòng "Re-assert Orange Pi 5B essentials" đã có — để một entry slim thêm
vào sau này không âm thầm tắt mất.

Rồi thêm vào danh sách verify (đang ở khoảng dòng 177):

```bash
for symbol in DRM_PANTHOR VIDEO_HANTRO VIDEO_ROCKCHIP_RGA \
              VIDEO_ROCKCHIP_VDEC VIDEO_ROCKCHIP_RKVENC; do ...
for symbol in DMABUF_HEAPS DMABUF_HEAPS_CMA; do   # phải là =y
```

**Gate W1** — trên board:

```bash
for v in /dev/video*; do
  echo "== $v $(v4l2-ctl -d $v --info | grep -m1 'Card type')"
  v4l2-ctl -d "$v" --list-formats-out; v4l2-ctl -d "$v" --list-formats
done
ls /dev/dma_heap/
```

Phải thấy **cả hai**:

- node **decoder**: OUTPUT `H264_SLICE` + `HEVC_SLICE`, CAPTURE `NV12`/`NV15`
- node **encoder**: OUTPUT `NV12`, CAPTURE `H264`

và `/dev/dma_heap/` có `linux,cma`. Không bao giờ ghim số node `/dev/videoN`
vào code — thứ tự probe đổi giữa các lần boot.

Rồi mượn `tools/rkvenc-v4l2-smoke` của reference (shell, chạy trên board, tự
tìm encoder theo card name):

```bash
tools/rkvenc-v4l2-smoke -e
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=60 -frames:v 300 \
  -pix_fmt nv12 -c:v h264_v4l2m2m -g 60 -b:v 12M -f h264 /tmp/rkvenc.h264
ffmpeg -i /tmp/rkvenc.h264 -f null -      # phải decode sạch
```

### W2 — VA driver: libva-v4l2-request cho libva 2.23

Nguồn: `github.com/pencilbsp/libva-v4l2-request`, rev
`ed4bc90a8c979c83790013297850960c73a3d2f1`, nhánh `rk3588-vp9` — đã có sẵn
tại `/root/orangepi5b/upstream/libva-v4l2-request`.

Patch queue **37 patch** tại `/root/orangepi5b/patches/libva-v4l2-request/ed4bc90/`:

- `0001`–`0022` — nền decode (H.264, HEVC, VP9, AV1, VP8, multi-stream, 10-bit)
- `0023`–`0037` — **encode**: advertise, context, encode H.264, map tham số,
  packed headers, export từ dma-heap, HEVC

Giữ **cả 37** — lý do đầy đủ ở §2quater: bỏ patch AV1/VP8 làm vỡ `0018` và
mọi patch sau nó, tốn công hơn là giữ. Runtime không tốn gì vì `0013` chỉ
advertise codec nào có thiết bị phía sau.

Với phạm vi này, **Chrome decode H.264/HEVC là mục tiêu ngang hàng với GRD
encode, không phải lợi ích kèm theo** — và nó dùng chung đúng một driver
`v4l2_request_drv_video.so`. Chrome desktop không có đường V4L2 decode
(`BUILDFLAG(IS_CHROMEOS)`), nên VA-API là lối duy nhất cho cả hai chiều.

**Cách build.** Repo này cross-compile kernel từ x86 nhưng dựng rootfs trong
chroot arm64 qua `qemu-aarch64-static`. VA driver là shared object arm64 phải
resolve đúng libva mà image ship, nên dựng nó trong chroot arm64 raise từ
**chính tarball** `cache/sources/ubuntu-base-26.04.1-base-arm64.tar.gz`.

Port `scripts/build-va-driver-package.sh` của reference, sửa 3 chỗ:

1. Bỏ `jq`/`sources.lock.json`; đọc thẳng đường dẫn tarball 26.04.
2. `PACKAGE_VERSION` mới, `Depends: libva2 (>= 2.23)`.
3. Giữ nguyên — và giữ *đúng vì lý do gì* — bước kiểm tra symbol:

```bash
libva_version=$(pkg-config --modversion libva)    # 2.23.0
expected_symbol=__vaDriverInit_1_23
nm -D --defined-only .../v4l2_request_drv_video.so | grep -F "$expected_symbol"
```

Symbol này sinh từ header libva lúc build, nên build trong chroot 26.04 là tự
đúng. Kiểm tra ở đây để **fail lúc build thay vì lúc chạy**: một VA driver
sai version không được nạp, và client chỉ báo mỗi "vaInitialize failed".

**Rủi ro thật của W2:** 37 patch này viết trên libva 2.20 headers. libva 2.23
có thể đã đổi struct hoặc thêm field. Đây là chỗ *duy nhất* trong kế hoạch có
khả năng phải sửa code chứ không chỉ port. Làm sớm để lộ ra sớm.

**Gate W2** — trên board:

```bash
LIBVA_DRIVER_NAME=v4l2_request vainfo --display drm --device /dev/dri/renderD128
```

Phải thấy, và chỉ thấy những profile đã verify:

```text
VAProfileH264ConstrainedBaseline : VAEntrypointEncSlice
VAProfileH264Main                : VAEntrypointEncSlice
VAProfileH264High                : VAEntrypointEncSlice
```

và phía decode:

```text
VAProfileH264High : VAEntrypointVLD
VAProfileHEVCMain : VAEntrypointVLD
```

```bash
# encode
LIBVA_DRIVER_NAME=v4l2_request ffmpeg -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc2=size=1920x1080:rate=60 -vf 'format=nv12,hwupload' \
  -frames:v 300 -c:v h264_vaapi -g 60 -b:v 12M /tmp/vaapi-rkvenc.mp4

# decode, cả hai codec
LIBVA_DRIVER_NAME=v4l2_request ffmpeg -hwaccel vaapi \
  -hwaccel_device /dev/dri/renderD128 -i sample-h264.mp4 -f null -
LIBVA_DRIVER_NAME=v4l2_request ffmpeg -hwaccel vaapi \
  -hwaccel_device /dev/dri/renderD128 -i sample-hevc.mp4 -f null -
```

Cộng `tools/rkvenc-vaapi-smoke` của reference.

### W3 — Tích hợp image

Sửa `scripts/install-rootfs.sh`:

```bash
# Cấp quyền thiết bị cho GRD. Package tạo user lúc cài trong chroot,
# nên gán group ngay lúc build, không cần service runtime như reference.
chroot "$R" usermod -aG video,render gnome-remote-desktop

# Chọn VA driver. environment.d KHÔNG được system service đọc.
install -D -m 0644 /dev/stdin \
  "$R/usr/lib/systemd/system/gnome-remote-desktop.service.d/10-vaapi.conf" <<'EOF'
[Service]
Environment=LIBVA_DRIVER_NAME=v4l2_request
EOF
```

Cần **cả hai phía**, vì GRD chạy hai chế độ:

- system: `gnome-remote-desktop.service` — Remote Login qua GDM
- user: `gnome-remote-desktop.service` + `gnome-remote-desktop-handover.service`
  trong `/usr/lib/systemd/user/` — Desktop Sharing

Cho phía user, dùng drop-in trong `/usr/lib/systemd/user/...service.d/` hoặc
`/etc/environment.d/`. Cài `.deb` VA driver trong chroot cùng chỗ Chrome đang
được cài (`install-rootfs.sh` dòng ~55).

Cân nhắc: nếu VA driver là driver duy nhất và ổn định, đặt
`LIBVA_DRIVER_NAME` toàn hệ thống sẽ đơn giản hơn — nhưng nó cũng ép Chrome
đi cùng đường, nên chỉ làm sau khi W5 xanh.

### W4 — GNOME Remote Desktop: xác minh trước, vá sau

Đây là chỗ khác kế hoạch cũ nhiều nhất. **Không build GRD từ source trừ khi
probe chứng minh là phải.**

GRD 50 chỉ tạo VAAPI encode session khi qua **cả bốn** cửa, đọc từ
`grd-rdp-render-context.c`:

```c
g_assert (buffer_info->buffer_type == GRD_RDP_BUFFER_TYPE_DMA_BUF);
if (!buffer_info->has_vk_image) return;
if (!buffer_info->has_syncobjs) return;
if (buffer_info->drm_format_modifier == DRM_FORMAT_MOD_INVALID) return;
```

Trạng thái từng cửa trên repo này:

| Cửa | Trên 26.04 | Ghi chú |
|---|---|---|
| `buffer_type == DMA_BUF` | Có, **nếu** W1 bật `DMABUF_HEAPS` và W2 có patch 0031 | |
| `has_vk_image` | Phụ thuộc PanVK chọn được physical device | Cửa của patch 0004 |
| `has_syncobjs` | **Mở** — PipeWire 1.6.2 có explicit sync | *Đây là thứ 24.04 không bao giờ qua được* |
| modifier hợp lệ | **Chưa biết** — phải đo PanVK trên Mesa 26.0.8 | |

Cửa thứ ba là lý do reference từng kết luận "phải chờ upstream". **Trên
26.04 nó mở sẵn.** Còn lại đúng một ẩn số: modifier.

**Phép đo đã chạy (§1bis): cần patch GRD fallback và patch HOST_CACHED.** W4 là rebuild `gnome-remote-desktop` 50.2 từ source package Ubuntu (`50.2-0ubuntu0.1`) với patch GRD, đóng `.deb`, ghim version.

Cụ thể:

```bash
apt-get source gnome-remote-desktop      # trong chroot arm64 26.04
# đặt 0008 (+0007 nếu muốn chẩn đoán) vào debian/patches/
# thêm tên vào debian/patches/series, bump changelog -> 50.2-0orangepi5b1
dpkg-buildpackage -b -uc -us
```

Giữ `debian/` của Ubuntu nguyên vẹn — đó là điểm khác biệt lớn so với
reference (build từ tarball upstream, phải tự lo toàn bộ packaging).

Ghi rõ trong `patches/gnome-remote-desktop/README`: đây là fork phải nuôi, và
**điều kiện để bỏ nó** là Mesa panvk trả lời truy vấn modifier bản v2.

Theo dõi log — và biết đọc log:

```bash
sudo journalctl -u gnome-remote-desktop -f      # Remote Login
journalctl --user -u gnome-remote-desktop -f    # Desktop Sharing
```

Reference để lại một cái bẫy đáng nhớ: dòng
`[HWAccel.Vulkan] Initialization of Vulkan was successful` là init **instance**,
không phải tín hiệu cần theo dõi. Việc chọn **physical device** diễn ra sau,
theo từng phiên. Dòng cần tìm là:

```text
[HWAccel.VAAPI] Using VAEntrypoint 6 for profile VAProfileH264High
[HWAccel.VAAPI] Successfully initialized VAAPI 1.23 with vendor: v4l2-request
[HWAccel.VAAPI] Created VAAPI encode session for surface with size ...
```

Cách chẩn đoán nhanh nếu phiên không lên: **đếm tiến trình**. Đường handover
chạy đúng có **hai** `gnome-remote-desktop-daemon` (system + session); một
cái là hỏng.

**Client test.** FreeRDP 3.31 của 26.04 **vẫn** build không có H.264 —
kiểm chứng từ chuỗi build config nhúng trong `libfreerdp3.so.3` của
`build/rootfs`:

```text
WITH_GFX_H264=OFF   WITH_OPENH264=OFF   WITH_VAAPI=OFF
WITH_VAAPI_H264_ENCODING=OFF
```

Nên client chạy ngay trên board luôn báo `AVC444: false, AVC420: false` —
**không phải lỗi encoder**. Cần client có AVC thật (`sdl-freerdp` từ Homebrew trên macOS
đã dùng được ở reference). Dòng `CapsAdvertise ... Client cap flags` trong
log GRD nói thẳng phía nào từ chối trước.

**Gate W4:**

- log có `Created VAAPI encode session`
- `tools/vpu-activity 5` cho thấy IRQ RKVENC tăng theo frame rate khi có motion
- CPU của `gnome-remote-desktop` giảm rõ so với baseline software
- phiên sống ≥ 30 phút, qua được reconnect và đổi độ phân giải

### W5 — Chrome

**Decode là mục tiêu chính, không tuỳ chọn.** Chạy được ngay sau W2, không
phải chờ W4 — hai đường độc lập. Kiểm tra ở `chrome://gpu`, mục *Video
Acceleration Information*, phải thấy H.264 và HEVC decode.

Encode thì vẫn là tuỳ chọn, sau khi W4 xanh.

```bash
LIBVA_DRIVER_NAME=v4l2_request google-chrome-stable \
  --enable-features=AcceleratedVideoEncoder --ignore-gpu-blocklist \
  --hardware-video-device-path=/dev/dri/renderD128
```

`AcceleratedVideoEncoder` mặc định tắt trên Linux. Đường V4L2 encode của
Chrome vẫn khoá sau `BUILDFLAG(IS_CHROMEOS)`, nên VA-API là lối duy nhất.
Giữ sau launch flag cho tới khi validate nhiều lần.

---

## 4. Thứ tự thực hiện

Sắp theo *rủi ro lộ sớm*, không theo thứ tự chuỗi:

Mainline decode chia dự án thành **hai nhánh gần như độc lập**. Nhánh decode
ngắn và gần như chắc thắng; nhánh encode dài và còn ẩn số. Không để nhánh dài
giữ nhánh ngắn làm con tin.

**Mốc sớm — Chrome decode H.264/HEVC (nhanh nhất tới giá trị thật):**

1. `CONFIG_VIDEO_ROCKCHIP_VDEC=m` + `DMABUF_HEAPS` + patch `9008`, build kernel.
   Kiểm tra node decoder advertise `H264_SLICE`/`HEVC_SLICE`.
2. **W2 dry-run:** apply 37 patch lên `ed4bc90`, build trong chroot 26.04.
   Chỗ dễ vỡ nhất vì bước nhảy libva 2.20 → 2.23. Lộ sớm.
3. Đóng `.deb`, `vainfo` báo `VAEntrypointVLD` cho H264+HEVC, Chrome
   `chrome://gpu` xanh. **Tới đây đã có một image đáng ship.**

**Nhánh encode, chạy song song từ đầu:**

4. ~~Probe modifier PanVK.~~ **XONG 2026-09-08** — cần `0004` + `0008` (§1bis).
5. **W1 phần encoder:** patch `9010`+`9011`, `rkvenc-v4l2-smoke -e`,
   `h264_v4l2m2m`.
6. **W2 phần encode:** `VAEntrypointEncSlice`, `h264_vaapi`.
7. **W4a — rebuild GRD** từ source package Ubuntu với patch GRD.
8. **W3:** groups + drop-in + cài `.deb` GRD vào image.
9. **W4b:** chạy GRD, đo IRQ và CPU.
10. HEVC encode (`9012` + VA `0036`) chỉ khi H.264 đã xanh và có người dùng thật.

---

## 5. Rủi ro

| Rủi ro | Mức | Xử lý |
|---|---|---|
| 37 patch VA không build trên libva 2.23 | **Cao** | Làm sớm (bước 2). Sửa từng patch; queue có README 60 KB giải thích ý đồ từng cái |
| ~~PanVK trả rỗng ở truy vấn v2~~ | — | **Đã xác nhận là có.** Không còn là rủi ro, đã thành việc phải làm: patch `0004` |
| ~~PanVK thiếu `HOST_CACHED`~~ | — | **Đã xác nhận.** Patch `0008` |
| Ubuntu cập nhật GRD trong 26.04, fork lệch | Thấp | Ba patch nhỏ; ghim version, theo dõi `-updates` và thử bỏ từng workaround khi upstream sửa |
| **Ubuntu cập nhật Mesa, fork lệch** | **Trung bình** | Mesa ra bản vá bảo mật thường xuyên hơn GRD. Phải rebase patch panvk mỗi lần, và mỗi lần là một build Mesa đầy đủ. Giảm rủi ro bằng cách gửi patch lên thượng nguồn sớm |
| Mesa tự build khác Mesa của Ubuntu ở chỗ khác | Thấp | Dùng `dpkg-buildpackage` trên source package Ubuntu, giữ nguyên `debian/rules` — không tự chọn cờ meson cho bản ship. Và chỉ ship `mesa-vulkan-drivers`, các package Mesa khác vẫn là bản Ubuntu |
| Đường handover của GDM 50 khác GDM 46 | Trung bình | 26.04 đã ship `gnome-remote-desktop-handover.service` — có vẻ native. Verify, đừng giả định |
| Patch kernel không apply | Thấp | Cùng base rev; không giao file. `patch --dry-run` để chắc |
| Encode làm hỏng decode | Thấp | Hai driver kernel riêng (rkvdec / rkvenc), hai thiết bị riêng. Nhưng dùng chung một VA driver — test đồng thời như reference đã làm |
| CMA cạn khi vừa encode vừa decode | Thấp | 512 MB đã cấu hình sẵn; theo dõi ở test 30 phút |

---

## 6. Không làm

- Không gọi thẳng `libmpp` từ GRD. MPP chỉ dùng làm reference thanh ghi và
  baseline hiệu năng.
- Không coi HEVC encode là thành công. RDP graphics pipeline cần H.264
  AVC420/AVC444. Patch `9012` để sau.
- Không advertise qua VA-API bất kỳ profile nào chưa verify.
- Không ghim số node `/dev/videoN` vào code hay script.
- Không đưa Chrome vào đường V4L2 ChromeOS-only.
- Không commit password. `target.json` đã có credential — không in ra log.

---

## 7. Definition of done

Image dựng từ repo này, boot trên Orange Pi 5B, và:

1. `vainfo` báo `VAEntrypointEncSlice` cho đúng những profile đã verify.
2. `h264_v4l2m2m` encode ra bitstream decode sạch trên máy khác.
3. `h264_vaapi` encode qua driver của repo.
4. GRD phục vụ phiên RDP bằng hardware H.264; log có
   `Created VAAPI encode session`; IRQ RKVENC tăng theo frame rate.
5. CPU của `gnome-remote-desktop` giảm rõ khi có full-screen motion.
6. Sống qua reconnect, đổi độ phân giải, và 30 phút motion liên tục.
7. Fallback sạch về software encode khi encoder không dùng được — không
   crash desktop.
8. Chrome decode H.264 **và** HEVC qua VA-API hoạt động, thấy được ở
   `chrome://gpu`; decode và encode chạy đồng thời không xung đột.
9. `README.md` ghi lại đúng board, kernel, package, lệnh và giới hạn đã test.
