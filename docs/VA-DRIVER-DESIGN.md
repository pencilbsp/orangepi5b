# VA-API driver cho RK3588S — thiết kế, phạm vi H.264 + HEVC

Tài liệu này rút bài học từ queue 37 patch của project tham chiếu
(`/root/orangepi5b/patches/libva-v4l2-request/ed4bc90/`) để **viết mới**, không
chép. Mỗi mục dưới đây là một cái hố đã có người dẫm; chi phí học lại bằng cách
tự dẫm là hàng tuần.

Nguồn: `README.md` (1088 dòng) của queue đó, cộng nội dung từng patch.

## Phạm vi

Chỉ **H.264 và HEVC decode**, chỉ **rkvdec (VDPU381)**, chỉ **8-bit NV12**.

Không làm: AV1, VP8, VP9, MPEG-2, 10-bit, multi-core. Cắt được vì:
- AV1 nằm trên block Hantro khác, node khác — logic chọn thiết bị đa codec biến mất
- 10-bit HEVC bị chặn ở **biểu diễn format**, không phải silicon: rkvdec chỉ ra
  NV15 (layout 10-bit nén chặt của Rockchip) và **libva không có FOURCC cho NV15**
  (`P010`, `P016`, `I010` có; NV15 không). rkvdec không có postprocessor để
  chuyển. Nên không có gì để đưa cho client
- VP9 chỉ NV12; MPEG-2 trong base là stub 19 dòng

## Nguyên tắc bao trùm

**Chỉ advertise thứ thật sự decode được.** Quảng cáo một profile không chạy được
còn tệ hơn im lặng: client cam kết đi đường phần cứng, lấy được config và
context, rồi mới vỡ giữa chừng.

## Kiến trúc — bốn điều phải đúng từ đầu

Bốn nguyên nhân độc lập, phải sửa hết mới thấy được cái tiếp theo (patch `0018`,
111 KB — đây là phần đắt nhất của cả queue).

### 1. Mỗi context tự mở node của mình — ĐÃ LÀM 2026-09-08

`v4l2-mem2mem` khoá một phiên decode theo **file handle**: `REQBUFS`, hàng đợi
và mọi job thuộc về handle nào đã xin chúng. Nếu các context dùng chung handle
dò tìm thiết bị, stream thứ hai sẽ lập trình lại hàng đợi của stream thứ nhất
ngay dưới chân nó — biểu hiện là `CAPTURE pool exhausted at 0 buffers`.

Hệ quả thiết kế: mọi thứ mô tả một ca decode đang chạy phải nằm **trong
context**, không phải toàn cục — thiết bị, handle, CAPTURE format, độ sâu pool.
Unbind surface cũng phải theo context: unbind mọi surface trên display sẽ rút
buffer khỏi một ca decode đang chạy ở chỗ khác.

### 2. Phải có khoá — ĐÃ LÀM 2026-09-08

libva **không** tuần tự hoá lời gọi driver, và client phát hai video sẽ decode
mỗi cái trên một thread riêng (FFmpeg đặt tên `dec0:0:av1`; Chrome cho mỗi video
một thread). Object heap và danh sách thiết bị dùng chung mà không đồng bộ thì
hỏng.

Thiết kế: một lớp vỏ quanh các entry point VA, mỗi cái lấy khoá driver-wide,
chạy `_impl`, rồi nhả. **`SyncSurface` phải nhả khoá trong lúc chờ phần cứng** —
đó là gần như toàn bộ thời gian thực của một ca decode. Như vậy chỉ tuần tự hoá
phần sổ sách, không tuần tự hoá việc decode.

### 3. Chọn thiết bị bằng xếp hạng, không phải "khớp đầu tiên"

**Hai thiết bị trên SoC này advertise `H264_SLICE`:** rkvdec, và block Hantro
VDPU2 cũ mà cùng driver `hantro-vpu` đăng ký dưới tên
`rockchip,rk3568-vpu-dec`. **Chỉ rkvdec decode đúng.** VDPU2 nhận stream, trả về
frame, và mọi frame đều là rác (PSNR ~5 dB).

