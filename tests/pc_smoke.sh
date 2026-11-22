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
exit $fail
