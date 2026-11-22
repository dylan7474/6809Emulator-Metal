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

// Timing of a PAL Dragon: 0.8867 MHz CPU, 57 cycles per video line,
// 312 lines per 50 Hz frame
#define DRAGON_LINE_CYCLES	57
#define DRAGON_FRAME_LINES	312
#define DRAGON_FRAME_CYCLES	(DRAGON_LINE_CYCLES * DRAGON_FRAME_LINES)	// 17784

// ---- CPU + memory (dragon_cpu.c) -------------------------------------------

// Copy the 16 KB BASIC ROM in, clear RAM and reset the CPU.
void dragon_init(const uint8_t *rom);

// Reset button: restart the CPU from the reset vector (RAM is kept).
void dragon_reset(void);

// Run one 50 Hz frame: 17784 CPU cycles, with the VDG's line and frame
// sync pulses (the frame pulse is BASIC's 50 Hz IRQ).
void dragon_run_frame(void);

// One instruction (or an interrupt being taken) with the same timing,
// interrupts and sync pulses as dragon_run_frame(); trace = print it
// with the registers (for debugging and the terminal version's trace mode)
void dragon_step(bool trace);

uint64_t dragon_cycles(void);		// CPU cycles run since dragon_init
unsigned dragon_sam(void);		// the SAM's 16 control bits (bit 3-9 = display start / 512)
unsigned dragon_irqs(void);		// IRQs taken since dragon_init

// ---- Sound -----------------------------------------------------------------
// Mono, signed 16-bit, made as the CPU runs (rate / 50 samples a frame).
void dragon_audio_rate(unsigned rate);	// e.g. 48000; 0 = off (the default)
unsigned dragon_audio_take(int16_t *buf, unsigned max);	// samples made so far

// The 64 KB address space as the CPU sees it (video RAM, $FF22 etc).
const uint8_t *dragon_memory(void);

typedef struct
{
	uint16_t pc, x, y, u, s;
	uint8_t a, b, dp, cc;
	uint8_t wait;		// 0 = running, 1 = in SYNC, 2 = in CWAI
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
#define DRAGON_KEY_UP_ROW	5	// cursor keys: row 5, columns 3-6
#define DRAGON_KEY_UP_COL	3
#define DRAGON_KEY_DOWN_COL	4
#define DRAGON_KEY_LEFT_COL	5
#define DRAGON_KEY_RIGHT_COL	6

// Live keyboard: matrix[col] has a 0 bit for each row held down
// (0x7F = nothing in that column).  Used by the Pi's USB keyboard.
void dragon_set_matrix(const uint8_t matrix[8]);

// Typed input: each character is pressed and released in turn, with the
// timing the ROM's keyboard scan needs.  "\r" or a real CR = ENTER.
// Used for tests (like the ncurses version's --inject).
void dragon_type(const char *text);
bool dragon_typing(void);		// still keys waiting to be typed
void dragon_type_key(int row, int col, bool shift);	// one key by matrix position, typed

// Which key gives this character: false if the Dragon has no such key.
bool dragon_char_key(char c, int *row, int *col, bool *shift);

// ---- Cassette deck (tape.c) ---------------------------------------------------
// One input tape (.CAS or .WAV) plays into the cassette port in real time,
// started and stopped by the Dragon's motor relay (PIA1 CA2) while PLAY is
// down.  Anything the Dragon writes to tape (CSAVE, CSAVEM, data files) is
// recorded as a .WAV (real-tape compatible) and decoded to .CAS bytes.
// The platform supplies the file I/O.

#define DRAGON_TAPE_CAS		1
#define DRAGON_TAPE_WAV		2
#define DRAGON_TAPE_WAV_RATE	22050	// recording: 8-bit unsigned mono

// Read len bytes at offset of the inserted tape file; returns bytes read.
typedef unsigned (*DragonTapeRead)(void *ctx, uint32_t offset, uint8_t *buf, unsigned len);

// Insert a tape (PLAY is pressed); false if it is not a usable WAV.
bool dragon_tape_insert(int kind, uint32_t size, DragonTapeRead read, void *ctx);
void dragon_tape_eject(void);
bool dragon_tape_inserted(void);
void dragon_tape_play(bool play);	// the deck's PLAY / STOP keys
bool dragon_tape_playing(void);
bool dragon_tape_motor(void);		// the Dragon's motor relay is on
void dragon_tape_seek_ms(uint32_t ms);	// rewind = 0; fast forward/back
uint32_t dragon_tape_pos_ms(void);	// tape counter
uint32_t dragon_tape_len_ms(void);

// Programs found on a .CAS tape (name blocks), for "jump to program"
typedef struct
{
	char name[9];
	uint8_t type;		// 0 BASIC, 1 data, 2 machine code
	uint32_t ms;		// tape position of its leader
} DragonTapeProgram;
unsigned dragon_tape_programs(DragonTapeProgram *list, unsigned max);

// Recording.  begin() when the Dragon starts writing a tape; wav() and
// cas() with the data as it comes; end() once the end-of-file block is
// written and the motor stops (name = the program's name from its header,
// "" if none).  NULL = no recording.
typedef struct
{
	void *ctx;
	void (*begin)(void *ctx);
	void (*wav)(void *ctx, const uint8_t *samples, unsigned n);
	void (*cas)(void *ctx, const uint8_t *bytes, unsigned n);
	void (*end)(void *ctx, const char *name);
} DragonTapeRecorder;
void dragon_tape_recorder(const DragonTapeRecorder *rec);
void dragon_tape_record_finish(void);	// end a recording now (eject, shutdown)
bool dragon_tape_recording(void);

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

// The border colour around the picture (black for text, the colour set's
// first colour for graphics)
uint8_t vdg_border(const uint8_t *memory);

// One row (0-11) of the 6847's 8 x 12 cell for an ASCII character
// (bit 7 = leftmost pixel); lower case shows as upper case.  For the
// platform's own status text.
uint8_t vdg_font_row(char c, int row);

// The text screen as plain characters (16 rows of 32 + '\n'), for tests
// and logs.  buf must hold 16 * 33 + 1 bytes.
void vdg_text(const uint8_t *memory, char *buf);

#ifdef __cplusplus
}
#endif

#endif
