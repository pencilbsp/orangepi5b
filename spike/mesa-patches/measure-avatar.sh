#!/usr/bin/env bash
# Host side. Runs the generated-avatar path on the board against both Mesa
# builds from build.sh and reports which one produces a real avatar.
#
# Nothing on the board is modified: the two builds are copied to /tmp and
# selected per-process with LD_LIBRARY_PATH and LIBGL_DRIVERS_PATH. The board's
# own Mesa keeps serving the compositor throughout.
#
# The test has to run inside a session that has the GPU. Over SSH there is no
# seat, so /dev/dri/renderD128 carries an ACL only for whoever owns seat0 --
# gdm-greeter at the login screen, the logged-in user afterwards. Either one is
# the same stack gnome-initial-setup used when it wrote the bad avatar.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="$ROOT/build/mesa-patches"
REPRO="$(dirname "$0")/avatar-repro.py"
REMOTE=/tmp/mesa-ab

for v in stock patched; do
    [[ -e "$BUILD/$v/libEGL_mesa.so.0.0.0" ]] || {
        echo "Missing $BUILD/$v -- run build.sh first"; exit 1; }
done

# shellcheck source=lib-board.sh
source "$(dirname "$0")/lib-board.sh"
resolve_seat_session

# HOME points here during the run, so gdm-greeter leaves root-owned dot-dirs
# behind; clearing it needs sudo.
sudo_board <<EOF
rm -rf $REMOTE && mkdir -p $REMOTE && chown $USER:$USER $REMOTE
EOF
scp_board -r "$BUILD/stock" "$BUILD/patched" "$REPRO" "$USER@$HOST:$REMOTE/"
ssh_board "chmod -R a+rX $REMOTE && chmod a+w $REMOTE"

for variant in stock patched; do
    echo
    echo "===== $variant"
    sudo_board <<EOF
sudo -u $SEAT_USER env \
  XDG_RUNTIME_DIR=/run/user/$SEAT_UID WAYLAND_DISPLAY=$SEAT_DISPLAY \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$SEAT_UID/bus \
  GDK_BACKEND=wayland GSK_DEBUG=renderer HOME=$REMOTE \
  LD_LIBRARY_PATH=$REMOTE/$variant LIBGL_DRIVERS_PATH=$REMOTE/$variant/dri \
  timeout 90 python3 $REMOTE/avatar-repro.py $REMOTE/out-$variant.png 2>&1 |
  grep -iE "renderer|drmPrime|CRITICAL|WROTE|ERROR"
ls -l $REMOTE/out-$variant.png 2>/dev/null || echo "no PNG produced"
EOF
done

mkdir -p "$BUILD/result"
scp_board "$USER@$HOST:$REMOTE/out-*.png" "$BUILD/result/" 2>/dev/null || true
echo
echo "PNGs: $BUILD/result"
ls -l "$BUILD/result" 2>/dev/null || true
echo
echo "A correct avatar is ~14 KB. ~2 KB means the download left the buffer"
echo "zeroed; ~130 KB means it left whatever was in that memory."
