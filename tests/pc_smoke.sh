#!/bin/sh
# Boot the ROM in the PC build of the bare-metal core, type BASIC, check
# the screen.  Needs your own Dragon 32 ROM: tests/pc_smoke.sh [d32.rom]
set -e
cd "$(dirname "$0")/.."
ROM="${1:-local/d32.rom}"
if [ ! -f "$ROM" ]; then
	echo "SKIP: no ROM at $ROM (supply your own Dragon 32 ROM)"
	exit 0
fi
PC=build/pc/dragon_pc
fail=0
check() {	# check <screen> <wanted text>
	if printf '%s\n' "$1" | grep -qF "|$2"; then
		echo "PASS: screen shows '$2'"
	else
		echo "FAIL: screen does not show '$2'"; fail=1
	fi
}

OUT=$($PC -f 900 -t '? 2+3*4\r10 GOTO 10\rRUN\r\b' "$ROM")
check "$OUT" "OK"
check "$OUT" " 14"
check "$OUT" "BREAK IN 10"
[ $fail -eq 0 ] || printf '%s\n' "$OUT"

# CLS 3 fills the screen with blue semigraphics blocks (shown as '#')
OUT=$($PC -f 300 -t 'CLS 3\r' -p build/pc/cls3.ppm "$ROM")
if printf '%s\n' "$OUT" | sed -n 3p | grep -q '################################'; then
	echo "PASS: CLS 3 draws block graphics"
else
	echo "FAIL: CLS 3 did not draw block graphics"; fail=1; printf '%s\n' "$OUT"
fi
# A pixel below the "OK" line (x 100, y 100 of the 2x picture) is blue
HDR=$(head -3 build/pc/cls3.ppm | wc -c)
if [ "$(od -An -tu1 -j $((HDR + (100 * 512 + 100) * 3)) -N3 build/pc/cls3.ppm | tr -s ' ')" = " 32 32 255" ]; then
	echo "PASS: CLS 3 picture is blue"
else
	echo "FAIL: CLS 3 picture is not blue"; fail=1
fi

# PMODE 4 circle (needs CMPU, which the first version of the core got
# wrong): pixels on the circle are lit, its centre is not
$PC -f 3000 -s 1 -t '10 PMODE 4,1:PCLS:SCREEN 1,1\r20 CIRCLE(128,96),60\r30 GOTO 30\r\rRUN\r' -p build/pc/circle.ppm "$ROM" >/dev/null
HDR=$(head -3 build/pc/circle.ppm | wc -c)
px() { od -An -tu1 -j $((HDR + ($2 * 256 + $1) * 3)) -N1 build/pc/circle.ppm | tr -d ' '; }
if [ "$(px 128 36)" -gt 100 ] && [ "$(px 68 96)" -gt 100 ] && [ "$(px 188 96)" -gt 100 ] && [ "$(px 128 96)" -lt 50 ]; then
	echo "PASS: PMODE 4 circle drawn"
else
	echo "FAIL: PMODE 4 circle wrong (top $(px 128 36), left $(px 68 96), right $(px 188 96), centre $(px 128 96))"; fail=1
fi
exit $fail
