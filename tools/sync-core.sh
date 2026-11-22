#!/bin/sh
# Copy the shared Dragon core to 6809Emulator (the terminal version), so
# CPU and hardware fixes are made once, here:  tools/sync-core.sh [path]
# Then build and test 6809Emulator and commit there too.
set -e
cd "$(dirname "$0")/.."
DEST=${1:-../6809Emulator}
[ -d "$DEST/.git" ] || { echo "no 6809Emulator checkout at $DEST"; exit 1; }
mkdir -p "$DEST/src/core"
for f in dragon.h dragon_cpu.c tape.c tape.h vdg.c; do
	cp "src/core/$f" "$DEST/src/core/$f"
done
echo "core copied to $DEST/src/core from 6809Emulator-Metal $(git rev-parse --short HEAD)"
