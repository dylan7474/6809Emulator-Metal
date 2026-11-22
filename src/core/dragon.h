/*
 * dragon.h - the Dragon 32 core as seen by a platform (Pi or PC)
 *
 * Copyright (C) 2026 Dylan Jones
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The core is plain C with no I/O of its own: the platform hands it the
 * ROM, calls dragon_run_frame() 50 times a second, draws the picture
 * vdg_render() makes, and feeds it keys.
 */
#ifndef DRAGON_H
#define DRAGON_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRAGON_ROM_SIZE		16384

// Instructions run per 50 Hz frame.  Same figure as the ncurses version
// (17800 steps, ~50 frames a second); the core does not count cycles yet.
#define DRAGON_FRAME_STEPS	17800

// ---- CPU + memory (dragon_cpu.c) -------------------------------------------

// Copy the 16 KB BASIC ROM in, clear RAM and reset the CPU.
void dragon_init(const uint8_t *rom);

// Reset button: restart the CPU from the reset vector (RAM is kept).
void dragon_reset(void);

// Run one 50 Hz frame's worth of instructions.
void dragon_run_frame(void);

// The 64 KB address space as the CPU sees it (video RAM, $FF22 etc).
const uint8_t *dragon_memory(void);

typedef struct
{
	uint16_t pc, x, y, u, s;
	uint8_t a, b, dp, cc;
} DragonRegs;
void dragon_regs(DragonRegs *regs);

// ---- Keyboard ----------------------------------------------------------------
// The Dragon's keyboard is an 8 x 7 matrix: column 0-7 (PIA0 port B),
// row 0-6 (PIA0 port A).  SHIFT is row 6, column 7.

#define DRAGON_KEY_ENTER_ROW	6
#define DRAGON_KEY_ENTER_COL	0
#define DRAGON_KEY_CLEAR_ROW	6
#define DRAGON_KEY_CLEAR_COL	1
#define DRAGON_KEY_BREAK_ROW	6
#define DRAGON_KEY_BREAK_COL	2
#define DRAGON_KEY_SHIFT_ROW	6
#define DRAGON_KEY_SHIFT_COL	7

// Live keyboard: matrix[col] has a 0 bit for each row held down
// (0x7F = nothing in that column).  Used by the Pi's USB keyboard.
void dragon_set_matrix(const uint8_t matrix[8]);

// Typed input: each character is pressed and released in turn, with the
// timing the ROM's keyboard scan needs.  "\r" or a real CR = ENTER.
// Used for tests (like the ncurses version's --inject).
void dragon_type(const char *text);
bool dragon_typing(void);		// still keys waiting to be typed

// Which key gives this character: false if the Dragon has no such key.
bool dragon_char_key(char c, int *row, int *col, bool *shift);

// ---- Video (vdg.c) -----------------------------------------------------------

#define VDG_WIDTH	256
#define VDG_HEIGHT	192

// Palette indices used in the picture
enum
{
	VDG_GREEN, VDG_YELLOW, VDG_BLUE, VDG_RED,
	VDG_BUFF, VDG_CYAN, VDG_MAGENTA, VDG_ORANGE,
	VDG_BLACK, VDG_DARK_GREEN, VDG_DARK_ORANGE,
	VDG_COLOURS
};

// RGB for each palette index
extern const uint8_t vdg_palette[VDG_COLOURS][3];

// Draw the screen as the MC6847 would: one palette index per pixel.
// Text and semigraphics-4 from the 512 bytes at $0400.
void vdg_render(const uint8_t *memory, uint8_t picture[VDG_HEIGHT][VDG_WIDTH]);

// The text screen as plain characters (16 rows of 32 + '\n'), for tests
// and logs.  buf must hold 16 * 33 + 1 bytes.
void vdg_text(const uint8_t *memory, char *buf);

#ifdef __cplusplus
}
#endif

#endif
