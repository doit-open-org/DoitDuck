#!/bin/sh
# A Cove-610 flash image with DoitDuck in it, started at boot.
#
# Takes a V610-Buildroot that has been built (output/target, output/host,
# output/images/flash) and leaves it as it is: its root filesystem is copied,
# DoitDuck added to the copy (/root/doitduck, S40app's APP_PATH, S52wifi), and
# the UBI image made again the way Buildroot makes it. The U-Boot environment
# boots with 'sdio1-pins none', which the servo bus needs, and no boot delay. Build DoitDuck for
# the board first (firmware/main_board/out/doitduck).
#
#   sh tools/pack_image.sh [V610-Buildroot]     # default: ../../../.. from here
#   -> build/image/flash/, for burntool-cli and nand_burn_table.xml
set -eu

DUCK="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BR="$(CDPATH= cd -- "${1:-$DUCK/../../../..}" && pwd)"
OUT="$DUCK/build/image"
HOST="$BR/output/host"
. "$BR/board/cove_610/flash.cfg"

for f in "$BR/output/target/etc/init.d/S40app" "$BR/output/images/flash/nand_burn_table.xml" "$BR/output/build/buildroot-fs/full_devices_table.txt" \
         "$BR/buildroot/support/scripts/mkusers" "$HOST/bin/fakeroot" "$DUCK/firmware/main_board/out/doitduck"; do
    [ -e "$f" ] || { echo "pack_image: missing $f" >&2; exit 1; }
done

rm -rf "$OUT"
mkdir -p "$OUT/flash"
echo "pack_image: copying the root filesystem"
cp -a "$BR/output/target" "$OUT/target"

# DoitDuck, started by S40app; Wi-Fi at boot by S52wifi (the network is set from the web page).
APP="$OUT/target/root/doitduck"
mkdir -p "$APP/data" "$APP/models"
cp "$DUCK/firmware/main_board/out/doitduck" "$APP/"
cp -r "$DUCK/web" "$DUCK/robots" "$DUCK/sounds" "$APP/"
[ -d "$DUCK/models" ] && cp "$DUCK"/models/* "$APP/models/" 2>/dev/null || true
sed -i 's|^APP_PATH=.*|APP_PATH=/root/doitduck/doitduck|' "$OUT/target/etc/init.d/S40app"
install -m 755 "$DUCK/tools/board/S52wifi" "$OUT/target/etc/init.d/S52wifi"

# The root filesystem as Buildroot makes it (buildroot/fs/common.mk; its script is
# output/build/buildroot-fs/ubifs/fakeroot): the system users added, owners, devices, then UBIFS and a UBI volume.
FS="$BR/output/build/buildroot-fs"
{
    echo "set -e"
    echo "chown -h -R 0:0 '$OUT/target'"
    BR2_CONFIG="$BR/output/.config" PATH="$HOST/bin:$HOST/sbin:$PATH" "$BR/buildroot/support/scripts/mkusers" "$FS/full_users_table.txt" "$OUT/target"
    echo "'$HOST/bin/makedevs' -d '$FS/full_devices_table.txt' '$OUT/target'"
    echo "find '$OUT/target/run/' '$OUT/target/tmp/' -mindepth 1 -prune -print0 | xargs -0r rm -rf --"
    echo "'$HOST/sbin/mkfs.ubifs' -d '$OUT/target' -e 0x1f000 -c 472 -m 0x800 -x lzo -F -o '$OUT/rootfs.ubifs'"
} > "$OUT/fakeroot.sh"
echo "pack_image: making the root filesystem"
"$HOST/bin/fakeroot" -- sh "$OUT/fakeroot.sh"
sed -e "s;BR2_ROOTFS_UBIFS_PATH;$OUT/rootfs.ubifs;" "$BR/board/cove_610/ubinize.cfg" > "$OUT/ubinize.cfg"
"$HOST/sbin/ubinize" -o "$OUT/rootfs.ubi" -m 0x800 -p 0x20000 "$OUT/ubinize.cfg"

# The other flash files as built; the root filesystem ours; the environment with sdio1_pins=none and
# bootdelay=0 (a key held down at power-on still stops in U-Boot).
cp "$BR/output/images/flash/"* "$OUT/flash/"
ROOTFS="$OUT/flash/rootfs_${CHIP}_${NAND_PAGE}_${NAND_BLOCK}_$((ROOTFS_SIZE / 1048576))M.ubifs"
[ -f "$ROOTFS" ] || { echo "pack_image: no $(basename "$ROOTFS") in output/images/flash" >&2; exit 1; }
cp "$OUT/rootfs.ubi" "$ROOTFS"
size=$(stat -c %s "$ROOTFS")
[ "$size" -le "$ROOTFS_SIZE" ] || { echo "pack_image: root filesystem $size bytes, partition $ROOTFS_SIZE" >&2; exit 1; }
sed -i 's/^sdio1_pins=.*/sdio1_pins=none/; s/^bootdelay=.*/bootdelay=0/' "$OUT/flash/nand_env.txt"
"$HOST/bin/mkenvimage" -s "$ENV_IMAGE_SIZE" -o "$OUT/flash/nand_env.bin" "$OUT/flash/nand_env.txt"

rm -rf "$OUT/target"
echo "pack_image: $OUT/flash (root filesystem $((size / 1048576)) MB of $((ROOTFS_SIZE / 1048576)) MB)"
ls -l "$OUT/flash"
