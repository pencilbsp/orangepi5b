# Bộ test bit-exact cho VA driver

Chứng minh driver decode **giống hệt từng bit** với bản decode phần mềm. Đây là
phép thử duy nhất bắt được lỗi ánh xạ điều khiển — chúng không làm decode thất
bại, chúng làm ra ảnh sai.

## Cách chạy

```bash
# 1. Sinh stream (trong chroot amd64, chạy native — encode không liên quan kiến trúc đích)
chroot build/cross-chroot bash /build/make-streams.sh /build/streams

# 2. Đẩy sang board
tar -C build/cross-chroot/build/streams -cf - . | ssh board 'tar -C ~/streams -xf -'
scp spike/va-decode-test/compare.sh board:~/

# 3. So sánh
ssh board 'sudo bash ~/compare.sh ~/streams'
```

`compare.sh` decode mỗi stream **hai lần bằng cùng một binary ffmpeg** — một lần
qua VA-API, một lần phần mềm — rồi so `framemd5` từng frame. Dùng chung một
binary nghĩa là mọi khác biệt đều là của driver, không phải do lệch phiên bản
giữa hai máy.

## Vì sao chọn những stream này

Không phải bộ conformance codec. Mỗi stream nhắm một thứ đã từng hỏng:

| Stream | Nhắm vào |
|---|---|
| `h264-long-repeated` (300f, 10 GOP) | **DPB slot tái sử dụng** — quan trọng nhất |
| `h264-idr-every-5` (24 IDR) | slot bị giải phóng và cấp lại liên tục |
| `h264-cqm-jvt` | ánh xạ scaling list 8x8 (VA list 1 → slot 1, không phải 3) |
| `h264-refs5-bf3` | 5 reference, B-pyramid |
| `h264-4slices` | nhiều slice mỗi frame |
| `h264-854x482` | không chia hết macroblock, kiểm cropping |
| `h264-baseline-cavlc`, `main-cavlc` | CAVLC thay vì CABAC |
| `hevc-idr-every-5` | như trên, phía HEVC |
| `vp9-profile0-repeated` | reset/kế thừa probability context qua nhiều GOP |
| `vp9-profile0-tiles` | compressed header và tile layout nhiều cột |

**Clip ngắn một GOP không chứng minh được gì.** Lỗi DPB-có-lỗ để GOP đầu hoàn
hảo và chỉ hỏng từ sau IDR thứ hai.

## Kết quả 2026-09-08

```
  h264-1080p.h264            PASS  120 frames bit-identical
  h264-4slices.h264          PASS   60 frames bit-identical
  h264-854x482.h264          PASS   60 frames bit-identical
  h264-baseline-cavlc.h264   PASS   60 frames bit-identical
  h264-cqm-jvt.h264          PASS  120 frames bit-identical
  h264-high-cabac.h264       PASS   60 frames bit-identical
  h264-idr-every-5.h264      PASS  120 frames bit-identical
  h264-long-repeated.h264    PASS  300 frames bit-identical
  h264-main-cavlc.h264       PASS   60 frames bit-identical
  h264-refs5-bf3.h264        PASS  120 frames bit-identical
  hevc-1080p.h265            PASS  120 frames bit-identical
  hevc-idr-every-5.h265      PASS  120 frames bit-identical
  hevc-main-basic.h265       PASS   60 frames bit-identical
  hevc-main-bframes.h265     PASS  120 frames bit-identical

pass=14 fail=0 skip=0
```

Kết quả VP9 sau khi boot kernel có patch `0012` và cài VA driver mới,
2026-09-10:

```
  vp9-profile0-repeated.ivf  PASS  180 frames bit-identical
  vp9-profile0-tiles.ivf     PASS  120 frames bit-identical

pass=2 fail=0 skip=0
```

`vainfo` quảng bá `VAProfileVP9Profile0: VAEntrypointVLD`. Bản export
`chrome://media-internals` cho clip VP9 Profile 0 1920x1080 ghi
`kVideoDecoderName = VaapiVideoDecoder` và `kIsPlatformVideoDecoder = true`,
không có warning fallback hay pipeline error.

Regression deploy cùng ngày: module VP9 đầu tiên được build từ cây tạm thiếu
patch kernel `0008`, làm H.264 4K bị `-EBUSY` khi gửi SPS đầu tiên. Sau khi
build lại đủ queue, chính file Chrome đã fallback
`bbb_sunflower_2160p_60fps_normal.mp4` PASS 120/120 frame bit-identical ở
3840x2160. HEVC 1080p PASS 60/60 và cả hai ca VP9 ở trên vẫn PASS.

## Độ phân giải cao

```
  h264-4k.h264   (3840x2160)  PASS  24 frames bit-identical
  h264-8k.h264   (7680x4320)  PASS  24 frames bit-identical
  hevc-4k.h265   (3840x2160)  PASS  24 frames bit-identical
  hevc-8k.h265   (7680x4320)  PASS  24 frames bit-identical
```

**Cả H.264 lẫn HEVC decode 8K đúng từng bit.**

### Throughput 8K, và vì sao chưa đạt spec

Decode thuần (không hwdownload), đo bằng `ffmpeg -f null -`:

| | fps ở 7680x4320 |
|---|---|
| H.264 | 18 |
| HEVC | 19 |

