# Kiểm chứng patch queue Mesa trên board

Điều tra + đo cho `config/patches/mesa-26.0.8`. Không thuộc ảnh.

```bash
bash spike/mesa-patches/build.sh              # cross-build stock + patched
bash spike/mesa-patches/stage-from-debs.sh    # hoặc lấy cả hai từ .deb
bash spike/mesa-patches/measure-avatar.sh     # patch 0001
bash spike/mesa-patches/measure-modifiers.sh  # patch 0002
```

`build.sh` giải thích vì sao phải đi đường meson tối giản thay vì source
package của Ubuntu. Hai script đo **không sửa gì trên board**: thư viện nằm ở
`/tmp`, chọn theo từng tiến trình bằng `LD_LIBRARY_PATH` / `LIBGL_DRIVERS_PATH`
/ `VK_DRIVER_FILES`. Phải chạy khi board đang ở màn hình đăng nhập hoặc đã đăng
nhập, vì qua SSH không có seat — `/dev/dri/renderD128` chỉ có ACL cho chủ của
`seat0`. `lib-board.sh` tự tra ra tài khoản đó.

# Patch 0001 — EGL dmabuf export

## Triệu chứng ban đầu

Avatar trên màn hình đăng nhập là một ô nhiễu, dù không ai đặt ảnh.
`/var/lib/AccountsService/icons/<user>` là PNG 512×512 mang chunk
`tEXt: source=gnome-generated`, ghi bởi `gnome-initial-setup` đúng lúc tạo tài
khoản (journal: `create user 'pencil'`). GNOME tự sinh avatar chữ cái đầu khi
người dùng không chọn ảnh — `gis-account-page-local.c:628`, `IMAGE_SIZE 512`.

Đường sinh avatar render trên GPU rồi đọc ngược về CPU:

```
AdwAvatar → gsk_renderer_render_texture() → gdk_pixbuf_get_from_texture() → PNG
```

CRC của PNG còn nguyên, nên hỏng xảy ra trước lúc mã hoá — tức ở khâu đọc
ngược texture.

## Kết quả

Năm lần chạy sạch, mỗi lần dựng lại thư mục từ đầu. Bốn lần đầu trong session
GDM greeter, lần thứ năm trong desktop session đã đăng nhập:

| lần | stock | patched |
|---|---|---|
| 1 | 151040 B, `Gdk-CRITICAL` | **13968 B**, không lỗi |
| 2 | 133678 B, `Gdk-CRITICAL` | **13968 B**, không lỗi |
| 3 | 33996 B, `Gdk-CRITICAL` | **13968 B**, không lỗi |
| 4 | 110453 B, `Gdk-CRITICAL` | **13968 B**, không lỗi |
| 5 | 142693 B, `Gdk-CRITICAL` | **13968 B**, không lỗi |

Chữ ký rất rõ: bản gốc ra kích thước **khác nhau mỗi lần** — đúng như đọc phải
vùng nhớ chưa khởi tạo, nội dung tuỳ trạng thái RAM lúc đó. Bản vá ra **đúng
13968 byte mỗi lần**, là avatar thật (hình tròn gradient + chữ cái đầu), khớp
với kích thước thu được khi ép `GDK_DISABLE=dmabuf` trước đó.

Cả hai bản vẫn in `MESA: error: drmPrimeHandleToFD() failed (err=22)` — đó là
panthor từ chối export BO nằm trong exclusive VM, và patch không định sửa
chuyện đó. Khác biệt nằm ở chỗ tiếp theo:

```text
stock:    drmPrimeHandleToFD() failed (err=22)
          Gdk-CRITICAL: Failed to download 512x512 dmabuf texture (AB24:0x800000000000351)
patched:  drmPrimeHandleToFD() failed (err=22)
          (không có gì thêm — GTK rơi xuống glReadPixels và ra ảnh đúng)
```

Ảnh của lần chạy cuối nằm ở `build/mesa-patches/result/`.

## Phạm vi kết luận

Bản Mesa đem đo là bản tối giản (chỉ panfrost + EGL, không LLVM), **không phải**
gói của Ubuntu — lý do ở đầu `build.sh`. Code được vá nằm trong EGL core và
không phụ thuộc driver hay LLVM, nên đường chạy là một; nhưng muốn ship thì vẫn
còn việc làm cho `debian/control` cross-build được. Xem phần cuối
`config/patches/mesa-26.0.8/README.md`.


# Patch 0002 — panvk trả lời truy vấn modifier bản v2

`modifier-probe.c` hỏi panvk cả hai kiểu cho năm format mà
`gnome-remote-desktop` quan tâm, rồi in cả số lượng lẫn giá trị modifier.

```text
===== stock                          ===== patched
device: Mali-G610 MC4                device: Mali-G610 MC4
  B8G8R8A8_UNORM  v1=1 [0x0] v2=0 []   B8G8R8A8_UNORM  v1=1 [0x0] v2=1 [0x0]
  R8G8B8A8_UNORM  v1=1 [0x0] v2=0 []   R8G8B8A8_UNORM  v1=1 [0x0] v2=1 [0x0]
  A2B10G10R10     v1=1 [0x0] v2=0 []   A2B10G10R10     v1=1 [0x0] v2=1 [0x0]
  A2R10G10B10     v1=1 [0x0] v2=0 []   A2R10G10B10     v1=1 [0x0] v2=1 [0x0]
  NV12            v1=1 [0x0] v2=0 []   NV12            v1=1 [0x0] v2=1 [0x0]
```

