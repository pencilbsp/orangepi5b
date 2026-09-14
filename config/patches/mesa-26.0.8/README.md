# Mesa 26.0.8 — patch cho Orange Pi 5B

Áp lên source package Ubuntu `mesa_26.0.8-1ubuntu0.3`, **thêm** vào
`debian/patches/series` của Ubuntu chứ không thay. Ubuntu đã có năm patch ở
đó; patch này nối vào cuối và không đụng file nào trong số đó
(`egl-gbm-Ignore-current-front-buffer-in-get_back_bo.patch` sửa
`platform_drm.c`, không phải `egl_dri2.c`).

## `0001` — báo lỗi khi export dmabuf thất bại

`dri2_export_dma_buf_image_mesa()` kiểm tra giá trị trả về của mọi truy vấn
`__DRI_IMAGE_ATTRIB` **trừ** đúng cái quan trọng nhất: nó bỏ qua kết quả của
`__DRI_IMAGE_ATTRIB_FD` rồi `return EGL_TRUE` vô điều kiện. Khi truy vấn đó
fail, `fds[i]` không bao giờ được ghi — caller nhận lại rác trên stack của
chính nó dưới dạng file descriptor, kèm thông báo "thành công".

Trên RK3588 đây không phải chuyện lý thuyết. panfrost cấp offscreen render
target từ exclusive VM của device, và panthor từ chối export đúng loại BO đó
(`panthor_gem_prime_export()`, `drivers/gpu/drm/panthor/panthor_gem.c`):

```c
/* We can't export GEMs that have an exclusive VM. */
if (to_panthor_bo(obj)->exclusive_vm_root_gem)
        return ERR_PTR(-EINVAL);
```

nên `drmPrimeHandleToFD()` trả EINVAL cho mọi buffer như vậy.

Đo được trên board (session GDM thật, Mesa 26.0.8, GTK 4.22.4, kernel 7.1.8),
hỏng 4/4 lần:

```text
Using renderer 'GskGLRenderer' for surface 'GdkWaylandToplevel'
MESA: error: drmPrimeHandleToFD() failed (err=22)
Gdk-CRITICAL: Failed to download 512x512 dmabuf texture (format AB24:0x800000000000351)
```

Hệ quả nhìn thấy được: `gnome-initial-setup` sinh avatar mặc định bằng
`gsk_renderer_render_texture()` + `gdk_pixbuf_get_from_texture()`, nhận về
buffer chưa khởi tạo, và lưu nguyên bộ nhớ rác đó thành
`/var/lib/AccountsService/icons/<user>`. Màn hình đăng nhập hiện một ô nhiễu.
Mọi app GTK4 snapshot widget ra texture rồi đọc về CPU đều dính, không riêng
avatar.

Patch chỉ làm một việc: trả `EGL_FALSE` khi truy vấn fd fail, sau khi đóng
những descriptor đã export. Không đổi hành vi đường thành công. Consumer đã
xử lý sẵn trường hợp export bị từ chối — GTK rơi xuống đọc texture bằng
`glReadPixels` (`gsk/gpu/gskgpudownloadop.c`), đường đó chạy đúng trên phần
cứng này.

Đã đo trên board, 4/4 lần: bản gốc ra file kích thước khác nhau mỗi lần
(151040 / 133678 / 33996 / 110453 byte) kèm `Gdk-CRITICAL`; bản vá ra đúng
13968 byte mỗi lần, là avatar thật, không lỗi. Chi tiết và cách chạy lại:
`spike/mesa-patches/README.md`.

**Điều kiện bỏ patch này:** Mesa thượng nguồn kiểm tra giá trị trả về của lần
truy vấn `__DRI_IMAGE_ATTRIB_FD` trong `dri2_export_dma_buf_image_mesa()`, và
bản sửa về tới Ubuntu.

Nếu panfrost sau này cấp render target không nằm trong exclusive VM thì export
sẽ thành công và patch thành no-op — **đó không phải lý do để bỏ nó**, vì lỗi
không kiểm tra giá trị trả về vẫn còn nguyên cho driver khác.

## Ghi chú: quyết định này ngược với lần trước

`config/patches/gnome-remote-desktop-50.2/README.md` đã từng ghi rõ **chọn
không ship bản Mesa vá**, vì compile Mesa tốn kém mỗi lần rebase còn patch
phía consumer thì rẻ hơn nhiều. Lần này khác ở chỗ lỗi nằm ngay trong EGL
core chứ không phải trong panvk, và nó làm hỏng dữ liệu một cách im lặng cho
*mọi* consumer EGL, không riêng một app. Một patch 2 dòng phía GTK
(`gdk/gdkglcontext.c`: khởi tạo `fds[]` bằng -1 rồi từ chối `fds[0] < 0`) cũng
cho ra avatar đúng và rẻ hơn khi rebase — nếu chi phí rebuild Mesa là vấn đề
thì đó là lựa chọn thay thế hợp lệ.

