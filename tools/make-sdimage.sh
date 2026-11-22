#!/bin/sh
# Build a flashable SD card image: tools/make-sdimage.sh [d32.rom] [out.img]
# With no arguments it uses local/d32.rom (local/ is gitignored).
# One FAT32 partition holding build/sdcard/ (run `make sdcard` first) + ROM.
# Flash with Raspberry Pi Imager ("Use custom") or dd. Needs mtools + dosfstools.
set -e
ROM=${1:-local/d32.rom}
[ -f "$ROM" ] || { echo "No ROM at $ROM (usage: $0 [d32.rom] [out.img])"; exit 1; }
OUT=${2:-build/6809Emulator-Metal.img}
[ -f build/sdcard/kernel8.img ] || { echo "Run 'make sdcard' first"; exit 1; }
SIZE=64
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
truncate -s ${SIZE}M "$OUT"
echo 'start=2048, type=c, bootable' | sfdisk -q "$OUT"
mkfs.vfat --offset 2048 -F 32 -n DRAGON32 "$OUT" >/dev/null
export MTOOLS_SKIP_CHECK=1
mcopy -s -i "$OUT@@1M" build/sdcard/* ::
mcopy -i "$OUT@@1M" "$ROM" ::d32.rom
mdir -i "$OUT@@1M" ::
echo "Made $OUT (${SIZE} MB)"
