# gnome-session 50.1 — bảo vệ handoff Remote Login

Queue này áp thêm lên source package Ubuntu
`gnome-session_50.1-0ubuntu0.1`. Artifact cần cài là `gnome-session-bin`; các
gói `gnome-session-common`, `gnome-session` và `ubuntu-session` không chứa mã
đã sửa.

## `0001` — không restart user D-Bus khi còn session khác

`gnome-session-restart-dbus.service` dọn các D-Bus service còn sót lại bằng
cách dừng `dbus.service` sau logout. D-Bus này thuộc systemd user manager và
được dùng chung cho mọi session của cùng UID.

GDM Remote Login 50 mở PAM session mới trước khi hiện hộp thoại
`Session Already Running`. Nếu chọn `Force Stop`, phiên local cũ chạy shutdown
target rồi dừng user D-Bus dù phiên remote mới vẫn tồn tại. Log thực tế trên
board cho thấy:

- logind xoá session local lúc `21:44:35.496`;
- shutdown dừng `dbus.service` lúc `21:44:35.563`;
- PAM session remote đóng lúc `21:44:35.583`;
- GDM báo `Session never registered, failing` lúc `21:44:35.585`.

Patch hỏi logind bằng `sd_uid_get_sessions()` trước khi dừng bus. Nếu UID còn
session đăng nhập khác thì bus phải được giữ lại; nếu không còn session, hành
vi logout thông thường không đổi. Hai class tổng hợp `manager` và
`manager-early` của chính systemd user manager bị loại khỏi phép đếm. Nếu truy
vấn logind lỗi, helper giữ hành vi upstream và vẫn restart bus.

**Điều kiện bỏ patch:** gnome-session upstream không còn restart D-Bus dùng
chung khi một session khác của cùng UID vẫn tồn tại, hoặc GDM đổi handoff để
không giữ một PAM session đã xác thực song song với shutdown của phiên cũ.

## Build

```bash
sudo scripts/build-gnome-session-package.sh
```
