# ubuntu-orangepi5b

Ảnh Ubuntu 26.04 arm64 cho Orange Pi 5B (RK3588S). Build trên máy x86 —
trừ mesa, xem ngoại lệ ở dưới.

## Quy tắc bắt buộc: cross-compile, không emulate

**Mọi thứ phải build bằng cross-compilation trên host x86. Không chạy compiler
dưới `qemu-user`.**

```bash
dpkg-buildpackage -aarm64 -b -uc -us     # gói Debian
CROSS_COMPILE=aarch64-linux-gnu-         # kernel, u-boot
```

Emulation làm chết thời gian một cách vô nghĩa: cùng một cây Mesa mất **~80
phút** trong chroot arm64 dưới qemu, trong khi cross-build ước tính 8–10 phút.
Compiler chạy native x86 và chỉ *sinh ra* mã arm64 — không có lý do gì phải
emulate chính compiler.

Đo thật: `gnome-remote-desktop` cross-build xong trong khoảng **90 giây**, so
với **~60 phút** trong chroot arm64 dưới qemu (phần lớn là cài build-dep).
Cùng một `.deb`, cùng `debian/` của Ubuntu.

Nếu cross-build có vẻ không được, gần như chắc là do một trong hai cạm bẫy dưới
đây chứ không phải do gói đó "không cross được".

Đây không phải sở thích. Toàn bộ pipeline của dự án trước đã theo lối này:
kernel và u-boot cross-compile, và **không thứ gì lớn từng được compile dưới
emulation**. Đi ngược lại là tự chuốc lấy thời gian chết.

### Khi nào được dùng chroot arm64 + qemu

Cho việc **không phải compile**:

- dựng rootfs, cài gói bằng `apt` (`scripts/install-rootfs.sh`)
- chạy maintainer script của gói
- kiểm tra thứ gì đó *bên trong* hệ thống đích

Và cho **ngoại lệ đã kiểm chứng dưới đây**. Ngoài hai trường hợp đó, nếu thấy
mình đang chờ `cc1`/`cc1plus` chạy dưới qemu thì đã đi sai đường.

### Ngoại lệ: build native trên máy arm64 thật (mesa)

Quy tắc trên cấm **emulation**, không cấm "không-cross". Lý do của nó là "không
có lý do gì phải emulate chính compiler". Trên một máy arm64 thật — Apple
Silicon — compiler *là* binary arm64 và sinh mã arm64: không có emulation nào
cả, nên lối này thoả đúng nguyên tắc dù không phải cross-build.

Chỉ **mesa** đi đường này, và vì một lý do cụ thể: build-dep arm64 của nó không
giải được trong một apt universe có hai kiến trúc.

```text
llvm-21-dev:arm64 → llvm-21-tools:arm64 → python3-yaml:arm64 → python3:arm64
```

mà `python3:amd64 Conflicts python3:arm64`, nên kéo LLVM cho target sẽ hất văng
chính thông dịch viên native mà script sinh mã của mesa chạy trên đó. Nút nằm
trong packaging của `llvm-21-tools` và `python3-yaml`, không phải của mesa, nên
thêm `:native` vào `debian/control` của mesa không gỡ được. Và không bỏ LLVM đi
được: ảnh cần llvmpipe. Dẫn giải đầy đủ:
`config/patches/mesa-26.0.8/README.md`.

Trên host arm64 chỉ có **một** kiến trúc trong apt universe, nên
`apt-get build-dep mesa` giải bình thường, `debian/` của Ubuntu giữ nguyên
không phải vá gì. Đây đúng là cách buildd của Ubuntu build mesa.

```bash
brew install container            # container CLI của Apple
container system kernel set --recommended
bash scripts/build-mesa-package.sh
```

`scripts/build-mesa-package.sh` chạy **trên macOS**, không chạy trên host x86 —
mọi script khác trong `scripts/` thì ngược lại. Nó từ chối chạy nếu guest không
phải `aarch64`/`arm64`: amd64 dưới Rosetta **là** emulation, đúng thứ quy tắc
này cấm.

