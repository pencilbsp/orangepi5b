# Shared helpers for building arm64 Debian packages in a chroot on the build
# host, through binfmt_misc + qemu-aarch64 -- the same mechanism
# install-rootfs.sh uses. Source this; it expects $C to name the chroot.
#
# Not executable on its own.

BASE_TARBALL="${BASE_TARBALL:-cache/sources/ubuntu-base-26.04.1-base-arm64.tar.gz}"

require_host_prereqs() {
 [[ $(id -u) == 0 ]] || { echo "must run as root"; exit 1; }
 [[ -s "$ROOT/$BASE_TARBALL" ]] || {
  echo "Missing base tarball: $ROOT/$BASE_TARBALL"; exit 1; }
 # procfs reports every entry as zero length, so test content, not size.
 grep -qx enabled /proc/sys/fs/binfmt_misc/qemu-aarch64 2>/dev/null || {
  echo "binfmt_misc has no enabled qemu-aarch64 handler; install qemu-user-static"
  exit 1; }
 for c in dpkg-source dpkg-deb tar findmnt mountpoint; do
  command -v "$c" >/dev/null || { echo "Missing required command: $c"; exit 1; }
 done
}

# A bind mount is the host filesystem, not a copy. Never let rm descend into a
# chroot until every mount below it is gone and that has been verified.
chroot_mounts() {
 findmnt -rn -o TARGET | awk -v c="$C" '$0 == c || index($0, c "/") == 1'
}

unmount_chroot() {
 local -a t=()
 mapfile -t t < <(chroot_mounts)
 for ((i=${#t[@]}-1; i>=0; i--)); do umount -l -- "${t[i]}" || return 1; done
 [[ -z "$(chroot_mounts)" ]] || { echo "mounts remain under $C"; return 1; }
}

mount_api() {
 mkdir -p "$C/dev" "$C/proc" "$C/sys"
 mountpoint -q "$C/dev" || { mount --rbind /dev "$C/dev"; mount --make-rslave "$C/dev"; }
 mountpoint -q "$C/proc" || mount -t proc proc "$C/proc"
 mountpoint -q "$C/sys" || mount -t sysfs sysfs "$C/sys"
}

# Raise the chroot and install build dependencies for one source package.
# Marked ready so later runs go straight to compiling.
prepare_chroot() {
 local src_package="$1"
 shift
 local marker="$C/.chroot-ready"

 if [[ -e "$marker" ]]; then
  mount_api
  return 0
 fi

 unmount_chroot
 rm -rf "$C"
 mkdir -p "$C"
 tar -xpf "$ROOT/$BASE_TARBALL" -C "$C"
 rm -f "$C/etc/resolv.conf"; cp -L /etc/resolv.conf "$C/etc/resolv.conf"
 cat > "$C/etc/apt/sources.list.d/ubuntu.sources" <<'APT'
Types: deb deb-src
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: resolute resolute-updates resolute-security
Components: main universe restricted multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
APT
 printf '#!/bin/sh\nexit 101\n' > "$C/usr/sbin/policy-rc.d"
 chmod 755 "$C/usr/sbin/policy-rc.d"

 mount_api
 chroot "$C" dpkg --print-architecture | grep -Fqx arm64
 export DEBIAN_FRONTEND=noninteractive
 chroot "$C" apt-get update -o Acquire::Languages=none
 chroot "$C" apt-get install -y --no-install-recommends \
  build-essential fakeroot devscripts dpkg-dev quilt "$@"
 chroot "$C" apt-get build-dep -y "$src_package"
 touch "$marker"
}

# Fetch inside the chroot, because only it has arm64 deb-src; unpack outside,
# with the host's own dpkg-source. GNU tar's extraction path fails under
# qemu-aarch64 with ENOSYS on the first nested mkdir, and a source tree is
# architecture-independent, so nothing is gained by emulating the unpack.
fetch_and_unpack_source() {
 local src_package="$1" dsc="$2"
 rm -rf "$C/build"
 mkdir -p "$C/build"
 chroot "$C" sh -c "cd /build && apt-get source --download-only $src_package"
 (cd "$C/build" && dpkg-source --no-check -x "$dsc")
}

# Add our patches to the Ubuntu queue rather than replacing it.
queue_patches() {
 local src_dir="$1" patch_dir="$2" name
 while IFS= read -r name; do
  [[ -n "$name" && "$name" != \#* ]] || continue
  [[ -f "$patch_dir/$name" ]] || { echo "Patch missing: $patch_dir/$name"; exit 1; }
  install -m 0644 "$patch_dir/$name" "$C$src_dir/debian/patches/$name"
  printf '%s\n' "$name" >> "$C$src_dir/debian/patches/series"
  echo "queued patch: $name"
 done < "$patch_dir/series"
}
