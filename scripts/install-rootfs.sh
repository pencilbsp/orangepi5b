#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
R="$ROOT/build/rootfs"
[[ $(id -u) == 0 && -f "$R/etc/os-release" ]]
grep -qx 'VERSION_ID="26.04"' "$R/etc/os-release"
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
mapfile -t purge_packages < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$ROOT/config/boot.purge-packages")
if ((${#purge_packages[@]})); then
  chroot "$R" apt-get -y purge "${purge_packages[@]}"
fi
# Minimal board settings for this GNOME boot/display baseline.
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
