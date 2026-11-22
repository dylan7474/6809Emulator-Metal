/*
 * keymap_usb.c - USB keyboard (UK layout) -> Dragon 32 keyboard matrix
 *
 * Copyright (C) 2026 Dylan Jones
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "keymap_usb.h"
#include "core/dragon.h"
#include <string.h>

// Keys whose PC character is looked up in the Dragon's own table:
// usage, unshifted, shifted (UK layout; 0 = no Dragon key)
static const struct { uint8_t usage; char plain, shifted; } symbols[] =
{
	{ 0x1E, '1', '!' },  { 0x1F, '2', '"' },  { 0x20, '3', 0 },
	{ 0x21, '4', '$' },  { 0x22, '5', '%' },  { 0x23, '6', '^' },
	{ 0x24, '7', '&' },  { 0x25, '8', '*' },  { 0x26, '9', '(' },
	{ 0x27, '0', ')' },
	{ 0x2D, '-', '_' },  { 0x2E, '=', '+' },  { 0x2F, '[', 0 },
	{ 0x30, ']', 0 },    { 0x32, '#', 0 },    { 0x33, ';', ':' },
	{ 0x34, '\'', '@' }, { 0x36, ',', '<' },  { 0x37, '.', '>' },
	{ 0x38, '/', '?' },  { 0x64, '\\', 0 },
};

// Keys that are a Dragon key as they stand: SHIFT is passed through
static const struct { uint8_t usage; int8_t row, col; } direct[] =
{
	{ 0x28, 6, 0 },		// Enter -> ENTER
	{ 0x58, 6, 0 },		// keypad Enter -> ENTER
	{ 0x4A, 6, 1 },		// Home -> CLEAR
	{ 0x29, 6, 2 },		// Esc -> BREAK
	{ 0x48, 6, 2 },		// Pause -> BREAK
	{ 0x2C, 5, 7 },		// Space
	{ 0x52, 5, 3 },		// Up
	{ 0x51, 5, 4 },		// Down
	{ 0x50, 5, 5 },		// Left
	{ 0x2A, 5, 5 },		// Backspace -> Left (the Dragon's delete)
	{ 0x4F, 5, 6 },		// Right
};

bool keymap_usb(uint8_t usage, bool pc_shift, int *row, int *col, bool *shift)
{
	if (usage >= 0x04 && usage <= 0x1D)	// A-Z
	{
		bool shifted;
		dragon_char_key((char)('A' + usage - 0x04), row, col, &shifted);
		*shift = pc_shift;
		return true;
	}
	for (unsigned i = 0; i < sizeof(direct) / sizeof(direct[0]); i++)
	{
		if (direct[i].usage == usage)
		{
			*row = direct[i].row;
			*col = direct[i].col;
			*shift = pc_shift;
			return true;
		}
	}
	for (unsigned i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++)
	{
		if (symbols[i].usage == usage)
		{
			char c = pc_shift ? symbols[i].shifted : symbols[i].plain;
			return c != 0 && dragon_char_key(c, row, col, shift);
		}
	}
	return false;
}

void keymap_matrix(const bool held[256], uint8_t matrix[8])
{
	bool pc_shift = held[USB_KEY_LSHIFT] || held[USB_KEY_RSHIFT];
	bool need_shift = false;

	memset(matrix, 0x7F, 8);
	for (int usage = 0x04; usage < 0xE0; usage++)
	{
		int row, col;
		bool shift;
		if (held[usage] && keymap_usb((uint8_t)usage, pc_shift, &row, &col, &shift))
		{
			matrix[col] &= (uint8_t)~(1 << row);
			need_shift |= shift;
		}
	}
	if (need_shift)
	{
		matrix[DRAGON_KEY_SHIFT_COL] &= (uint8_t)~(1 << DRAGON_KEY_SHIFT_ROW);
	}
}