Chọn theo thứ tự enumeration đã chạy đúng cho tới khi số hiệu node đổi và VDPU2
enumerate trước rkvdec — H.264 lặng lẽ thành rác mà **không dòng code nào thay
đổi**. Xếp hạng: thiết bị do biến môi trường chỉ định → rkvdec → còn lại.

Đây là lý do không bao giờ được ghim `/dev/videoN` vào code.

### 4. OUTPUT pixelformat chọn backend phần cứng

Trên rkvdec, **pixelformat của hàng đợi OUTPUT là thứ chọn codec backend**. Nếu
context được tạo sau một lần dò đã lập trình queue cho codec khác, kernel fail
mọi CAPTURE buffer với `unexpected bitstream resolution 1x1`.

Thiết kế: theo dõi pixelformat OUTPUT đang lập trình, và lập trình lại khi codec
yêu cầu khác đi.

## H.264 — những chỗ sai không nhìn ra được

### DPB phải đóng gói liên tiếp từ slot 0

**Bài học đắt nhất trong toàn bộ queue.** Triệu chứng: 27/60 frame hỏng, tất
định; GOP đầu đúng, hỏng bắt đầu từ frame 34 sau IDR thứ hai.

So sánh từng frame cho thấy control stream **giống hệt về mặt ngữ nghĩa** với
GStreamer: 0/60 frame khác nhau ở SPS, PPS, decode params, nội dung DPB, cờ
NAL/IDR hay slice params. Bitstream cũng đúng — Annex-B, start code ở offset 0,
một NAL mỗi access unit, dãy kích thước payload trùng khít.

Thứ duy nhất phép so sánh cố tình bỏ qua là **vị trí slot**, vì nó trông như chi
tiết cài đặt. Nó không phải:

```
GStreamer nộp:  {0:0, 1:1, 2:2}
bridge nộp:     {1:1, 3:2, 4:3}      ← khác ở 55/60 frame
```

**rkvdec lập trình một thanh ghi reference phần cứng cho mỗi chỉ số slot.** Mảng
có lỗ mô tả một tập reference khác, và mọi ảnh dự đoán từ reference nằm sau cái
lỗ đầu tiên sẽ decode dựa trên thanh ghi chưa ghi. Lỗ chỉ xuất hiện sau khi một
surface được giải phóng rồi cấp lại — đúng hình dạng của lỗi.

**Quy tắc: ghi các entry hợp lệ liên tiếp từ slot 0.** Áp dụng cho cả HEVC.

### Bốn chỗ VA-API không cho đủ thông tin

- **`nal_ref_idc` và cờ IDR**: VA chỉ phơi ra một boolean "là reference picture",
  không đủ để điền `V4L2_CID_STATELESS_H264_DECODE_PARAMS`. Phải suy ra từ **NAL
  VCL đầu tiên** của bitstream.
- **Scaling list 8x8**: VA list 1 là **Inter Y**, ánh xạ sang **slot 1** của
  V4L2, không phải slot 3 kiểu cũ.
- **`picnum`**: suy ra từ `FrameNumWrap`.
- **DPB cũ**: phải xoá entry đã rời VA DPB, và **bảo vệ entry output hiện tại**
  khỏi bị đuổi. Không làm thì bảng 16 slot dùng nhầm reference ở cuối GOP.

### Một cái bẫy: đừng "sửa" POC

FFmpeg đưa `TopFieldOrderCnt = 65536` cho IDR mở đầu — hợp lệ, vì client VA chỉ
cần POC nhất quán nội bộ. Neo POC lại ở mỗi IDR cho khớp reference (bias đo được
bằng 0) **không đổi kết quả**: vẫn 27/60. Đây là red herring; đừng mất thời gian.