Khớp đúng con số ghi trong `config/patches/gnome-remote-desktop-50.2/README.md`
("cả sáu format GRD quan tâm đều trả v1=1, v2=0"). Sau patch, v2 trả về **cùng
một modifier `0x0` = `DRM_FORMAT_MOD_LINEAR`** mà v1 vẫn trả — đúng cái mà log
của GRD sau khi vá `0001` báo: *"Found DRM format modifier 0 for DRM format
875713112"*.

## Hai patch độc lập với nhau

Đọc `gdk/gdkvulkancontext.c:2166`: GDK hỏi modifier bằng **bản v1**, và cố tình
**loại bỏ LINEAR** (`advertise = modifier != DRM_FORMAT_MOD_LINEAR`). panvk chỉ
trả LINEAR — các modifier AFBC nằm sau cờ debug `PANVK_DEBUG(WSI_AFBC)` — nên
GDK vẫn kết luận "no dmabuf support" và vẫn chọn `GskGLRenderer`.

Nghĩa là `0002` **không** sửa lỗi avatar, và `0001` **không** mở đường encode
của GRD. Hai lỗi riêng, hai patch riêng.

## End-to-end: GRD bỏ `0001` chạy được trên Mesa đã vá

Build GRD **không có** `0001` (`scripts/build-grd-package.sh` nhận
`GRD_PATCH_DIR` để dựng queue rút gọn mà không phải sửa queue đang ship), cài
lên board, trỏ user unit vào từng bản Mesa bằng drop-in, rồi nối
`xfreerdp3` tới `localhost`. Hai lần chạy liên tiếp, cùng cổng, cùng session,
chỉ khác biến môi trường trỏ vào Mesa nào:

```text
--- stock Mesa + GRD không có 0001
[HWAccel.Vulkan] Checking physical device 1/1
[HWAccel.Vulkan] Skipping device: No DRM format modifiers available for DRM format 875713112
[HWAccel.Vulkan] Could not acquire Vulkan physical device: Could not find proper device

--- patched Mesa + GRD không có 0001
[HWAccel.Vulkan] Checking physical device 1/1
[HWAccel.Vulkan] Found DRM format modifier 0 for DRM format 875713112
[HWAccel.Vulkan] Using device features: 0x00000003
v4l2-request: device: encoder at /dev/video4
[HWAccel.VAAPI] Using VAEntrypoint 6 for profile VAProfileH264High
[HWAccel.VAAPI] Successfully initialized VAAPI 1.23 with vendor: v4l2-request
```

Vế dưới đúng từng dòng với những gì header của
`config/patches/gnome-remote-desktop-50.2/0001-*.patch` ghi là kết quả sau khi
vá GRD. **Patch panvk thay được hoàn toàn GRD `0001`.**

Phải có client RDP thật, không lách được: `grd_hwaccel_vulkan_new()` lúc daemon
khởi động mới chỉ tạo `VkInstance`; việc chọn physical device — chỗ hỏi
modifier — nằm trong `grd_hwaccel_vulkan_acquire_physical_device()`, do
`grd-rdp-renderer.c:204` gọi khi có phiên.

### Hai cái bẫy gặp phải

`Failed to start remote desktop session: Session creation inhibited` — GNOME từ
chối tạo phiên remote desktop khi **màn hình đang khoá**. Board tự khoá sau vài
phút idle nên lần chạy control đầu tiên hỏng vì lý do chẳng liên quan gì tới
Mesa. Phải `loginctl unlock-session` trước khi đo, và khoá lại sau.

Cổng 3389 của board **mở ra internet**: ngay khi bật RDP đã có kết nối từ
160.191.50.249 và 103.92.25.71 (đều fail NTLM). Đo xong phải tắt RDP và xoá
credentials — đừng để bật.

### Việc đo này không chạy lại tự động được

Nó cần cài đè package, sửa unit, bật RDP, mở khoá màn hình — quá nhiều thay đổi
trên board để đóng thành script chạy một phát như hai script `measure-*.sh`.
Các bước đã ghi ở trên, làm tay khi cần.

## Đã ship, và `0001` của GRD đã rút khỏi `series`

Kết luận ở trên về sau được xác nhận bằng chính gói sẽ ship. Nút cross-build
(libclang/llvm ở đầu `build.sh`) không được gỡ mà được **đi vòng**: mesa dựng
native trên arm64 bằng `scripts/build-mesa-package.sh`, nơi apt universe chỉ có
một kiến trúc nên không có xung đột nào để gỡ. Chi tiết và số đo end-to-end:
`config/patches/mesa-26.0.8/README.md`.

Ảnh giờ cài Mesa đã vá, và `0001` của GRD đã rút khỏi
`config/patches/gnome-remote-desktop-50.2/series` — đúng như đo ở đây dự đoán.

Hai script `measure-*.sh` chạy được bằng **cả hai** nguồn thư viện:

```bash
bash spike/mesa-patches/build.sh             # cây panfrost tối giản, cross-build
bash spike/mesa-patches/stage-from-debs.sh   # hoặc: chính .deb sẽ ship
```

Cả hai đều ghi vào `build/mesa-patches/{stock,patched}` nên hai script đo không
phải đổi gì. Đã chạy bằng `.deb` và kết quả trùng khít bảng ở trên — avatar ra
đúng 13968 byte, v2 trả `1 [0x0]` cho cả năm format. Số đo đầy đủ:
`config/patches/mesa-26.0.8/README.md`.
