/*
 * keymap_test.c - checks the USB -> Dragon keyboard map (no ROM needed)
 * Copyright (C) 2026 Dylan Jones.  GNU GPL version 2 or later.
 */
#include "../src/keymap_usb.h"
#include "../src/core/dragon.h"
#include <stdio.h>
#include <string.h>

static int failures;

// Press these usages; expect exactly this one Dragon key (+ SHIFT or not)
static void expect(const char *what, const uint8_t *usages, int n, int row, int col, bool shift)
{
	bool held[256];
	memset(held, 0, sizeof(held));
	for (int i = 0; i < n; i++)
	{
		held[usages[i]] = true;
	}
	uint8_t m[8], want[8];
	keymap_matrix(held, m);
	memset(want, 0x7F, 8);
	if (row >= 0)
	{
		want[col] &= (uint8_t)~(1 << row);
	}
	if (shift)
	{
		want[7] &= (uint8_t)~(1 << 6);
	}
	if (memcmp(m, want, 8) != 0)
	{
		printf("FAIL: %s\n", what);
		failures++;
	}
	else
	{
		printf("PASS: %s\n", what);
	}
}

int main(void)
{
	const uint8_t S = USB_KEY_LSHIFT;
	expect("A", (uint8_t[]){ 0x04 }, 1, 2, 1, false);
	expect("Shift+A keeps SHIFT", (uint8_t[]){ S, 0x04 }, 2, 2, 1, true);
	expect("Z", (uint8_t[]){ 0x1D }, 1, 5, 2, false);
	expect("1", (uint8_t[]){ 0x1E }, 1, 0, 1, false);
	expect("0", (uint8_t[]){ 0x27 }, 1, 0, 0, false);
	expect("Shift+2 = \" = Dragon Shift+2", (uint8_t[]){ S, 0x1F }, 2, 0, 2, true);
	expect("; = Dragon ;", (uint8_t[]){ 0x33 }, 1, 1, 3, false);
	expect("Shift+; = : (unshifted on the Dragon)", (uint8_t[]){ S, 0x33 }, 2, 1, 2, false);
	expect("= is Dragon Shift+-", (uint8_t[]){ 0x2E }, 1, 1, 5, true);
	expect("Shift+= is + = Dragon Shift+;", (uint8_t[]){ S, 0x2E }, 2, 1, 3, true);
	expect("Shift+' = @ (UK)", (uint8_t[]){ S, 0x34 }, 2, 2, 0, false);
	expect("Shift+8 = * = Dragon Shift+:", (uint8_t[]){ S, 0x25 }, 2, 1, 2, true);
	expect("Enter", (uint8_t[]){ 0x28 }, 1, 6, 0, false);
	expect("Esc = BREAK", (uint8_t[]){ 0x29 }, 1, 6, 2, false);
	expect("Home = CLEAR", (uint8_t[]){ 0x4A }, 1, 6, 1, false);
	expect("Backspace = Left", (uint8_t[]){ 0x2A }, 1, 5, 5, false);
	expect("Space", (uint8_t[]){ 0x2C }, 1, 5, 7, false);
	expect("Shift+3 = pound: no Dragon key", (uint8_t[]){ S, 0x20 }, 2, -1, 0, false);
	expect("F1 is not a Dragon key", (uint8_t[]){ USB_KEY_F1 }, 1, -1, 0, false);
	expect("Shift alone presses nothing", (uint8_t[]){ S }, 1, -1, 0, false);

	printf(failures ? "\n%d keymap test(s) FAILED\n" : "\nall keymap tests passed\n", failures);
	return failures != 0;
}