## HEVC — nhỏ hơn H.264 nhiều

Kernel đã đúng: `v4l2slh265dec` của GStreamer decode bit-identical với software.
Chỉ thiếu ánh xạ userspace.

**Một client đúng nộp đúng bốn control mỗi request:** SPS, PPS, scaling matrix,
decode params. `hevc_decode_mode` là Frame-Based và `hevc_start_code` là Annex B,
nên `SLICE_PARAMS` và `ENTRY_POINT_OFFSETS` **không cần** — kernel cũng không
advertise.

Trong `v4l2_ctrl_hevc_decode_params`, backend VDPU381 chỉ đọc
`pic_order_cnt_val`, `flags`, `num_active_dpb_entries` và `dpb`; mỗi entry DPB
chỉ đọc `pic_order_cnt_val` (timestamp mới là thứ định danh buffer). Mảng chỉ số
`poc_st_curr_before/after/lt` **không được phần cứng này đọc**, nhưng vẫn nên
điền đúng.

**Ràng buộc cần cẩn thận:** rkvdec nhận thêm hai control mở rộng của Rockchip,
`V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS` và `..._LT_RPS`, mang reference picture
set khai trong SPS. VA-API **không phơi ra chúng**. Kernel không fail khi thiếu
— nó cảnh báo rồi "output wrong frames".

Có quan trọng hay không tuỳ stream: x265 ghi short-term RPS trong slice header,
nên clip mặc định lẫn clip `keyint=30:bframes=2` đều báo
`num_short_term_ref_pic_sets = 0` trong SPS và decode đúng mà không cần control
mở rộng. **Làm ánh xạ VA→V4L2 trước; parser `st_ref_pic_set()` từ SPS RBSP là
bước hai, chỉ kích hoạt khi `num_short_term_ref_pic_sets != 0`.**

## STREAMON phải đợi sequence header — phát hiện khi viết, 2026-09-08

**Không có trong tài liệu của reference.** Tự vấp phải và tra ra từ kernel source.

Triệu chứng: H.264 tạo context bình thường, HEVC fail.

```text
Unable to enable stream on type 10: Invalid argument
context: STREAMON(OUTPUT) failed for 0x35363253    ← 'S265'
```

Nguyên nhân, đọc thẳng từ `rkvdec-vdpu381-hevc.c`:

```c
static int rkvdec_hevc_start(struct rkvdec_ctx *ctx)
{
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_HEVC_SPS);
	if (!ctrl)
		return -EINVAL;
	ret = rkvdec_hevc_validate_sps(ctx, ctrl->p_new.p_hevc_sps);
```

`rkvdec_start_streaming()` gọi `desc->ops->start` lúc STREAMON, và
`rkvdec_hevc_validate_sps()` từ chối `chroma_format_idc != 1`. SPS chưa đặt thì
toàn 0, tức `chroma_format_idc == 0` → EINVAL. H.264 không có hook `start` nên
không lộ ra.

**Và control gắn vào request không cứu được:** nó chỉ được áp dụng khi request
được queue, mà queue thì phải sau STREAMON. Vòng lặp chết.

**Thiết kế đúng:** hoãn STREAMON tới `EndPicture` của frame đầu tiên, đặt SPS
lên **thiết bị** (`request_fd = -1`) từ picture parameters của frame đó, rồi mới
bật stream. Per-frame vẫn lặp lại SPS bên trong mỗi request như bình thường.

Đây cũng là cách GStreamer làm, và nó đúng về mặt khái niệm: không thể biết
capture format trước khi có sequence header.

## Pool không được là hằng số — phát hiện khi làm 8K, 2026-09-08

Base cấp pool cố định 64 buffer cho cả CAPTURE lẫn OUTPUT. Ở 1080p thì vô hại.
Ở 8K, một frame NV12 khoảng **50 MiB**, nên 64 buffer đòi **3.2 GiB** từ vùng
CMA **512 MiB** — REQBUFS thất bại và stream không bao giờ khởi động.