## Vì sao không cross-build được

Đã truy tới tận gốc. **Nút thắt không nằm ở packaging của mesa** — nó nằm ở
LLVM và python3-yaml.

Chuỗi, đo bằng `apt-get install -s` trong một chroot sạch:

```text
llvm-21-dev:arm64 → llvm-21-tools:arm64 → python3-yaml → python3:arm64
```

`python3-yaml` là `Multi-Arch: allowed` nhưng trường Depends của nó ghi:

```text
Depends: python3 (<< 3.15), python3 (>= 3.14~), python3:any, libc6, libyaml-0-2
```

Hai vế **không có qualifier** buộc cùng kiến trúc, nên `python3-yaml:arm64` kéo
theo `python3:arm64`, mà `python3:amd64 Conflicts python3:arm64`. Kết quả là cài
LLVM cho arm64 sẽ **hất văng python3 bản native** — chính thông dịch viên mà
meson và toàn bộ script sinh mã của mesa chạy trên đó.

apt nói thẳng ra điều này:

```text
python3:amd64 is selected for removal because:
  1. python3-yaml:arm64 is selected for install
  2. python3-yaml:arm64 Depends python3:arm64 (>= 3.14~)
  3. python3:amd64 Conflicts python3:arm64
```

Thêm `:native` vào `debian/control` của mesa không sửa được chuyện này. Nó nằm
trong packaging của `llvm-21-tools` (đáng lẽ phải là `python3-yaml:any`) và của
`python3-yaml` (đáng lẽ chỉ `python3:any`).

Xung đột `bindgen:amd64` ↔ `libclang-21-dev:arm64` nêu trước đây là có thật
nhưng chỉ là lớp ngoài: nó biến mất nếu bỏ rusticl/NVK, còn nút python3 thì
không, vì llvmpipe — thứ ảnh thật sự cần — bắt buộc phải có LLVM cho arm64.

### Đường đi khả dĩ, đã bỏ

Có thể không cài `llvm-21-dev:arm64` bằng apt mà **giải nén** nó vào chroot
bằng `dpkg-deb -x`, để không kéo theo `llvm-21-tools:arm64`; cộng thêm stage
build native riêng cho `mesa_clc` / `vtn_bindgen2` / `panfrost_compile` rồi
patch `debian/rules`.

Đã bỏ hướng này. Nó là một dự án build-system nhiều bước, và phải sửa
`debian/` của Ubuntu — trong khi build native trên arm64 không phải sửa gì và
mất chưa tới 9 phút. Xem mục dưới.

## `0002` — panvk trả lời truy vấn DRM format modifier bản v2

`panvk_GetPhysicalDeviceFormatProperties2()` điền
`VkDrmFormatModifierPropertiesListEXT` nhưng bỏ qua
`VkDrmFormatModifierPropertiesList2EXT`, nên bản v2 luôn trả count = 0. panvk
có quảng cáo `VK_KHR_format_feature_flags2` — chính thứ khiến hỏi kiểu v2 là
hợp lệ — nên caller có đủ lý do để dùng nó. Sáu driver Vulkan khác của Mesa
(radv, anv, v3dv, nvk, hk) đều trả lời cả hai.

Đây đúng là nguyên nhân gốc mà
`config/patches/gnome-remote-desktop-50.2/README.md` chỉ ra cho patch `0001`
của GRD.

Đo trên board, năm format GRD quan tâm: trước patch `v1=1 [0x0], v2=0 []`; sau
patch `v1=1 [0x0], v2=1 [0x0]`. Cùng một modifier `DRM_FORMAT_MOD_LINEAR` —
đúng cái mà GRD đã vá báo trong log (*"Found DRM format modifier 0"*). Chi tiết:
`spike/mesa-patches/README.md`.

Đã đo end-to-end: GRD build **không có** patch `0001` của nó, chạy trên Mesa
gốc thì `Skipping device: No DRM format modifiers available`; chạy trên Mesa đã
vá thì `Found DRM format modifier 0` rồi `[HWAccel.VAAPI] Successfully
initialized VAAPI 1.23 with vendor: v4l2-request`. Tức patch này **thay được
GRD `0001`** — xem `spike/mesa-patches/README.md`.

**Điều kiện bỏ patch này:** panvk thượng nguồn điền cả hai danh sách, và bản
sửa về tới Ubuntu.