Đây không phải giấy phép chung để bỏ cross-build. Gói nào cross-build được thì
vẫn phải cross-build; ngoại lệ này chỉ dành cho gói chứng minh được là bế tắc
multiarch, và phải ghi lại bế tắc đó như mesa đã ghi.

### Cạm bẫy cross-build: build profile

`dpkg-buildpackage -aarm64` cho `gnome-remote-desktop` **chạy được**, nhưng lần
đầu thử thì chết ở khâu build-dep:

```text
mutter:arm64 Depends gnome-settings-daemon-common:arm64
   but none of the choices are installable: [no choices]
```

Dễ kết luận nhầm là "stack GNOME không cross-build được" — **sai**. Nhìn
`debian/control` thì thấy:

```
mutter (>= 49~) <!nocheck>,
dbus-daemon <!nocheck>, pipewire <!nocheck>, wireplumber <!nocheck>,
python3-gi <!nocheck>, python3-dbus <!nocheck>, openssl <!nocheck>
```

`<!nocheck>` là **build profile**: những gói này chỉ cần để chạy test suite.
Bật profile thì apt bỏ qua chúng hoàn toàn, và toàn bộ resolution thông.

**`DEB_BUILD_OPTIONS=nocheck` KHÔNG làm việc này.** Build profile là cơ chế
riêng, phải truyền riêng:

```bash
apt-get build-dep -a arm64 -P nocheck <pkg>
DEB_BUILD_PROFILES=nocheck dpkg-buildpackage -aarm64 -b -uc -us
```

Kiểm tra rẻ trước khi kết luận một gói có cross được không:

```bash
chroot "$C" apt-get build-dep -s -a arm64 -P nocheck <pkg>   # -s: mô phỏng
```

### Cạm bẫy cross-build: apt trong chroot

apt hạ quyền xuống `_apt` và không ghi được vào chroot vừa giải nén — mọi lượt
tải fail với `Could not open file .../partial/*.deb`. Luôn truyền:

```bash
-o APT::Sandbox::User=root
```

Project trước cũng đã ghi lại đúng bẫy này. Và mount `/dev/pts`, nếu không apt
kêu `Can not write log (Is /dev/pts mounted?)` mỗi lần chạy.

## Board thử nghiệm

Thông tin kết nối trong `target.json` (gitignored, có mật khẩu — không in ra
log, không commit).

Board chạy **đúng** stack mà repo này build, nên dùng để *đo* thay vì suy đoán.
Nhưng nó là ảnh slim: **không có compiler, không header, không `vulkaninfo`**.
Cross-build trên host rồi `scp` sang.

**Board không phải máy build.** 7.8GB RAM, không swap. Chỉ dùng board để chạy và đo.

Chạy qua SSH thì không có seat, nên `/dev/dri/card*` báo Permission denied
trong khi `renderD128` vẫn dùng được — bình thường, không phải lỗi.

## Cấu trúc

- `config/patches/linux-7.1.8/` — queue patch kernel, áp theo `series`
- `config/patches/gnome-remote-desktop-50.2/` — ba patch để tới được RKVENC và giữ visible size đúng
- `config/patches/mesa-26.0.8/` — hai patch EGL/panvk, build native arm64 chứ không cross
- `scripts/lib/arm64-chroot.sh` — helper chroot dùng chung
- `docs/GRD-VAAPI-ENCODE-PLAN.md` — kế hoạch hardware encode/decode
- `spike/` — điều tra, không thuộc ảnh

## Nguyên tắc khi vá

Patch cho phần mềm bên thứ ba phải ghi rõ **điều kiện để bỏ nó đi**. Cả ba
patch GRD hiện tại là workaround cho bug thượng nguồn, không phải tính năng của
board; README của queue nói rõ cái gì phải xảy ra thì mới gỡ được.

Áp patch bằng cách **thêm** vào `debian/patches/series` của Ubuntu, không thay
thế. Giữ `debian/` của Ubuntu nguyên vẹn.
