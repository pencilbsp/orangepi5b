#!/usr/bin/env bash
# Host side. Asks panvk for DRM format modifiers both ways, against each Mesa
# build from build.sh, and prints the two counts side by side.
#
# Verifies config/patches/mesa-26.0.8/0002. Before it, the v2 query answers
# zero for every format and gnome-remote-desktop concludes the GPU supports no
# modifiers at all; after it, v2 answers the same as v1.
#
# Nothing on the board changes: the driver is selected per-process with
# VK_DRIVER_FILES pointing at an ICD manifest inside the copied build, so
# /usr/share/vulkan stays untouched.
#
# Runs as whoever owns seat0, because only that account has an ACL on
# /dev/dri/renderD128 -- over SSH there is no seat. No display is needed; this
# only enumerates a physical device.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="$ROOT/build/mesa-patches"
REMOTE=/tmp/mesa-ab-vk

for v in stock patched; do
    [[ -e "$BUILD/$v/libvulkan_panfrost.so" ]] || {
        echo "Missing $BUILD/$v/libvulkan_panfrost.so -- run build.sh first"; exit 1; }
done
[[ -x "$BUILD/modifier-probe" ]] || { echo "Missing $BUILD/modifier-probe"; exit 1; }

# shellcheck source=lib-board.sh
source "$(dirname "$0")/lib-board.sh"
resolve_seat_session

sudo_board <<EOF
rm -rf $REMOTE && mkdir -p $REMOTE && chown $USER:$USER $REMOTE
EOF
scp_board -r "$BUILD/stock" "$BUILD/patched" "$BUILD/modifier-probe" \
    "$USER@$HOST:$REMOTE/"
ssh_board "chmod -R a+rX $REMOTE && chmod a+x $REMOTE/modifier-probe"

for variant in stock patched; do
    echo
    echo "===== $variant"
    sudo_board <<EOF
sudo -u $SEAT_USER env \
  VK_DRIVER_FILES=$REMOTE/$variant/panvk_icd.json \
  VK_ICD_FILENAMES=$REMOTE/$variant/panvk_icd.json \
  LD_LIBRARY_PATH=$REMOTE/$variant HOME=$REMOTE \
  XDG_RUNTIME_DIR=/run/user/$SEAT_UID \
  timeout 60 $REMOTE/modifier-probe
EOF
done

echo
echo "v1 = VkDrmFormatModifierPropertiesListEXT (what panvk always answered)"
echo "v2 = VkDrmFormatModifierPropertiesList2EXT (what GRD 50 actually asks)"