Cho pool co theo kích thước frame: ngân sách cố định (192 MiB) chia cho kích
thước frame, kẹp trong [20, 64]. Cận dưới 20 vì H.264 cho phép 16 reference
cộng frame hiện tại cộng dư cho hàng đợi hiển thị.

Và đừng ghim độ phân giải tối đa: hỏi `VIDIOC_ENUM_FRAMESIZES`. Base báo cứng
3840x2160, biến giới hạn phần cứng thành giới hạn driver — 8K decode được.

## Buffer — hai lỗi chỉ lộ ra với nội dung thật

### Bitstream buffer 1 MiB là không đủ

Đo trên file 4K60 thật, 634 giây, 17 Mbit/s: **8 trên 38074 frame không vừa**
buffer 1 MiB (0.021%, đỉnh 1.30 MiB). Mỗi cái bị từ chối với
`VA_STATUS_ERROR_NOT_ENOUGH_BUFFER` — **không phải drop frame**:
`vaRenderPicture` fail, Chrome báo `PIPELINE_ERROR_DECODE`, decoder bị phá và
dựng lại, playback đứng hàng chục giây.

Clip ngắn không bao giờ lộ ra lỗi này.

**Quy tắc: 1 MiB tới 1080p, 4 MiB trên 1080p** (gấp ~3 lần đỉnh đo được, cùng
quy tắc Chromium dùng cho V4L2 bitstream buffer của nó). rkvdec cấp đúng
`sizeimage` được xin, đã kiểm từ 1 tới 16 MiB.

### Destroy context phải xoá độ sâu pool đã cache

Nếu `DestroyContext` giải phóng hàng đợi nhưng để lại độ sâu pool đã cache, hàm
khởi tạo đọc capture count khác 0 là "đã cấu hình rồi" và return sớm. Sau một
chu kỳ destroy/create, hàng đợi không được lập trình lại và context tạo ở độ
phân giải mới vẫn giữ buffer kích thước cũ — đúng đường mà **adaptive streaming
đổi chất lượng** đi qua.

### Linh tinh nhưng bắt buộc

- Dùng **số buffer thật kernel trả về**, không phải số mình xin
- Từ chối CAPTURE buffer có `V4L2_BUF_FLAG_ERROR`
- `vaCreateImage` **không nêu tên surface nào** — đừng lấy layout từ "hàng đợi
  CAPTURE dùng gần nhất". Với hai stream đang chạy, hàng đợi đó thuộc về decoder
  nào hỏi sau cùng, và `vaGetImage` sẽ memcpy quá cuối mapping

## Chrome — hình dạng khác FFmpeg

Chrome **cấp phát mọi frame qua driver**: gọi `vaCreateSurfaces` rồi ngay lập
tức `vaExportSurfaceHandle`, **trước khi decode một ảnh nào**. Nên:

- **Export phải chấp nhận surface chưa bind**
- Chrome xin `VA_EXPORT_SURFACE_SEPARATE_LAYERS` và đọc **một plane mỗi layer**;
  luôn trả descriptor gộp là sai

Chẩn đoán: lỗi báo về `MediaLog`, **không hiện trên stderr kể cả `--vmodule=*=4`**.
Xem `chrome://media-internals`; dấu hiệu là "video decoder fallback after
initial decode error".

Chrome phải chạy trong phiên Wayland đang chạy (`--ozone-platform=wayland`).
Headless vô dụng: GPU process không chạy VA probe trước sandbox.

## Chrome — đã làm, 2026-09-08

Ba thay đổi, đúng như phần Chrome ở trên mô tả.

### Export chấp nhận surface chưa bind

Chrome cấp phát mọi frame qua driver: `vaCreateSurfaces` rồi `vaExportSurfaceHandle`
**ngay lập tức**, trước khi decode ảnh nào, và decode vào chính surface đó sau.