Spec của RK3588 nói 8K@60 cho HEVC. Khoảng cách không phải do driver: DT khai
**hai** instance rkvdec (`0xfdc38000` và `0xfdc40000`) nhưng kernel chỉ probe
một —

```
rkvdec fdc40000.video-codec: missing multi-core support, ignoring this instance
```

— nên chỉ một nửa phần cứng được dùng. Đây là giới hạn của driver rkvdec
thượng nguồn, không phải của tầng VA-API. Con số này khớp với 20.5 fps mà
project tham chiếu đo được qua GStreamer, tức cùng một trần. Driver báo giới hạn lấy từ `VIDIOC_ENUM_FRAMESIZES`
của thiết bị (65520x65520 cho H.264, 65472x65472 cho HEVC) chứ không phải hằng
số — base cũ ghim 3840x2160, biến giới hạn phần cứng thành giới hạn của driver.

Điều kiện để 8K chạy được: **độ sâu pool phải co theo kích thước frame.** Một
frame NV12 8K khoảng 50 MiB; pool cố định 64 buffer sẽ đòi 3.2 GiB từ vùng CMA
512 MiB và stream không bao giờ khởi động. `pool_depth()` chia theo ngân sách
192 MiB, kẹp trong [20, 64].

## Hai luồng đồng thời

```bash
ssh board 'sudo bash ~/concurrent.sh ~/streams'
```

```
=== concurrent decode, one process, one VADisplay ===
  h264-long-repeated.h264      PASS  300 frames bit-identical
  hevc-main-bframes.h265       PASS  120 frames bit-identical
```

Một process, một `VADisplay`, hai decoder — đúng hình dạng một trang web có hai
video. Đây là ca mà `v4l2-mem2mem` trừng phạt việc dùng chung file handle: nó
khoá phiên decode theo handle, nên stream thứ hai lập trình lại queue của stream
thứ nhất ngay dưới chân nó.

Mỗi luồng vẫn được so với bản decode phần mềm **của chính nó**, để "cả hai chạy"
không bị nhầm thành "cả hai đúng".

## Chu kỳ tạo/huỷ trong một tiến trình

```bash
ssh board 'bash ~/chrome-cycle.sh 10 4'
```

Một Chrome duy nhất, một trang đổi `video.src` mỗi 4 giây. Mỗi lần đổi là một
lượt `vaDestroyContext` + `vaDestroySurfaces` rồi tạo lại — nên `object_heap`
phải **tái sử dụng** ô cũ thay vì luôn lấy trang mới từ kernel.

Tín hiệu đọc là `exit_code=139` (128+11, SIGSEGV) từ `gpu_process_host.cc`, và
số ngắt rkvdec mỗi chu kỳ: clip 60 frame thì mỗi chu kỳ phải đúng `+60`. Chu kỳ
nào `+0` là chu kỳ đó không có phần cứng nào chạy.

```
                     driver 281418138bb6      driver 975a0cae
cycle 1..10          +0 +60 +60 +60 +60       +60 x 10
                     +0 +60 +60 +60 +0
GPU process          exit_code=139            không crash
tổng ngắt rkvdec     420                      600
```

Bản cũ mất 2 trong 10 chu kỳ vào SIGSEGV. Xem `docs/VA-DRIVER-DESIGN.md`,
mục `object_heap` không xoá bộ nhớ.

## Bộ test đã bắt được lỗi gì

**`v4l2_h264_dpb_entry.fields` để 0.** Lần chạy đầu: HEVC pass cả bốn, H.264
fail cả mười, và **chỉ frame intra đúng** — khoảng 2 frame mỗi GOP:

```
h264-high-cabac.h264   FAIL  56/60 frames differ
h264-long-repeated     FAIL  280/300 frames differ
h264-idr-every-5       FAIL  72/120 frames differ    ← IDR dày nên đúng nhiều hơn
```

Trường `fields` nói entry được tham chiếu như nửa nào của frame. Để 0 nghĩa là
"không nửa nào" — entry **hợp lệ nhưng vô dụng** làm reference. Nên frame intra
vẫn đúng và mọi frame inter đều sai. Phải là `V4L2_H264_FRAME_REF` cho ảnh
progressive.

Đáng ghi lại vì đây đúng loại lỗi mà chỉ so bit-exact mới thấy: không có lỗi
ioctl nào, không có dòng log nào, decode "thành công" và ảnh sai.

## Bộ test này KHÔNG bắt được lỗi gì

Cần nói rõ, vì đã trả giá: **so bit-exact không nói được gì về khởi tạo.**

Trường `->session` của surface chưa bao giờ được khởi tạo. Cả 18 stream vẫn
pass, kể cả bài hai luồng đồng thời, vì mỗi lượt `ffmpeg` là một tiến trình
mới và trang bucket đầu tiên do kernel cấp đã zero — trường rác đọc ra đúng
`NULL`, tức đúng giá trị mà code mong đợi. Lỗi chỉ hiện ra khi tiến trình sống
đủ lâu để cấp lại một ô đã dùng.

Đó là lý do có `chrome-cycle.sh`. Một bộ test đúng đắn về nội dung vẫn có thể
mù hoàn toàn về vòng đời.
