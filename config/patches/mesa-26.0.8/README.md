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

## Chưa build được bằng source package của Ubuntu

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

### Đường đi khả dĩ, chưa làm

Không cài `llvm-21-dev:arm64` bằng apt mà **giải nén** nó vào chroot bằng
`dpkg-deb -x` (đúng kỹ thuật repo vẫn dùng cho sysroot arm64), để không kéo
theo `llvm-21-tools:arm64`. Mesa tìm LLVM qua cả `cmake` lẫn `config-tool`
(`meson.build:1880`, `1941`), nên có thể trỏ vào file CMake — vốn là text,
không phải binary arm64 phải chạy được.

Cộng thêm: stage build native riêng cho `mesa_clc` / `vtn_bindgen2` /
`panfrost_compile` (đã chứng minh chạy được trong `spike/mesa-patches/build.sh`),
rồi patch `debian/rules` thêm `-Dmesa-clc=system -Dprecomp-compiler=system`.

Đây là một dự án build-system nhiều bước, chưa làm.

## Ghi chú: quyết định này ngược với lần trước

`config/patches/gnome-remote-desktop-50.2/README.md` đã từng ghi rõ **chọn
không ship bản Mesa vá**, vì compile Mesa tốn kém mỗi lần rebase còn patch
phía consumer thì rẻ hơn nhiều. Lần này khác ở chỗ lỗi nằm ngay trong EGL
core chứ không phải trong panvk, và nó làm hỏng dữ liệu một cách im lặng cho
*mọi* consumer EGL, không riêng một app. Một patch 2 dòng phía GTK
(`gdk/gdkglcontext.c`: khởi tạo `fds[]` bằng -1 rồi từ chối `fds[0] < 0`) cũng
cho ra avatar đúng và rẻ hơn khi rebase — nếu chi phí rebuild Mesa là vấn đề
thì đó là lựa chọn thay thế hợp lệ.

## Chưa build được bằng source package của Ubuntu

`apt-get build-dep -a arm64 -P nocheck mesa` không giải được, và không phải vì
build profile như trường hợp `gnome-remote-desktop`. `debian/control` của mesa
không đánh dấu `:native` cho host tool nào, nên apt đòi cài bản arm64 của
chúng. Hai xung đột cứng, không lách được từ bên ngoài packaging:

```text
bindgen:amd64 → libclang-21-dev:amd64, mà nó Conflicts libclang-21-dev:arm64
llvm-21-dev:arm64 → llvm-21-tools:arm64 → python3-yaml:arm64,
  mà nó Conflicts python3-yaml bản native — thứ script của chính mesa cần
```

Cả hai đều không liên quan gì tới patch: code được vá nằm trong
`libEGL_mesa.so` (EGL core), không dính llvm, rusticl hay driver nào.

Nên việc kiểm chứng đi qua `spike/mesa-patches/build.sh`: cross-build một
bản Mesa tối giản chỉ có panfrost + EGL, **hai lần** — một bản gốc, một bản đã
vá — rồi chạy A/B trên board bằng `LD_LIBRARY_PATH`, không đụng gì vào Mesa của
board.

Muốn thực sự ship patch này trong ảnh thì vẫn còn việc phải làm: sửa
`debian/control` để cross-build được (thêm `:native`), hoặc build mesa native
trên máy arm64 khác. Đó là lý do patch phía GTK nêu ở trên đáng cân nhắc —
`gtk4` cross-build được bằng source package của Ubuntu.

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

## Bước kế tiếp: build native trên arm64, không cross

Quyết định: dựng gói này trong **container Linux arm64 trên máy Apple Silicon**,
không cross-build nữa. Nút thắt ở trên là hệ quả của multiarch — một apt
universe có cả amd64 lẫn arm64. Trên máy arm64 chỉ có một kiến trúc, nên
`apt-get build-dep mesa` giải bình thường: không cần `:native`, không xung đột
libclang, không ai hất văng `python3`. `debian/` của Ubuntu giữ nguyên, không
phải vá. Đây đúng là cách buildd của Ubuntu build mesa.

Quy tắc trong `CLAUDE.md` cấm **emulation**, với lý do "không có lý do gì phải
emulate chính compiler". Trên Apple Silicon compiler chạy native arm64 và sinh
mã arm64 — thoả đúng nguyên tắc đó. Nhưng câu chữ hiện tại ("Mọi thứ phải build
bằng cross-compilation trên host x86") không phủ trường hợp này, **cần sửa
CLAUDE.md** kẻo phiên sau đọc thành cấm.

Các bước, chạy trong container:

```bash
# Phải là arm64 THẬT, không phải amd64 dưới Rosetta -- Rosetta là emulation,
# đúng cái quy tắc cấm. Kiểm tra trước khi làm gì khác:
docker run --platform linux/arm64 -it --rm -v "$PWD/out:/out" ubuntu:26.04 bash
uname -m            # phải ra aarch64
dpkg --print-architecture   # phải ra arm64

apt-get update && apt-get install -y devscripts dpkg-dev
sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources
apt-get update
apt-get build-dep -y mesa
apt-get source mesa=26.0.8-1ubuntu0.3
cd mesa-26.0.8
# nối hai patch vào CUỐI series của Ubuntu, không thay
cp /path/to/config/patches/mesa-26.0.8/000*.patch debian/patches/
cat /path/to/config/patches/mesa-26.0.8/series >> debian/patches/series
dch --local +orangepi5b 'egl/dri2 + panvk fixes for RK3588.'
DEB_BUILD_OPTIONS="parallel=$(nproc) nocheck" DEB_BUILD_PROFILES=nocheck \
  dpkg-buildpackage -b -uc -us
cp ../*.deb /out/
```

Mang về **cả sáu** gói, không lấy lẻ được vì chúng ràng buộc nhau bằng
`mesa-libgallium (= ${binary:Version})`:

```
libegl-mesa0  libgbm1  libgl1-mesa-dri  libglx-mesa0  mesa-libgallium
mesa-vulkan-drivers
```

`scripts/install-rootfs.sh` gom `.deb` theo từng họ gói (dòng 56–93 cho
resources / grd / va-driver); mesa cần thêm một block tương tự.
