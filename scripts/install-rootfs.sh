#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
R="$ROOT/build/rootfs"
[[ $(id -u) == 0 && -f "$R/etc/os-release" ]]
grep -qx 'VERSION_ID="26.04"' "$R/etc/os-release"
source "$ROOT/config/chrome.env"
bash "$ROOT/scripts/fetch-chrome.sh"
cleanup() {
  for p in dev sys proc; do
    if mountpoint -q "$R/$p"; then umount -R "$R/$p"; fi
  done
}
trap cleanup EXIT
mount --rbind /dev "$R/dev"
mount --make-rslave "$R/dev"
mount -t proc proc "$R/proc"
mount -t sysfs sysfs "$R/sys"
rm -f "$R/etc/resolv.conf"
cp -L /etc/resolv.conf "$R/etc/resolv.conf"
cat > "$R/etc/apt/sources.list.d/ubuntu.sources" <<'APT'
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: resolute resolute-updates resolute-security
Components: main universe restricted multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
APT
install -D -m 0644 "$ROOT/config/defaults/ibus-bamboo-archive-keyring.asc" \
  "$R/etc/apt/keyrings/ibus-bamboo.asc"
install -D -m 0644 "$ROOT/config/defaults/ibus-bamboo.sources" \
  "$R/etc/apt/sources.list.d/ibus-bamboo.sources"
cat > "$R/usr/sbin/policy-rc.d" <<'POLICY'
#!/bin/sh
exit 101
POLICY
chmod 755 "$R/usr/sbin/policy-rc.d"
export DEBIAN_FRONTEND=noninteractive
printf '%s\n' \
  'tzdata tzdata/Areas select Etc' \
  'tzdata tzdata/Zones/Etc select UTC' \
  'keyboard-configuration keyboard-configuration/layoutcode string us' \
  'keyboard-configuration keyboard-configuration/modelcode string pc105' |
  chroot "$R" debconf-set-selections