**Lưu ý:** patch này **không** liên quan tới `0001`. GDK hỏi modifier bằng bản
v1 và loại bỏ LINEAR (`gdk/gdkvulkancontext.c:2166`), nên `0002` không đổi lựa
chọn renderer và không sửa lỗi avatar. Ngược lại `0001` cũng không mở đường
encode của GRD.

## Đã build được: native trên arm64, không cross

Gói này dựng trong **container Linux arm64 trên máy Apple Silicon**, không
cross-build. Nút thắt ở trên là hệ quả của multiarch — một apt universe có cả
amd64 lẫn arm64. Trên máy arm64 chỉ có một kiến trúc, nên `apt-get build-dep
mesa` giải bình thường: không cần `:native`, không xung đột libclang, không ai
hất văng `python3`. `debian/` của Ubuntu giữ nguyên, không phải vá. Đây đúng là
cách buildd của Ubuntu build mesa.

Quy tắc trong `CLAUDE.md` cấm **emulation**, với lý do "không có lý do gì phải
emulate chính compiler". Trên Apple Silicon compiler chạy native arm64 và sinh
mã arm64 — thoả đúng nguyên tắc đó. `CLAUDE.md` đã được sửa để nói rõ điều này
("Ngoại lệ: build native trên máy arm64 thật").

```bash
brew install container            # container CLI của Apple
container system kernel set --recommended
bash scripts/build-mesa-package.sh
```

Script chạy **trên macOS**, không chạy trên host x86, và từ chối chạy nếu guest
không phải `aarch64`/`arm64` — amd64 dưới Rosetta là emulation, đúng thứ quy
tắc cấm.

### Đo được

**8 phút 50 giây** tổng cộng trên M-series 12 nhân (build dùng 10), tính cả tải
44MB source và tải + cài toàn bộ build-dep. Đó là cây mesa **đầy đủ** — mọi
gallium driver, LLVM, rusticl, NVK — chứ không phải bản panfrost tối giản của
`spike/mesa-patches/build.sh`. So với con số `CLAUDE.md` ghi cho chroot arm64
dưới qemu (**~80 phút** cho cùng cây Mesa), đường này nhanh hơn gần một bậc độ
lớn mà vẫn không vá gì trong `debian/`.

apt archive và tarball nằm lại ở `cache/mesa-arm64/` (438MB), nên chạy lại
không tải lại.

Patch áp đúng thứ tự, năm patch của Ubuntu trước, hai patch của repo sau:

```text
dpkg-source: info: applying path_max.diff
dpkg-source: info: applying src_glx_dri_common.h.diff
dpkg-source: info: applying egl-gbm-Do-not-destroy-BO-of-current-front-buffer.patch
dpkg-source: info: applying egl-gbm-Ignore-buffers-with-no-BO-for-destroying-exc.patch
dpkg-source: info: applying egl-gbm-Ignore-current-front-buffer-in-get_back_bo.patch
dpkg-source: info: applying 0001-egl-dri2-fail-dmabuf-export-when-the-fd-query-fails.patch
dpkg-source: info: applying 0002-panvk-answer-the-v2-drm-format-modifier-query.patch
```

Ra `mesa_26.0.8-1ubuntu0.3+orangepi5b1`. Script mang về **cả sáu** gói, không
lấy lẻ được vì chúng ràng buộc nhau bằng `mesa-libgallium (= ${binary:Version})`:

```
libegl-mesa0  libgbm1  libgl1-mesa-dri  libglx-mesa0  mesa-libgallium
mesa-vulkan-drivers
```

Script kiểm lại nội dung gói trước khi nhận: `mesa-libgallium` phải có
`libgallium-*.so` và `mesa-vulkan-drivers` phải có `libvulkan_panfrost.so`. Một
bản mesa thiếu hai thứ đó vẫn cài sạch và vẫn để board rơi xuống llvmpipe mà
không nói gì — hai patch này thành vô nghĩa, nên phải chặn ngay ở đây thay vì
phát hiện trên board.

### Đã vào ảnh

`scripts/install-rootfs.sh` có block cài cả sáu gói một lượt, và từ chối dựng
ảnh nếu thiếu bất kỳ gói nào của cùng một version, hoặc nếu `libvulkan_panfrost.so`
không có mặt trong rootfs sau khi cài. Một ảnh đáng lẽ encode phần cứng mà lại
rơi xuống llvmpipe thì không nói gì cả — phải chặn lúc dựng.

Vì `scripts/build-mesa-package.sh` chạy trên macOS còn `install-rootfs.sh` chạy
trên host x86, sáu file `.deb` được **commit vào `output/debs/`**. Host x86
không tự dựng lại được chúng.

## Đo end-to-end bằng chính gói sẽ ship

