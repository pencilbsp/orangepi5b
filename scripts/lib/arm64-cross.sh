# Shared helpers for CROSS-building arm64 Debian packages on the x86 host.
#
# The compiler runs natively and only emits arm64 code, so nothing is emulated
# and builds run at host speed. See CLAUDE.md: this is the required path for
# anything that compiles. The arm64 chroot in arm64-chroot.sh is for work that
# is not compilation.
#
# The chroot here is x86: it exists only because the host's own dpkg is in a
# broken state and cannot take `dpkg --add-architecture arm64`. Source this; it
# expects $C to name the chroot.

CROSS_BASE_TARBALL="${CROSS_BASE_TARBALL:-cache/sources/ubuntu-base-26.04.1-base-amd64.tar.gz}"

require_cross_prereqs() {
 [[ $(id -u) == 0 ]] || { echo "must run as root"; exit 1; }
 [[ -s "$ROOT/$CROSS_BASE_TARBALL" ]] || {
  echo "Missing base tarball: $ROOT/$CROSS_BASE_TARBALL"; exit 1; }
 for c in tar findmnt mountpoint dpkg-deb; do
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
 # apt wants a pty for its log, and warns on every invocation without one.
 mkdir -p "$C/dev/pts"
 mountpoint -q "$C/dev/pts" || mount -t devpts devpts "$C/dev/pts" 2>/dev/null || true
}

# Raise an x86 chroot that can cross-build for arm64, and install the arm64
# build dependencies of one source package.
# prepare_cross_chroot [source-package] [extra packages...]
#
# Pass "-" as the source package to skip apt-get build-dep entirely: a project
# that is not a Debian source package has no build-deps to resolve, and asking
# for an unrelated package's would drag in half a distribution.
prepare_cross_chroot() {
 local src_package="$1"
 shift || true
 local marker="$C/.cross-chroot-ready"

 if [[ -e "$marker" ]]; then
  mount_api
  return 0
 fi

 unmount_chroot
 rm -rf "$C"
 mkdir -p "$C"
 tar -xpf "$ROOT/$CROSS_BASE_TARBALL" -C "$C"
 rm -f "$C/etc/resolv.conf"; cp -L /etc/resolv.conf "$C/etc/resolv.conf"

 # amd64 packages come from archive.ubuntu.com, arm64 ones from ports; a
 # multiarch setup that points both at one host fails to find half of them.
 cat > "$C/etc/apt/sources.list.d/ubuntu.sources" <<'APT'
Types: deb deb-src
URIs: http://archive.ubuntu.com/ubuntu
Suites: resolute resolute-updates resolute-security
Components: main universe restricted multiverse
Architectures: amd64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb deb-src
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: resolute resolute-updates resolute-security
Components: main universe restricted multiverse
Architectures: arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
APT
 printf '#!/bin/sh\nexit 101\n' > "$C/usr/sbin/policy-rc.d"
 chmod 755 "$C/usr/sbin/policy-rc.d"

 mount_api
 # apt drops privileges to _apt, which cannot write into a freshly unpacked
 # chroot: downloads fail with "Could not open file .../partial/*.deb". Keep
 # apt as root inside the chroot.
 mkdir -p "$C/var/cache/apt/archives/partial" "$C/var/lib/apt/lists/partial"
 APT_OPTS="-o APT::Sandbox::User=root -o Acquire::Languages=none"
 chroot "$C" dpkg --print-architecture | grep -Fqx amd64
 chroot "$C" dpkg --add-architecture arm64
 export DEBIAN_FRONTEND=noninteractive
 chroot "$C" apt-get update $APT_OPTS
 chroot "$C" apt-get install -y $APT_OPTS --no-install-recommends \
  build-essential crossbuild-essential-arm64 fakeroot devscripts dpkg-dev quilt \
  "$@"
 # -a arm64 pulls the target-architecture -dev packages plus the cross toolchain
 # rather than the native ones.
 #
 # -P nocheck is load-bearing, not an optimisation. Build-Depends carrying the
 # <!nocheck> profile -- for gnome-remote-desktop that is mutter, dbus-daemon,
 # pipewire, wireplumber, python3-gi, python3-dbus, openssl -- exist only to run
 # the test suite. Without the profile apt tries to install them for arm64, and
 # mutter:arm64 is not installable because gnome-settings-daemon-common is
 # Architecture: all without Multi-Arch: foreign, so the whole resolution fails.
 # DEB_BUILD_OPTIONS=nocheck does NOT do this; build profiles are a separate
 # mechanism.
 if [[ "$src_package" != "-" ]]; then
  chroot "$C" apt-get build-dep -y $APT_OPTS -a arm64 \
   -P "${BUILD_PROFILES:-nocheck}" "$src_package"
 fi
 touch "$marker"
}

# Source trees are architecture-independent, and the host has dpkg-source, so
# fetch and unpack both happen natively.
fetch_and_unpack_source_cross() {
 local src_package="$1" dsc="$2"
 rm -rf "$C/build"
 mkdir -p "$C/build"
 chroot "$C" sh -c "cd /build && apt-get source --download-only -o APT::Sandbox::User=root $src_package"
 chroot "$C" sh -c "cd /build && dpkg-source --no-check -x $dsc"
}

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