chroot "$R" apt-get update -o APT::Update::Error-Mode=any -o Acquire::Languages=none
mapfile -t packages < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$ROOT/config/boot.packages")
chroot "$R" apt-get --download-only -y --no-install-suggests install "${packages[@]}"
mkdir -p "$ROOT/cache/debs"
shopt -s nullglob
cp "$R"/var/cache/apt/archives/*.deb "$ROOT/cache/debs/" || true
chroot "$R" apt-get -y --no-install-suggests install "${packages[@]}"
if [[ ! -x "$R/usr/lib/ibus-bamboo/ibus-engine-bamboo" ]] ||
   ! grep -q '<name>Bamboo</name>' "$R/usr/share/ibus/component/bamboo.xml"; then
  echo "IBus Bamboo engine is missing or has an unexpected engine ID" >&2
  exit 1
fi
resources_debs=( "$ROOT"/output/debs/resources_*+orangepi5b*.deb )
if ((${#resources_debs[@]})); then
  mapfile -t resources_debs < <(printf '%s\n' "${resources_debs[@]}" | sort -V)
  resources_deb=${resources_debs[-1]}
  install -m 0644 "$resources_deb" "$R/tmp/$(basename "$resources_deb")"
  chroot "$R" apt-get -y --no-install-recommends install "/tmp/$(basename "$resources_deb")"
  rm -f "$R/tmp/$(basename "$resources_deb")"
fi

grd_debs=( "$ROOT"/output/debs/gnome-remote-desktop_*+orangepi5b*.deb )
if ((${#grd_debs[@]})); then
  mapfile -t grd_debs < <(printf '%s\n' "${grd_debs[@]}" | sort -V)
  grd_deb=${grd_debs[-1]}
  install -m 0644 "$grd_deb" "$R/tmp/$(basename "$grd_deb")"
  chroot "$R" apt-get -y --no-install-recommends install "/tmp/$(basename "$grd_deb")"
  rm -f "$R/tmp/$(basename "$grd_deb")"
else
  echo "Missing patched GNOME Remote Desktop package: run scripts/build-grd-package.sh first" >&2
  exit 1
fi

# The VA-API driver for rkvdec. Without it libva finds no backend, Chrome
# silently drops to FFmpegVideoDecoder and nothing in the log says why -- the
# fallback is reported to MediaLog, not stderr. Fail loudly instead: an image
# that is supposed to decode in hardware and cannot is a broken image.
va_driver_debs=( "$ROOT"/output/debs/orangepi5b-va-driver_*_arm64.deb )
if ((${#va_driver_debs[@]})); then
  mapfile -t va_driver_debs < <(printf '%s\n' "${va_driver_debs[@]}" | sort -V)
  va_driver_deb=${va_driver_debs[-1]}
  install -m 0644 "$va_driver_deb" "$R/tmp/$(basename "$va_driver_deb")"
  chroot "$R" apt-get -y --no-install-recommends install "/tmp/$(basename "$va_driver_deb")"
  rm -f "$R/tmp/$(basename "$va_driver_deb")"
else
  echo "Missing VA driver package: run scripts/build-va-driver-package.sh first" >&2
  exit 1
fi

# Remote GDM sessions have no physical seat, therefore logind does not grant
# their desktop user the uaccess ACL attached to V4L2/media devices. The VA
# package carries narrow udev rules which expose only RK3588 codec nodes and
# the two media DMA heaps to the standard `users` group. Refuse to build an
# image where that policy or its target group is missing: Chrome or GRD would
# silently fall back to software.
chroot "$R" getent group users >/dev/null || {
  echo "Required desktop group is missing: users" >&2
  exit 1
}
dma_heap_rules="$R/usr/lib/udev/rules.d/99-orangepi5b-dma-heap.rules"
[[ -s "$dma_heap_rules" ]] || {
  echo "VA driver package lacks remote-session DMA-heap policy" >&2
  exit 1
}
grep -Fq 'KERNEL=="system", GROUP="users", MODE="0660"' \
  "$dma_heap_rules" || {
  echo "VA driver package lacks the system DMA-heap remote-session rule" >&2
  exit 1
}
grep -Fq 'KERNEL=="default_cma_region", GROUP="users", MODE="0660"' \
  "$dma_heap_rules" || {
  echo "VA driver package lacks the CMA DMA-heap remote-session rule" >&2
  exit 1
}
[[ -s "$R/usr/lib/udev/rules.d/99-orangepi5b-media-accelerators.rules" ]] || {
  echo "VA driver package lacks remote-session media-device policy" >&2
  exit 1
}
grep -Fq 'ATTR{name}=="rkvdec", GROUP="users", MODE="0660"' \
  "$R/usr/lib/udev/rules.d/99-orangepi5b-media-accelerators.rules" || {
  echo "VA driver package lacks the rkvdec remote-session rule" >&2
  exit 1
}
grep -Fq 'ATTR{name}=="rockchip,rk3588-av1-vpu-dec", GROUP="users", MODE="0660"' \
  "$R/usr/lib/udev/rules.d/99-orangepi5b-media-accelerators.rules" || {
  echo "VA driver package lacks the AV1 remote-session rule" >&2
  exit 1
}
grep -Fq 'ATTR{name}=="rockchip-rkvenc", GROUP="users", MODE="0660"' \
  "$R/usr/lib/udev/rules.d/99-orangepi5b-media-accelerators.rules" || {
  echo "VA driver package lacks the RKVENC remote-session rule" >&2
  exit 1
}
mapfile -t purge_packages < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$ROOT/config/boot.purge-packages")
if ((${#purge_packages[@]})); then
  chroot "$R" apt-get -y purge "${purge_packages[@]}"
fi
# Minimal board settings for this GNOME boot/display baseline.
# Install the verified, versioned artifact rather than a repository candidate.
chrome_deb="google-chrome-stable_${CHROME_VERSION}_arm64.deb"
install -m 0644 "$ROOT/cache/debs/$chrome_deb" "$R/tmp/$chrome_deb"
chroot "$R" apt-get -y --no-install-recommends install "/tmp/$chrome_deb"
rm -f "$R/tmp/$chrome_deb"
[[ $(chroot "$R" dpkg-query -W '-f=${Version}' google-chrome-stable) == "$CHROME_VERSION" ]]
install -D -m 0755 "$ROOT/config/defaults/google-chrome-orangepi5b" \
  "$R/usr/local/bin/google-chrome-orangepi5b"

# Keep the distro-owned desktop file untouched. /usr/local/share has higher
# XDG precedence, so this same-id override survives a later Chrome package
# update and all three launcher actions keep using the VA-API wrapper.
install -D -m 0644 "$R/usr/share/applications/google-chrome.desktop" \
  "$R/usr/local/share/applications/google-chrome.desktop"
sed -i 's#^Exec=/usr/bin/google-chrome-stable#Exec=/usr/local/bin/google-chrome-orangepi5b#' \
  "$R/usr/local/share/applications/google-chrome.desktop"
chroot "$R" update-alternatives --install /usr/bin/x-www-browser \
  x-www-browser /usr/local/bin/google-chrome-orangepi5b 250
chroot "$R" update-alternatives --install /usr/bin/gnome-www-browser \
  gnome-www-browser /usr/local/bin/google-chrome-orangepi5b 250
chroot "$R" update-alternatives --set x-www-browser \
  /usr/local/bin/google-chrome-orangepi5b
chroot "$R" update-alternatives --set gnome-www-browser \
  /usr/local/bin/google-chrome-orangepi5b
install -D -m 0644 "$ROOT/config/defaults/mimeapps.list" "$R/etc/xdg/mimeapps.list"
# GNOME-specific defaults take precedence over the generic system defaults.
install -D -m 0644 "$ROOT/config/defaults/mimeapps.list" "$R/etc/xdg/gnome-mimeapps.list"
install -D -m 0644 "$ROOT/config/defaults/mimeapps.list" "$R/etc/skel/.config/mimeapps.list"
install -D -m 0644 "$ROOT/config/defaults/dconf-user-profile" "$R/etc/dconf/profile/user"
install -D -m 0644 "$ROOT/config/defaults/ibus-bamboo-input-sources" \
  "$R/etc/dconf/db/local.d/10-ibus-bamboo-input-sources"
install -D -m 0644 "$ROOT/config/defaults/ibus-bamboo-preload" \
  "$R/etc/dconf/db/ibus.d/10-ibus-bamboo-preload"
chroot "$R" dconf update
install -D -m 0644 "$ROOT/config/rootfs/orangepi5b-display.conf" "$R/usr/share/initramfs-tools/modules.d/orangepi5b-display.conf"
install -D -m 0644 "$ROOT/config/rootfs/orangepi5b-ap6275p.conf" "$R/usr/lib/modules-load.d/orangepi5b-ap6275p.conf"
install -D -m 0644 "$ROOT/config/rootfs/bluetooth.service.d/10-orangepi5b-persistent.conf" "$R/etc/systemd/system/bluetooth.service.d/10-orangepi5b-persistent.conf"
for firmware in BCM4362A2.hcd brcmfmac43752-pcie.bin brcmfmac43752-pcie.clm_blob brcmfmac43752-pcie.txt; do
  [[ -s "$ROOT/config/firmware/brcm/$firmware" ]] || { echo "Missing AP6275P firmware: config/firmware/brcm/$firmware"; exit 1; }
  install -D -m 0644 "$ROOT/config/firmware/brcm/$firmware" "$R/usr/lib/firmware/brcm/$firmware"
done
ln -sf brcmfmac43752-pcie.bin "$R/usr/lib/firmware/brcm/brcmfmac43752-pcie.xunlong,orangepi-5b.bin"
ln -sf brcmfmac43752-pcie.txt "$R/usr/lib/firmware/brcm/brcmfmac43752-pcie.xunlong,orangepi-5b.txt"
if [[ ! -e "$R/lib/firmware" ]]; then
  mkdir -p "$R/lib"
  ln -s ../usr/lib/firmware "$R/lib/firmware"
fi
install -D -m 0755 "$ROOT/config/rootfs/orangepi5b-grow-rootfs" "$R/usr/lib/orangepi5b/orangepi5b-grow-rootfs"
install -D -m 0644 "$ROOT/config/rootfs/orangepi5b-grow-rootfs.service" "$R/usr/lib/systemd/system/orangepi5b-grow-rootfs.service"
install -D -m 0644 "$ROOT/config/rootfs/10-orangepi5b-host-keys.conf" "$R/etc/systemd/system/ssh.service.d/10-orangepi5b-host-keys.conf"
install -D -m 0644 "$ROOT/config/defaults/gnome-login.session" "$R/usr/share/gdm/greeter/gnome-session/sessions/gnome-login.session"
# Networking and identity.
mkdir -p "$R/etc/netplan" "$R/etc/ssh/sshd_config.d" "$R/etc/gdm3" "$R/etc/systemd/system/ssh.service.d" "$R/etc/systemd/journald.conf.d" "$R/var/log/journal"
cat > "$R/etc/netplan/01-network-manager.yaml" <<'NETPLAN'
network:
  version: 2
  renderer: NetworkManager
NETPLAN
chmod 600 "$R/etc/netplan/01-network-manager.yaml"
printf 'PasswordAuthentication yes\nPermitRootLogin no\n' > "$R/etc/ssh/sshd_config.d/00-boot-access.conf"
cat > "$R/etc/gdm3/custom.conf" <<'GDM'
# GDM configuration storage

[daemon]
InitialSetupEnable=true

[security]

[debug]
GDM
rm -f "$R"/etc/ssh/ssh_host_*_key "$R"/etc/ssh/ssh_host_*_key.pub
printf '[Journal]\nStorage=persistent\nSystemMaxUse=128M\n' > "$R/etc/systemd/journald.conf.d/20-boot-diagnostics.conf"
install -D -m 0755 "$ROOT/config/diagnostics/boot-status" "$R/usr/local/sbin/orangepi5b-boot-status"
install -D -m 0644 "$ROOT/config/diagnostics/boot-status.service" "$R/etc/systemd/system/orangepi5b-boot-status.service"
install -D -m 0644 "$ROOT/config/diagnostics/boot-status.timer" "$R/etc/systemd/system/orangepi5b-boot-status.timer"
remove_colon_file_user() {
  local file="$1"
  local user="$2"

  [[ -f "$file" ]] || return 0
  awk -F: -v user="$user" '$1 != user { print }' "$file" > "$file.tmp"
  cat "$file.tmp" > "$file"
  rm -f "$file.tmp"
}

remove_colon_file_group_user() {
  local file="$1"
  local user="$2"

  [[ -f "$file" ]] || return 0
  awk -F: -v user="$user" '
    BEGIN { OFS = FS }
    $1 == user { next }
    {
      count = split($4, members, ",")
      filtered = ""
      for (i = 1; i <= count; i++) {
        if (members[i] != "" && members[i] != user) {
          filtered = filtered (filtered == "" ? "" : ",") members[i]
        }
      }
      $4 = filtered
      print
    }
  ' "$file" > "$file.tmp"
  cat "$file.tmp" > "$file"
  rm -f "$file.tmp"
}

remove_embedded_default_user() {
  local user=ubuntu

  if chroot "$R" id "$user" >/dev/null 2>&1; then
    chroot "$R" userdel -r "$user" 2>/dev/null || true
  fi

  remove_colon_file_user "$R/etc/passwd" "$user"
  remove_colon_file_user "$R/etc/shadow" "$user"
  remove_colon_file_user "$R/etc/subuid" "$user"
  remove_colon_file_user "$R/etc/subgid" "$user"
  remove_colon_file_group_user "$R/etc/group" "$user"
  remove_colon_file_group_user "$R/etc/gshadow" "$user"

  rm -f "$R/var/lib/AccountsService/users/$user"
  if [[ -d "$R/home/$user" ]]; then
    chown -R 0:0 "$R/home/$user" 2>/dev/null || true
    chmod -R u+rwX "$R/home/$user" 2>/dev/null || true
    rm -rf "$R/home/$user"
  fi
}

remove_embedded_default_user
find "$R/etc/skel" "$R/var/lib" "$R/home" -xdev -name gnome-initial-setup-done -delete 2>/dev/null || true
if [[ ! -x "$R/usr/libexec/gnome-initial-setup" ]]; then
  echo "GNOME Initial Setup is missing from the root filesystem"
  exit 1
fi
if awk -F: '$3 >= 1000 && $3 < 60000 { found = 1 } END { exit found ? 0 : 1 }' "$R/etc/passwd"; then
  echo "A regular user account was unexpectedly embedded in the image"
  exit 1
fi
chroot "$R" systemctl set-default graphical.target
chroot "$R" systemctl enable gdm3 NetworkManager ssh.service bluetooth.service orangepi5b-boot-status.timer orangepi5b-grow-rootfs.service || true
chroot "$R" dpkg-query -W '-f=${binary:Package}\t${Version}\n' > "$ROOT/output/packages.tsv"
# The host keeps downloaded debs in cache/debs. Do not ship apt build caches
# inside the runtime image; compressed .deb files barely shrink further in .xz.
chroot "$R" apt-get clean
rm -rf "$R/var/cache/apt/archives/partial"/* "$R/var/lib/apt/lists"/*
rm -f "$R/var/cache/apt/pkgcache.bin" "$R/var/cache/apt/srcpkgcache.bin"
rm -f "$R/usr/sbin/policy-rc.d" "$R/usr/bin/qemu-aarch64-static"
: > "$R/etc/machine-id"
rm -f "$R/var/lib/dbus/machine-id"
ln -sf /etc/machine-id "$R/var/lib/dbus/machine-id"
rm -f "$R/etc/resolv.conf"
ln -s ../run/systemd/resolve/stub-resolv.conf "$R/etc/resolv.conf"
