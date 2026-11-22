#!/bin/sh
# Make a FAT32 SD card image for QEMU testing: tools/qemu-sd.sh [d32.rom]
# Writes build/qemu-sd.img (64 MB). Needs mtools + dosfstools.
set -e
ROM=${1:-local/d32.rom}
[ -f "$ROM" ] || { echo "No ROM at $ROM (usage: $0 [d32.rom])"; exit 1; }
OUT=build/qemu-sd.img
mkdir -p build
rm -f "$OUT"
truncate -s 64M "$OUT"
echo 'start=2048, type=c' | sfdisk -q "$OUT"
mkfs.vfat --offset 2048 -F 32 -n DRAGON "$OUT" >/dev/null
export MTOOLS_SKIP_CHECK=1
mcopy -i "$OUT@@1M" "$ROM" ::d32.rom
mdir -i "$OUT@@1M" ::
echo "Made $OUT"