Cài sáu gói lên board, cùng một binary GRD dựng từ queue rút gọn (chỉ `0002` +
`0003`, **không** có `0001`), rồi kết nối RDP thật. Chỉ khác nhau ở bản Mesa:

| Mesa | log của daemon phiên |
|---|---|
| `26.0.8-1ubuntu0.3` gốc | `[HWAccel.Vulkan] Could not acquire Vulkan physical device: Could not find proper device` |
| `+orangepi5b1` đã vá | *không còn dòng đó* — đi tiếp tới VAAPI |

Đối chứng lặp 2/2 lần, bản vá 3/3 lần. Qua GNOME Remote Login, trong phiên thật
của người dùng (uid 1000):

```text
v4l2-request: device: encoder at /dev/video4
v4l2-request: device: using /dev/video3 with /dev/media1
v4l2-request: device: using /dev/video6 with /dev/media3
[HWAccel.VAAPI] Successfully initialized VAAPI 1.23 with vendor: v4l2-request
v4l2-request: encode: /dev/video4 1920x1088, 4 raw buffers, 4 coded buffers
```

Phiên chạy liên tục 8 phút, daemon giữ `/dev/video4`, CPU 0.4%. Đây là điều
`spike/mesa-patches/README.md` đã đo bằng bản panfrost tối giản với
`LD_LIBRARY_PATH`; giờ khớp lại bằng gói thật, cài thật, qua đường mà ảnh thật
sự dùng.

Kết quả: patch `0002` **thay thế hoàn toàn** patch `0001` của GRD, và patch đó
đã rút khỏi `config/patches/gnome-remote-desktop-50.2/series`.

### Cái bẫy khi đọc log

"Remote Login" chạy hai daemon nối tiếp. Giai đoạn đầu là màn hình đăng nhập GDM
từ xa, daemon chạy bằng user tạm của GDM (uid 6058x, nhóm `gdm`), không có ACL
`uaccess` cho node codec, nên nó **luôn** in:

```text
v4l2-request: device: no V4L2 stateless H.264/HEVC/VP9 decoder found
```

Đó không phải lỗi và không liên quan Mesa. Phiên thật chỉ bắt đầu sau handover,
dưới uid của người dùng. Đọc log phải nhìn uid của tiến trình.

### Cả hai patch đã đo lại bằng chính `.deb`

`spike/mesa-patches/stage-from-debs.sh` dựng `build/mesa-patches/{stock,patched}`
từ gói thay vì từ `build.sh`, nên hai script `measure-*.sh` chạy nguyên xi mà
vẫn đo đúng thư viện sẽ ship. `stock` lấy từ archive Ubuntu, `patched` lấy từ
`output/debs` — cả hai đều là build đầy đủ, không phải bản panfrost tối giản.

`measure-avatar.sh` — patch `0001`:

| | kích thước PNG | log |
|---|---|---|
| stock | 124027 B | `Gdk-CRITICAL: Failed to download 512x512 dmabuf texture` |
| patched | **13968 B** | không có `Gdk-CRITICAL` |

13968 byte là **đúng con số** năm lần chạy bằng bản tối giản đã cho. Ảnh xem
được: bản gốc là ô nhiễu, bản vá là hình tròn gradient kèm chữ cái đầu.

`measure-modifiers.sh` — patch `0002`, năm format GRD quan tâm:

```text
===== stock                              ===== patched
  B8G8R8A8_UNORM  v1=1 [0x0] v2=0 []       B8G8R8A8_UNORM  v1=1 [0x0] v2=1 [0x0]
  R8G8B8A8_UNORM  v1=1 [0x0] v2=0 []       R8G8B8A8_UNORM  v1=1 [0x0] v2=1 [0x0]
  A2B10G10R10     v1=1 [0x0] v2=0 []       A2B10G10R10     v1=1 [0x0] v2=1 [0x0]
  A2R10G10B10     v1=1 [0x0] v2=0 []       A2R10G10B10     v1=1 [0x0] v2=1 [0x0]
  NV12            v1=1 [0x0] v2=0 []       NV12            v1=1 [0x0] v2=1 [0x0]
```

Trùng khít số đo cũ. Cả hai patch tái hiện nguyên vẹn khi đi qua source package
đầy đủ của Ubuntu — có LLVM, rusticl, NVK — chứ không riêng cây tối giản.

Cả hai đều in `drmPrimeHandleToFD() failed (err=22)` ở cả hai bản: đó là panthor
từ chối export BO nằm trong exclusive VM, patch không định sửa. Khác biệt nằm ở
chỗ tiếp theo — bản vá trả `EGL_FALSE` nên GTK rơi xuống `glReadPixels`.