Base từ chối surface chưa bind — có lý do được ghi rõ: mpv probe một surface rồi
không bao giờ decode vào, nên dmabuf export ra trỏ vào bộ nhớ chưa khởi tạo và
màn hình xanh lè. Nhưng với Chrome thì surface **sẽ** được decode vào.

Cách xử lý: **bind lazily ngay tại export** thay vì từ chối. Như vậy dmabuf trỏ
đúng buffer mà decode sẽ ghi vào. Client nào export rồi không decode thì thấy nội
dung chưa khởi tạo — đó là client đòi bộ nhớ trước khi có gì trong đó.

### `VA_EXPORT_SURFACE_SEPARATE_LAYERS`

Base luôn trả descriptor gộp (`num_layers = 1`). Chrome xin separate layers và
đọc **một plane mỗi layer**, nên nhận được số plane không như nó chờ.

Giờ tôn trọng cờ: NV12 tách thành hai layer, luma `DRM_FORMAT_R8` và chroma
`DRM_FORMAT_GR88` — mỗi plane là một ảnh riêng.

### Khoá driver-wide

libva **không** tuần tự hoá lời gọi driver, và Chrome cho mỗi video một thread
decode. Object heap, handle thiết bị và trạng thái queue đều dùng chung.

`src/sync.c` được **sinh tự động** bởi `tools/gen-sync.py`: 49 wrapper, mỗi cái
lấy khoá, gọi `Request*`, nhả khoá. Viết tay 49 hàm thì kiểu gì cũng quên một cái.

Hai chỗ cố ý khác biệt:

- **`RequestSyncSurface` nhả khoá quanh `media_request_wait_completion()`** —
  gần như toàn bộ thời gian thực của một ca decode. Giữ khoá qua đó sẽ tuần tự
  hoá chính việc decode chứ không chỉ phần sổ sách.
- **`vaTerminate` KHÔNG được bọc.** Nó giải phóng `driver_data`; wrapper sẽ mở
  khoá trên bộ nhớ đã free.

## `object_heap` không xoá bộ nhớ — phát hiện qua crash Chrome, 2026-09-09

Lỗi tốn nhiều công nhất cho tới giờ, và là lỗi mà **toàn bộ bộ test bit-exact
không thể bắt được**.

`object_heap_allocate()` lấy ô từ một bucket cấp bằng `malloc` và chỉ ghi
`id` với `next_free`. Phần thân của object giữ nguyên byte của chủ cũ. Trang
của bucket mới thì kernel trả về đã zero — nên **mọi client ngắn hạn đều
chạy đúng**, kể cả 18 stream bit-exact và bài test hai luồng đồng thời. Chỉ
tiến trình sống đủ lâu để tái sử dụng ô mới đọc phải rác. Chrome là tiến
trình đó.

`RequestCreateSurfaces2` đặt mọi trường trừ `->session`. Một trường duy nhất,
và nó giải thích cả hai triệu chứng người dùng báo:

| `->session` đọc ra | Hậu quả |
|---|---|
| rác ≠ NULL, ≠ probe, ≠ context | `context_adopt_probe_session()` từ chối: *"surface belongs to another decode session"* → Chrome fallback sang `FFmpegVideoDecoder` |
| rác được deref | SIGSEGV trong `RequestExportSurfaceHandle` → mojo đứt → `PIPELINE_ERROR_DECODE` |

Cách đọc ra: `/var/crash` có `_opt_google_chrome_chrome.1000.crash`,
`apport-unpack` lấy core, gdb **có sẵn trên board** và `.so` giữ `.symtab`:

```
#0  RequestExportSurfaceHandle+156
    ldr x19, [x0, #4408]   ; surface_object->session
    ldr x20, [x19, #8]     ; session->video_format   <-- fault
x19 = 0x5ffc000000026      si_addr = x19+8
```

`x19` không phải con trỏ hợp lệ chứ không phải con trỏ đã free — đó là dấu
hiệu của trường chưa khởi tạo, không phải use-after-free.

