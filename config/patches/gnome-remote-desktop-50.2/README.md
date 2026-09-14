# gnome-remote-desktop 50.2 — patch cho Orange Pi 5B

Áp lên source package Ubuntu `gnome-remote-desktop_50.2-0ubuntu0.1`, **thêm**
vào `debian/patches/series` của Ubuntu chứ không thay.

Hai patch, đều là workaround cho lệch pha giữa GRD và encoder stateful của
Rockchip, **không phải** tính năng riêng của board. Mỗi patch ghi rõ điều kiện
để bỏ đi.

Có patch thứ ba trong thư mục này,
`0001-hwaccel-vulkan-fall-back-to-v1-modifier-query.patch`, nhưng nó **không
nằm trong `series`** nên không được áp. `queue_patches()` chỉ đọc `series`, nên
file cứ để đó là đủ. Lý do và số đo: mục cuối file.

## Vì sao chỉ có hai

Project tham chiếu (`/root/orangepi5b`) mang **chín** patch cho GRD 50.2, vì nó
chạy trên Ubuntu 24.04. Bảy trong số đó là shim cho dependency cũ — PipeWire
1.0.5, libei 1.2.1, GDM 46. Ubuntu 26.04 ship PipeWire 1.6.2, libei 1.5.0,
GDM 50.1, nên **sáu patch thành thừa** (`0001`, `0002`, `0003`, `0005`, `0006`,
`0009`) và `0007` chỉ là log chẩn đoán, không cần cho bản ship.

Đặc biệt: patch `0005`/`0006` của reference tồn tại vì PipeWire 1.0.5 không có
syncobj timeline, mà GRD lại **bắt buộc** có explicit sync mới tạo phiên encode.
Trên 26.04 điều kiện đó thoả sẵn — đây là thứ mở khoá cả đường encode.

## `0002` — không đòi `HOST_CACHED` cho state buffer

GRD xin `HOST_VISIBLE|HOST_COHERENT|HOST_CACHED` cho damage/chroma state
buffer. PanVK có `HOST_CACHED` (memory type 1) và có `HOST_COHERENT` (type 2)
nhưng **không bao giờ cùng một type**, nên điều kiện không thể thoả và
view creator VA-API không được tạo.

**Đây không phải bug Mesa.** panvk chỉ khai báo type gộp cả hai khi
`device->kmod.dev->props.is_io_coherent`, và trên board này GPU **thật sự không
IO-coherent** — node DT `gpu@fb000000` không có `dma-coherent`. panvk đang nói
đúng sự thật phần cứng.

Cached access là **gợi ý hiệu năng**, không phải điều kiện đúng đắn. Patch xin
host-visible + coherent, đúng thứ buffer thật sự cần.

**Điều kiện bỏ patch này:** GRD thượng nguồn nới yêu cầu. Đây là bug đáng gửi
lên GRD — nó làm hỏng mọi GPU không IO-coherent, không riêng Mali.

## `0003` — crop AVC stream về visible size thật

GRD align surface encode theo macroblock/coded size. Với desktop 1920x1080,
surface làm việc thành 1920x1088 để RKVENC nhận được chiều cao chia hết cho 16.
Đó là kích thước buffer phần cứng, nhưng SPS H.264 phải khai báo crop để decoder
chỉ trình bày 1080 dòng thật.

Patch này giữ `source_width`/`source_height` cạnh `surface_width`/`surface_height`,
set `frame_cropping_flag` khi aligned surface lớn hơn source, và để NAL writer
ghi bốn crop offset vào SPS. Vì H.264 4:2:0 tính crop theo chroma sample, 8 dòng
padding tương ứng `frame_crop_bottom_offset = 4`.

Kích thước desktop do client gửi có thể lẻ, đặc biệt khi Windows App trên macOS
đổi kích thước động. SPS 4:2:0 không biểu diễn được crop một pixel, nên patch
crop tới kích thước chẵn kế tiếp và để RDP surface clip hàng/cột dư. Kích thước
không hợp lệ hoặc tràn khi align trả lỗi để GRD fallback thay vì `g_assert()`
làm chết handover daemon.

**Điều kiện bỏ patch này:** GRD upstream tự ghi H.264 frame cropping khi VA-API
encode surface được align lớn hơn kích thước desktop/source.

## Khác gì với cách project tham chiếu build

Project tham chiếu (`/root/orangepi5b`) build GRD **từ tarball upstream** tag
`50.2` (commit `60423c8`) rồi **tự nặn `.deb` bằng tay**:
`packages/gnome-remote-desktop/DEBIAN/control.in` với `@SHLIB_DEPENDS@`, cộng
`postinst`/`postrm` viết tay lo `systemd-sysusers`, `systemd-tmpfiles`,
`glib-compile-schemas`, và `deb-systemd-helper --user enable` cho ba user unit.
`.deb` thành phẩm được commit đè `.gitignore` bằng `git add -f`, và một script
riêng chỉ kiểm SHA-256 rồi stage nó.

**Không phải vì họ thích làm khó.** Ubuntu 24.04 ship GRD **46.3** — không hề có
source package 50.2 để mà patch. Họ không có lựa chọn nào khác.

Ubuntu 26.04 ship thẳng `gnome-remote-desktop 50.2-0ubuntu0.1` **có source
package**. Nên repo này patch source package đó, và được lại toàn bộ những thứ
kia miễn phí:

| | Reference (24.04) | Repo này (26.04) |
|---|---|---|
| Nguồn | tarball upstream | **source package Ubuntu** |
| `Depends` | `@SHLIB_DEPENDS@` tự sinh + liệt kê tay | `dh_shlibdeps` với thư viện 26.04 thật |
| sysusers / tmpfiles / schemas | `postinst` viết tay | `dh_installsysusers`, `dh_installtmpfiles`, `dh_installgsettings` |
| Bật user unit | `deb-systemd-helper` viết tay | `dh_installsystemduser` |
| `.deb` trong git | commit bằng `git add -f` | **không commit**, build lại được từ script |
| Số patch | 9 | **3** |

Ít thứ phải tự bảo trì hơn, và không có nguy cơ lệch khỏi packaging của Ubuntu.

### Một thứ của reference cố tình không mang sang

`gnome-remote-desktop-handover.desktop` đặt vào
`/usr/share/gdm/greeter/autostart/`. Reference cần nó vì **GDM 46** khởi động
remote greeter bằng một `dbus-run-session` riêng, không kích hoạt
`gnome-session.target`, nên user unit không bao giờ chạy.

Ubuntu 26.04 có GDM 50.1 và **đã ship sẵn** `gnome-remote-desktop-handover.service`
trong `/usr/lib/systemd/user/` (đã kiểm tra trong `build/rootfs`). Nên hack này
không cần nữa. Đã verify bằng phiên RDP thật với package
`50.2-0ubuntu0.1+orangepi5b3`: handover daemon nạp
`v4l2_request_drv_video.so`, giữ `/dev/video5`, và desktop vẫn trình bày đúng
1920x1080 trong khi coded surface của RKVENC là 1920x1088.

## Build

```bash
sudo scripts/build-grd-package.sh
```

Dựng chroot arm64 từ chính tarball `ubuntu-base-26.04.1` mà `install-rootfs.sh`
dùng, nên daemon link đúng thư viện image ship. Script từ chối package mà
daemon không có `vaCreateConfig`/`vaExportSurfaceHandle`/`vaBeginPicture` —
tức là build ra bản không có đường VA-API.

## `0001` đã rút khỏi `series` — nguyên nhân gốc đã sửa ở Mesa

Patch này làm GRD hỏi DRM format modifier bằng bản v1 khi bản v2 trả về rỗng,
vì `panvk_GetPhysicalDeviceFormatProperties2()` không điền
`VkDrmFormatModifierPropertiesList2EXT`. Đó là workaround phía consumer cho một
bug nằm trong Mesa.

Bug đó giờ được sửa đúng chỗ: `config/patches/mesa-26.0.8/`, patch `0002`. Và
kể từ khi `scripts/build-mesa-package.sh` dựng được gói Mesa đã vá (build native
arm64, xem `CLAUDE.md`), ảnh ship chính bản Mesa đó — nên workaround không còn
lý do tồn tại.

**File vẫn giữ lại, chỉ rút khỏi `series`.** Nếu vì lý do gì đó ảnh phải quay
về Mesa gốc của Ubuntu, thêm lại một dòng vào `series` là đủ để có hardware
encode trở lại — không phải đi tìm lại patch trong lịch sử git.

Ghi chú cũ ở đây từng viết **"đã chọn không ship bản Mesa vá"** vì compile Mesa
tốn kém. Điều kiện đó đã đổi: build mất **8 phút 50 giây** và không phải vá
`debian/` của Ubuntu.

### Đo trên board trước khi gỡ

GRD build với queue rút gọn (chỉ `0002` + `0003`, **không** có `0001`), cài lên
board, đo hai lần chỉ khác nhau ở bản Mesa:

| Mesa | log của daemon phiên |
|---|---|
| `26.0.8-1ubuntu0.3` gốc | `[HWAccel.Vulkan] Could not acquire Vulkan physical device: Could not find proper device` |
| `+orangepi5b1` đã vá | *không còn dòng đó* — đi tiếp tới VAAPI |

Đối chứng lặp 2/2 lần, bản vá 3/3 lần. Rồi chạy trọn end-to-end qua GNOME
Remote Login, phiên thật của người dùng (uid 1000):

```text
v4l2-request: device: encoder at /dev/video4
v4l2-request: device: using /dev/video3 with /dev/media1
v4l2-request: device: using /dev/video6 with /dev/media3
[HWAccel.VAAPI] Successfully initialized VAAPI 1.23 with vendor: v4l2-request
v4l2-request: encode: /dev/video4 1920x1088, 4 raw buffers, 4 coded buffers
```

Khớp đúng những dòng mà header của patch `0001` từng ghi là kết quả sau khi vá
GRD. Patch Mesa thay được hoàn toàn. Phiên chạy liên tục 8 phút, daemon giữ
`/dev/video4`, CPU 0.4%.

`1920x1088` ở dòng cuối là lý do `0003` vẫn phải ở lại: 1080 bị pad lên 1088.

### Một cái bẫy khi đo lại

"Remote Login" của GNOME chạy hai daemon. Giai đoạn đầu là màn hình đăng nhập
GDM từ xa, daemon chạy bằng **user tạm của GDM** (uid 6058x, nhóm `gdm`), không
có ACL `uaccess` cho node codec, nên nó luôn báo:

```text
v4l2-request: device: no V4L2 stateless H.264/HEVC/VP9 decoder found
```

Đó **không** phải lỗi. Phiên thật chỉ bắt đầu sau handover, dưới uid của người
dùng, và ở đó encoder mở được. Đọc log phải nhìn uid của tiến trình, nếu không
sẽ kết luận nhầm là hardware encode hỏng.