Hai việc phải làm:

- **Xoá cả thân object khi cấp phát**, chừa lại `struct object_base`. Vá đúng
  một trường thì lần sau lại vấp trường khác; `destination_map[]`,
  `destination_sizes[]`, `timestamp` đều đang được che bởi
  `destination_buffers_count == 0` chứ không phải bởi khởi tạo.
- **Gỡ surface khỏi session khi context bị huỷ.** Chrome tạo context **không
  kèm danh sách render target**, nên `RequestDestroyContext` không với tới
  được những surface đó qua `surfaces_ids`; chúng sống sót và trỏ vào một ô
  `object_context` đã nằm trong free list, ô mà `vaCreateContext` kế tiếp sẽ
  nhận. `surface_detach_session()` đồng thời đánh dấu surface là chưa bind —
  buffer CAPTURE đã đi cùng handle, nên chỉ số buffer cũ vô nghĩa ở session
  mới.

### Bài học về test

Test ngắn hạn không chứng minh được gì về khởi tạo. Muốn bắt lớp lỗi này
phải có bài chạy **nhiều chu kỳ tạo/huỷ trong cùng một tiến trình** —
`spike/va-decode-test/chrome-cycle.sh` phát clip nối tiếp trong một Chrome
duy nhất và đọc `exit_code=139` từ `gpu_process_host.cc` làm tín hiệu.

## Review trước khi chốt, 2026-09-09

Quét bằng công cụ chứ không đọc chay, vì đọc chay đã bỏ lọt một trường chưa
khởi tạo suốt cả ngày.

**Cảnh báo trình biên dịch.** Build riêng với `-Wall -Wextra -Wshadow
-Wmissing-prototypes -Wstrict-prototypes`. Hai phát hiện, cả hai là lỗi thật:

- `RequestDestroySurfaces` đếm bằng `unsigned int` trong khi `surfaces_count`
  là `int` của ABI. Một count âm sẽ được nâng thành số rất lớn và chạy tràn
  mảng; đếm bằng `int` thì vòng lặp đơn giản là không chạy.
- `v4l2_try_format` không có prototype — dấu hiệu của hàm không ai gọi.

`-Wall` đã bao gồm `-Wunused-function`, và nó im lặng — nghĩa là **không có
hàm `static` nào thừa**. Hàm extern thừa thì trình biên dịch không thấy được.

**Code chết, hỏi linker.** `-ffunction-sections` + `-Wl,--gc-sections
-Wl,--print-gc-sections` liệt kê chính xác thứ bị vứt đi:

| Hàm | Vì sao chết |
|---|---|
| `v4l2_try_format`, `v4l2_create_buffers`, `video_format_find` | bị `v4l2_request_buffers` / `video_format_find_mplane` thay thế |
| `RequestSetSubpicturePalette` | `vaSetSubpicturePalette` không còn trong `VADriverVTable` của libva |
| `SyncSetSubpicturePalette` | wrapper của hàm trên |
| `SyncTerminate` | `vaTerminate` cố ý **không** bọc khoá |

`SyncTerminate` là cái đáng nói. Nó vô hại vì không ai gọi, nhưng để lại thì
người sau chỉ cần nối nó vào vtable là tái tạo đúng lỗi mở khoá trên bộ nhớ đã
free. Nên `tools/gen-sync.py` giờ có tập `SKIP` — xoá tay thì lần sinh lại nó
mọc lên nguyên vẹn. Còn 47 wrapper.

**Slot vtable bỏ trống.** 12 trong 60 slot không được gán (`vaSyncBuffer`,
`vaCopy`, `vaMapBuffer2`, `vaCreateMFContext`...). Nghe như crash chờ sẵn, nên
tôi dịch ngược `libva.so.2` của board thay vì đoán:

```
ldr x3, [x0, #456]   ; vtable->vaSyncBuffer
cbz x3, <thoát>      ; NULL thì bỏ qua
mov w2, #0x14        ; = VA_STATUS_ERROR_UNIMPLEMENTED (20)
```

libva có kiểm NULL. Slot trống là an toàn, **không cần stub**.

**Cấp phát không kiểm NULL.** Ba chỗ deref ngay sau `malloc`:
`driver_data`, `attributes_list`, `export_fds`. Xác suất thấp, nhưng đây là
thư viện chạy trong GPU process của Chrome — lúc hết bộ nhớ là đúng lúc không
được để segfault che mất nguyên nhân thật. Riêng `export_fds` còn tệ hơn: nhãn
`error:` lặp qua chính mảng đó để đóng fd, nên NULL sẽ nổ trong lúc dọn dẹp.

**Rò khi cấp phát hỏng giữa chừng.** `vaCreateSurfaces` tạo được 3 trong 8
surface rồi hỏng thì trả lỗi — client không bao giờ biết 3 cái kia tồn tại nên
không huỷ chúng. Giờ hàm tự thu hồi phần đã tạo.

### Hai nhánh adopt chưa từng chạy

Đo bằng cách gắn log vào đúng chỗ mỗi nhánh nổ (phải ghi ra file, vì Chrome
chuyển hướng stderr của tiến trình con — đo qua stderr cho kết quả toàn số 0
và suýt dẫn tới kết luận sai):

| | ffmpeg | Chrome |
|---|---|---|
| probe session mở | 1 | 0 |
| nhánh `context.c` | 0 | 0 |
| nhánh `picture.c` adopt | 0 | 0 |

`find_unique_context_session()` thì **có chạy** — nó chính là lý do Chrome
không mở probe session nào. Hai nhánh adopt thì chưa lần nào. Giữ lại vì không
tốn gì và chỉ có ý nghĩa với thứ tự "export trước, tạo context sau" kiểu
mpv/VLC — board không cài hai thứ đó nên chưa kiểm được. **Đừng coi chúng là
đang gánh việc.**

## Trạng thái, 2026-09-09

| | |
|---|---|
| Khung driver, khoá, context tự mở node | **xong** |
| Xếp hạng thiết bị, chọn rkvdec | **xong** |
| H.264 + HEVC, bit-exact 18/18 stream | **xong** |
| Buffer sizing theo độ phân giải (8K) | **xong** |
| Chrome: export surface chưa bind, separate layers | **xong** |
| Hai luồng đồng thời, bit-exact | **xong** |
| Chrome phát nối tiếp nhiều clip, không crash GPU process | **xong** 2026-09-09 |
| Review: 0 cảnh báo ở mức tối đa, 0 hàm chết, cấp phát có kiểm NULL | **xong** 2026-09-09 |
| Bitstream buffer 4 MiB trên 1080p | chưa — chỉ lộ ra với nội dung dài, bitrate cao |
| HEVC ext RPS khi `num_short_term_ref_pic_sets != 0` | chưa — x265 mặc định không cần |

## Thứ tự làm

1. Khung driver + khoá + object heap, một context tự mở node
2. Xếp hạng thiết bị, chọn rkvdec
3. H.264: ánh xạ control ổn định, DPB đóng gói từ slot 0, bốn chỗ VA thiếu
4. Kiểm bit-exact với software qua `framemd5`, **nhiều loại stream** (xem bảng
   coverage trong README nguồn: High/Main/Baseline, CABAC/CAVLC, B-frame,
   multi-slice, không chia hết macroblock, IDR dày)
5. HEVC: bốn control, cùng quy tắc DPB
6. Buffer sizing theo độ phân giải
7. Chrome: export surface chưa bind, separate layers
8. Concurrent decode: hai stream một VADisplay

**Không bỏ qua bước 4.** Lỗi DPB-có-lỗ chỉ lộ ra khi so bit-exact trên stream có
IDR thứ hai; clip ngắn một GOP luôn đúng.
