/*
 * dragon_cpu.c - Motorola 6809 CPU + Dragon 32 memory map and keyboard
 *
 * Copyright (C) 2021-2026 Dylan Jones
 *
 * This is src/emulator.c from 6809Emulator (commit 1a96432) with the
 * ncurses screen, ROM file loading and main() taken out; the CPU,
 * memory map and keyboard matrix code is unchanged.  The platform API
 * (dragon.h) is at the end of the file.  Keep CPU fixes in step with
 * the 6809Emulator repo.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "dragon.h"
#include "tape.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>

// This will contain the ROM
static unsigned char rom[16384];

// This will contain the RAM
static unsigned char ram[65536];

// Registers. These MUST stay static: ncurses exports its own global "PC"
// (termcap pad char) and writes to it in initscr(), which would otherwise
// overwrite the 6809 program counter and crash the full-screen mode.
static uint8_t A;			// 8-bit accumulator A
static uint8_t B;			// 8-bit accumulator B
static uint8_t CC;			// 8-bit Condition Code
static uint8_t DP;			// 8-bit Direct Page Register
static uint16_t X, Y;		// 16-bit index registers
static uint16_t U;			// 16-bit User stack pointer
static uint16_t S;			// 16-bit System stack pointer
static uint16_t PC;		// 16-bit Program Counter

// Condition Code (CC) flag bitmasks: E F H I N Z V C
#define CC_C 0x01
#define CC_V 0x02
#define CC_Z 0x04
#define CC_N 0x08
#define CC_I 0x10
#define CC_H 0x20
#define CC_F 0x40
#define CC_E 0x80

// Helper functions for 16-bit D accumulator (A:B)
static inline uint16_t get_D(void)
{
	return ((uint16_t)A << 8) | (uint16_t)B;
}

static inline void set_D(uint16_t val)
{
	A = (uint8_t)((val >> 8) & 0xFF);
	B = (uint8_t)(val & 0xFF);
}

// Memory Abstraction and ROM Write Protection

// Dragon 32 Keyboard Matrix & Input Emulation

// CPU wait states: SYNC waits for any interrupt line, CWAI has already
// stacked the registers and waits for an unmasked interrupt
static bool cpu_sync, cpu_cwai;

static uint8_t key_matrix[8] = { 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F }; // Rows PA0..PA6 for each column (active low)

typedef struct {
	int row;
	int col;
	bool shift;
	char ch;
} KbdEvent;

#define KBD_QUEUE_SIZE 256
static KbdEvent kbd_queue[KBD_QUEUE_SIZE];
static int kbd_q_head = 0;
static int kbd_q_tail = 0;
static int kbd_hold_cycles = 0;
static int kbd_release_cycles = 0;

static void kbd_enqueue_event(int row, int col, bool shift, char ch)
{
	int next = (kbd_q_tail + 1) % KBD_QUEUE_SIZE;
	if (next != kbd_q_head)
	{
		kbd_queue[kbd_q_tail].row = row;
		kbd_queue[kbd_q_tail].col = col;
		kbd_queue[kbd_q_tail].shift = shift;
		kbd_queue[kbd_q_tail].ch = ch;
		kbd_q_tail = next;
	}
}

static bool kbd_map_char(char c, int *out_row, int *out_col, bool *out_shift);

static void kbd_enqueue_char(char c)
{
	int row = 0, col = 0;
	bool shift = false;
	if (kbd_map_char(c, &row, &col, &shift))
	{
		kbd_enqueue_event(row, col, shift, c);
	}
}

static void kbd_enqueue_special(int row, int col, bool shift)
{
	kbd_enqueue_event(row, col, shift, 0);
}

static void kbd_enqueue_string(const char *str)
{
	while (*str)
	{
		if (*str == '\\' && *(str + 1) == 'r')
		{
			kbd_enqueue_char('\r');
			str += 2;
		}
		else if (*str == '\\' && *(str + 1) == 'c')
		{
			kbd_enqueue_special(6, 1, false); // Dragon 32 CLEAR
			str += 2;
		}
		else if (*str == '\\' && *(str + 1) == 'b')
		{
			kbd_enqueue_special(6, 2, false); // Dragon 32 BREAK
			str += 2;
		}
		else if (*str == '\\' && *(str + 1) == 'n')
		{
			kbd_enqueue_char('\r');
			str += 2;
		}
		else
		{
			kbd_enqueue_char(*str++);
		}
	}
}

static void kbd_clear_matrix(void)
{
	for (int i = 0; i < 8; i++)
	{
		key_matrix[i] = 0x7F;
	}
}

static void kbd_set_key(int row, int col, bool shift)
{
	if (col >= 0 && col < 8 && row >= 0 && row < 7)
	{
		key_matrix[col] &= ~(1 << row);
	}
	if (shift)
	{
		key_matrix[7] &= ~(1 << 6); // Shift is at PB7, PA6
	}
}
static bool kbd_map_char(char c, int *out_row, int *out_col, bool *out_shift)
{
	*out_shift = false;
	if (c >= 'a' && c <= 'z') c = (char)(c - 32);

	switch (c)
	{
		case '0': *out_row = 0; *out_col = 0; return true;
		case '1': *out_row = 0; *out_col = 1; return true;
		case '2': *out_row = 0; *out_col = 2; return true;
		case '3': *out_row = 0; *out_col = 3; return true;
		case '4': *out_row = 0; *out_col = 4; return true;
		case '5': *out_row = 0; *out_col = 5; return true;
		case '6': *out_row = 0; *out_col = 6; return true;
		case '7': *out_row = 0; *out_col = 7; return true;

		case '8': *out_row = 1; *out_col = 0; return true;
		case '9': *out_row = 1; *out_col = 1; return true;
		case ':': *out_row = 1; *out_col = 2; return true;
		case ';': *out_row = 1; *out_col = 3; return true;
		case ',': *out_row = 1; *out_col = 4; return true;
		case '-': *out_row = 1; *out_col = 5; return true;
		case '.': *out_row = 1; *out_col = 6; return true;
		case '/': *out_row = 1; *out_col = 7; return true;

		case '@': *out_row = 2; *out_col = 0; return true;
		case 'A': *out_row = 2; *out_col = 1; return true;
		case 'B': *out_row = 2; *out_col = 2; return true;
		case 'C': *out_row = 2; *out_col = 3; return true;
		case 'D': *out_row = 2; *out_col = 4; return true;
		case 'E': *out_row = 2; *out_col = 5; return true;
		case 'F': *out_row = 2; *out_col = 6; return true;
		case 'G': *out_row = 2; *out_col = 7; return true;

		case 'H': *out_row = 3; *out_col = 0; return true;
		case 'I': *out_row = 3; *out_col = 1; return true;
		case 'J': *out_row = 3; *out_col = 2; return true;
		case 'K': *out_row = 3; *out_col = 3; return true;
		case 'L': *out_row = 3; *out_col = 4; return true;
		case 'M': *out_row = 3; *out_col = 5; return true;
		case 'N': *out_row = 3; *out_col = 6; return true;
		case 'O': *out_row = 3; *out_col = 7; return true;

		case 'P': *out_row = 4; *out_col = 0; return true;
		case 'Q': *out_row = 4; *out_col = 1; return true;
		case 'R': *out_row = 4; *out_col = 2; return true;
		case 'S': *out_row = 4; *out_col = 3; return true;
		case 'T': *out_row = 4; *out_col = 4; return true;
		case 'U': *out_row = 4; *out_col = 5; return true;
		case 'V': *out_row = 4; *out_col = 6; return true;
		case 'W': *out_row = 4; *out_col = 7; return true;

		case 'X': *out_row = 5; *out_col = 0; return true;
		case 'Y': *out_row = 5; *out_col = 1; return true;
		case 'Z': *out_row = 5; *out_col = 2; return true;
		case ' ': *out_row = 5; *out_col = 7; return true;

		case '\n':
		case '\r': *out_row = 6; *out_col = 0; return true;

		// Shifted Digits on Dragon 32 (subtract $10):
		case '!': *out_row = 0; *out_col = 1; *out_shift = true; return true; // Shift + 1
		case '"': *out_row = 0; *out_col = 2; *out_shift = true; return true; // Shift + 2
		case '#': *out_row = 0; *out_col = 3; *out_shift = true; return true; // Shift + 3
		case '$': *out_row = 0; *out_col = 4; *out_shift = true; return true; // Shift + 4
		case '%': *out_row = 0; *out_col = 5; *out_shift = true; return true; // Shift + 5
		case '&': *out_row = 0; *out_col = 6; *out_shift = true; return true; // Shift + 6
		case '\'': *out_row = 0; *out_col = 7; *out_shift = true; return true; // Shift + 7
		case '(': *out_row = 1; *out_col = 0; *out_shift = true; return true; // Shift + 8
		case ')': *out_row = 1; *out_col = 1; *out_shift = true; return true; // Shift + 9
		case '*': *out_row = 1; *out_col = 2; *out_shift = true; return true; // Shift + :
		case '+': *out_row = 1; *out_col = 3; *out_shift = true; return true; // Shift + ;

		// Shifted Punctuation (from Dragon 32 table at $BC84):
		case '<': *out_row = 1; *out_col = 4; *out_shift = true; return true; // Shift + ,
		case '=': *out_row = 1; *out_col = 5; *out_shift = true; return true; // Shift + -
		case '>': *out_row = 1; *out_col = 6; *out_shift = true; return true; // Shift + .
		case '?': *out_row = 1; *out_col = 7; *out_shift = true; return true; // Shift + /
		case '[': *out_row = 5; *out_col = 4; *out_shift = true; return true; // Shift + Down
		case ']': *out_row = 5; *out_col = 6; *out_shift = true; return true; // Shift + Right
		case '\\': *out_row = 6; *out_col = 1; *out_shift = true; return true; // Shift + Clear
		case '^': *out_row = 5; *out_col = 3; *out_shift = true; return true; // Shift + Up
		case '_': *out_row = 5; *out_col = 3; *out_shift = true; return true; // Shift + Up

		default: return false;
	}
}

static void kbd_update_cycles(int cycles)
{
	if (kbd_hold_cycles > 0)
	{
		kbd_hold_cycles -= cycles;
		if (kbd_hold_cycles <= 0)
		{
			kbd_clear_matrix();
			kbd_release_cycles = 60000;	// ~68 ms (was 15000 instructions)
		}
	}
	else if (kbd_release_cycles > 0)
	{
		kbd_release_cycles -= cycles;
	}
	else if (kbd_q_head != kbd_q_tail)
	{
		KbdEvent ev = kbd_queue[kbd_q_head];
		kbd_q_head = (kbd_q_head + 1) % KBD_QUEUE_SIZE;
		kbd_clear_matrix();
		kbd_set_key(ev.row, ev.col, ev.shift);
		kbd_hold_cycles = 100000;	// ~113 ms (was 25000 instructions)
	}
}

// ---- MC6821 PIAs ($FF00-$FF1F = PIA0, $FF20-$FF3F = PIA1, mirrored every 4) ----
// Behaviour follows the MC6821 datasheet as modelled in XRoar: a data
// address reads the data direction register when control bit 2 is 0;
// reading the data register clears the interrupt flags; Cx1 edges set
// control bit 7 and pull IRQ (PIA0) or FIRQ (PIA1) when bit 0 enables it.
//
// PIA0 A: keyboard rows (PA0-6) + joystick comparator (PA7)   CA1: HS  CA2: sound mux SEL1
// PIA0 B: keyboard columns (out)                              CB1: FS  CB2: sound mux SEL2
// PIA1 A: PA2-7 6-bit DAC, PA1 printer strobe, PA0 cassette   CA2: cassette motor
// PIA1 B: PB3-7 VDG mode, PB1 single-bit sound, PB0 printer busy  CB2: sound enable

typedef struct
{
	uint8_t ora, ddra, cra;		// port A output, direction, control
	uint8_t orb, ddrb, crb;
	bool ca1, cb1;			// current level on the Cx1 inputs
} Pia;

static Pia pia0, pia1;

static void audio_update_level(void);

static void pia_reset(Pia *p)
{
	memset(p, 0, sizeof(*p));
	p->ca1 = p->cb1 = true;
}

// Cx1 input changed: the active edge (control bit 1: 0 = falling, 1 = rising)
// sets the flag in control bit 7
static void pia_set_cx1(uint8_t *cr, bool *line, bool level)
{
	if (level == *line)
	{
		return;
	}
	*line = level;
	if (((*cr & 0x02) != 0) == level)
	{
		*cr |= 0x80;
	}
}

static inline bool pia_irq(const Pia *p)
{
	return ((p->cra & 0x81) == 0x81) || ((p->crb & 0x81) == 0x81);
}

// Cx2 as a manual output (control bits 5,4 = 1,1) gives bit 3; otherwise high
static inline bool pia_cx2(uint8_t cr)
{
	return (cr & 0x30) == 0x30 ? (cr & 0x08) != 0 : true;
}

// Pin levels on an output port: outputs drive, inputs float high (port A)
static inline uint8_t pia_out_a(const Pia *p)
{
	return (uint8_t)~(~p->ora & p->ddra);
}

static inline uint8_t pia_out_b(const Pia *p)
{
	return p->orb & p->ddrb;
}

static uint8_t keyboard_rows(void)
{
	uint8_t cols = pia_out_b(&pia0) | (uint8_t)~pia0.ddrb;	// undriven columns stay high
	uint8_t rows = 0x7F;
	for (int col = 0; col < 8; col++)
	{
		if ((cols & (1 << col)) == 0)
		{
			rows &= key_matrix[col];
		}
	}
	return rows | 0x80;	// PA7: joystick comparator, no joystick
}

static uint8_t pia_read(uint16_t addr)
{
	Pia *p = (addr & 0x20) ? &pia1 : &pia0;
	switch (addr & 3)
	{
	case 0:
		if (!(p->cra & 0x04))
		{
			return p->ddra;
		}
		p->cra &= 0x3F;		// reading the data clears the flags
		{
			uint8_t in = (p == &pia0) ? keyboard_rows()
				   : (uint8_t)(0xFE | (tape_input_level() ? 1 : 0));	// PIA1 PA0: cassette in
			return pia_out_a(p) & in;
		}
	case 1:
		return p->cra;
	case 2:
		if (!(p->crb & 0x04))
		{
			return p->ddrb;
		}
		p->crb &= 0x3F;
		{
			uint8_t in = (p == &pia1) ? 0x01 : 0x00;	// PIA1 PB0: printer !BUSY pulled up
			return (pia_out_b(p) | (in & (uint8_t)~p->ddrb));
		}
	default:
		return p->crb;
	}
}

static void pia_write(uint16_t addr, uint8_t val)
{
	Pia *p = (addr & 0x20) ? &pia1 : &pia0;
	switch (addr & 3)
	{
	case 0:
		if (p->cra & 0x04) p->ora = val; else p->ddra = val;
		break;
	case 1:
		p->cra = (uint8_t)((p->cra & 0xC0) | (val & 0x3F));
		break;
	case 2:
		if (p->crb & 0x04) p->orb = val; else p->ddrb = val;
		break;
	default:
		p->crb = (uint8_t)((p->crb & 0xC0) | (val & 0x3F));
		break;
	}
	// The VDG reads PIA1 port B (mode bits); keep a copy where vdg.c looks
	ram[0xFF22] = pia_out_b(&pia1);
	// Cassette motor relay: PIA1 CA2 as an output, set high
	tape_set_motor((pia1.cra & 0x38) == 0x38);
	audio_update_level();
}

static inline uint8_t mem_read(uint16_t addr)
{
	if ((addr & 0xFFC0) == 0xFF00)
	{
		return pia_read(addr);
	}
	return ram[addr];
}

static inline uint16_t mem_read16(uint16_t addr)
{
	if (addr >= 0xFEFF && addr < 0xFF40)	// either byte in the PIAs
	{
		return ((uint16_t)mem_read(addr) << 8) | mem_read((uint16_t)(addr + 1));
	}
	return ((uint16_t)ram[addr] << 8) | ram[(uint16_t)(addr + 1)];
}

// ---- SAM (MC6883) ----
// 16 control bits, each set by writing anywhere to an odd address and
// cleared by an even one in $FFC0-$FFDF: V0-V2 (display mode), F0-F6
// (display start, x 512 bytes), P1, R0-R1 (CPU rate), M0-M1 (memory size),
// TY (map type).  The data written doesn't matter.
static uint16_t sam_reg;

static inline void mem_write(uint16_t addr, uint8_t val)
{
	if ((addr & 0xFFC0) == 0xFF00)
	{
		pia_write(addr, val);
		return;
	}
	if (addr >= 0xFFC0 && addr <= 0xFFDF)
	{
		uint16_t bit = (uint16_t)(1u << ((addr - 0xFFC0) >> 1));
		if (addr & 1)
		{
			sam_reg |= bit;
		}
		else
		{
			sam_reg &= (uint16_t)~bit;
		}
		return;
	}
	// Dragon 32 ROM Protection:
	// $8000-$BFFF: 16KB BASIC ROM
	// $FFF0-$FFFF: Vectors
	if ((addr >= 0x8000 && addr <= 0xBFFF) || addr >= 0xFFF0)
	{
		return;
	}
	ram[addr] = val;
}

static inline void mem_write16(uint16_t addr, uint16_t val)
{
	mem_write(addr, (uint8_t)(val >> 8));
	mem_write((uint16_t)(addr + 1), (uint8_t)(val & 0xFF));
}

// Flag helpers
static inline void set_flags_nz8(uint8_t res)
{
	CC &= ~(CC_N | CC_Z | CC_V);
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
}

static inline void set_flags_nz16(uint16_t res)
{
	CC &= ~(CC_N | CC_Z | CC_V);
	if (res == 0) CC |= CC_Z;
	if (res & 0x8000) CC |= CC_N;
}

static inline void set_flags_logic8(uint8_t res)
{
	CC &= ~(CC_N | CC_Z | CC_V);
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
}

static inline void set_flags_clr(void)
{
	CC &= ~(CC_N | CC_V | CC_C);
	CC |= CC_Z;
}

static inline uint8_t do_inc8(uint8_t val)
{
	uint8_t res = val + 1;
	CC &= ~(CC_N | CC_Z | CC_V);
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if (val == 0x7F) CC |= CC_V;
	return res;
}

static inline uint8_t do_dec8(uint8_t val)
{
	uint8_t res = val - 1;
	CC &= ~(CC_N | CC_Z | CC_V);
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if (val == 0x80) CC |= CC_V;
	return res;
}

static inline uint8_t do_com8(uint8_t val)
{
	uint8_t res = (uint8_t)~val;
	CC &= ~(CC_N | CC_Z | CC_V);
	CC |= CC_C;
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	return res;
}

static inline uint8_t do_neg8(uint8_t val)
{
	uint8_t res = (uint8_t)(0 - val);
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if (val == 0x80) CC |= CC_V;
	if (res != 0) CC |= CC_C;
	return res;
}

static inline uint8_t do_sub8(uint8_t r, uint8_t v)
{
	uint16_t diff = (uint16_t)r - (uint16_t)v;
	uint8_t res = (uint8_t)diff;
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if (((r ^ v) & (r ^ res)) & 0x80) CC |= CC_V;
	if (diff & 0x100) CC |= CC_C;
	return res;
}

static inline uint8_t do_sbc8(uint8_t r, uint8_t v)
{
	uint8_t c = (CC & CC_C) ? 1 : 0;
	uint16_t diff = (uint16_t)r - (uint16_t)v - c;
	uint8_t res = (uint8_t)diff;
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if (((r ^ v) & (r ^ res)) & 0x80) CC |= CC_V;
	if (diff & 0x100) CC |= CC_C;
	return res;
}

static inline uint8_t do_add8(uint8_t r, uint8_t v)
{
	uint16_t sum = (uint16_t)r + (uint16_t)v;
	uint8_t res = (uint8_t)sum;
	CC &= ~(CC_H | CC_N | CC_Z | CC_V | CC_C);
	if ((r ^ v ^ res) & 0x10) CC |= CC_H;
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if ((~(r ^ v) & (r ^ res)) & 0x80) CC |= CC_V;
	if (sum & 0x100) CC |= CC_C;
	return res;
}

static inline uint8_t do_adc8(uint8_t r, uint8_t v)
{
	uint8_t c = (CC & CC_C) ? 1 : 0;
	uint16_t sum = (uint16_t)r + (uint16_t)v + c;
	uint8_t res = (uint8_t)sum;
	CC &= ~(CC_H | CC_N | CC_Z | CC_V | CC_C);
	if ((r ^ v ^ res) & 0x10) CC |= CC_H;
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if ((~(r ^ v) & (r ^ res)) & 0x80) CC |= CC_V;
	if (sum & 0x100) CC |= CC_C;
	return res;
}

static inline void do_bit8(uint8_t r, uint8_t v)
{
	uint8_t res = r & v;
	set_flags_logic8(res);
}

static inline uint8_t do_lsr8(uint8_t val)
{
	uint8_t c = val & 0x01;
	uint8_t res = val >> 1;
	CC &= ~(CC_N | CC_Z | CC_C);
	if (c) CC |= CC_C;
	if (res == 0) CC |= CC_Z;
	return res;
}

static inline uint8_t do_ror8(uint8_t val)
{
	uint8_t old_c = (CC & CC_C) ? 0x80 : 0x00;
	uint8_t new_c = val & 0x01;
	uint8_t res = (val >> 1) | old_c;
	CC &= ~(CC_N | CC_Z | CC_C);
	if (new_c) CC |= CC_C;
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	return res;
}

static inline uint8_t do_asr8(uint8_t val)
{
	uint8_t c = val & 0x01;
	uint8_t res = (uint8_t)(((int8_t)val) >> 1);
	CC &= ~(CC_N | CC_Z | CC_C);
	if (c) CC |= CC_C;
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	return res;
}

static inline uint8_t do_asl8(uint8_t val)
{
	uint16_t res16 = (uint16_t)val << 1;
	uint8_t res = (uint8_t)res16;
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (res16 & 0x100) CC |= CC_C;
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if (((val ^ res) & 0x80) != 0) CC |= CC_V;
	return res;
}

static inline uint8_t do_rol8(uint8_t val)
{
	uint8_t old_c = (CC & CC_C) ? 1 : 0;
	uint8_t new_c = (val & 0x80) ? 1 : 0;
	uint8_t res = (uint8_t)((val << 1) | old_c);
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (new_c) CC |= CC_C;
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if (((val ^ res) & 0x80) != 0) CC |= CC_V;
	return res;
}

static inline uint16_t do_sub16(uint16_t a, uint16_t b)
{
	uint32_t diff = (uint32_t)a - (uint32_t)b;
	uint16_t res = (uint16_t)diff;
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (res == 0) CC |= CC_Z;
	if (res & 0x8000) CC |= CC_N;
	if (((a ^ b) & (a ^ res)) & 0x8000) CC |= CC_V;
	if (diff & 0x10000) CC |= CC_C;
	return res;
}

static inline uint16_t do_subd(uint16_t val)
{
	uint16_t d = get_D();
	uint32_t diff = (uint32_t)d - (uint32_t)val;
	uint16_t res = (uint16_t)diff;
	set_D(res);
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (res == 0) CC |= CC_Z;
	if (res & 0x8000) CC |= CC_N;
	if (((d ^ val) & (d ^ res)) & 0x8000) CC |= CC_V;
	if (diff & 0x10000) CC |= CC_C;
	return res;
}

static inline uint16_t do_addd(uint16_t val)
{
	uint16_t d = get_D();
	uint32_t sum = (uint32_t)d + (uint32_t)val;
	uint16_t res = (uint16_t)sum;
	set_D(res);
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (res == 0) CC |= CC_Z;
	if (res & 0x8000) CC |= CC_N;
	if ((~(d ^ val) & (d ^ res)) & 0x8000) CC |= CC_V;
	if (sum & 0x10000) CC |= CC_C;
	return res;
}

static inline void do_branch(bool condition, const char *name, bool quiet)
{
	int8_t offset = (int8_t)ram[PC++];
	uint16_t target = (uint16_t)(PC + (int16_t)offset);
	if (!quiet)
	{
		printf("\t%s [%02X]\t$%04X", name, (uint8_t)offset, target);
		if (condition) printf(" (taken)"); else printf(" (not taken)");
	}
	if (condition)
	{
		PC = target;
	}
}

static inline void do_lbranch(bool condition, const char *name, bool quiet)
{
	int16_t offset = (int16_t)(((uint16_t)ram[PC] << 8) | ram[PC+1]);
	PC += 2;
	uint16_t target = (uint16_t)(PC + offset);
	if (!quiet)
	{
		printf("\t%s\t$%04X", name, target);
		if (condition) printf(" (taken)"); else printf(" (not taken)");
	}
	if (condition)
	{
		PC = target;
	}
}

// 6809 Stack Push / Pull Helpers
static inline void push_s8(uint8_t val)
{
	S--;
	mem_write(S, val);
}

static inline void push_s16(uint16_t val)
{
	S--;
	mem_write(S, (uint8_t)(val & 0xFF));
	S--;
	mem_write(S, (uint8_t)((val >> 8) & 0xFF));
}

static inline uint8_t pull_s8(void)
{
	return mem_read(S++);
}

static inline uint16_t pull_s16(void)
{
	uint8_t hi = mem_read(S++);
	uint8_t lo = mem_read(S++);
	return ((uint16_t)hi << 8) | lo;
}

static inline void push_u8(uint8_t val)
{
	U--;
	mem_write(U, val);
}

static inline void push_u16(uint16_t val)
{
	U--;
	mem_write(U, (uint8_t)(val & 0xFF));
	U--;
	mem_write(U, (uint8_t)((val >> 8) & 0xFF));
}

static inline uint8_t pull_u8(void)
{
	return mem_read(U++);
}

static inline uint16_t pull_u16(void)
{
	uint8_t hi = mem_read(U++);
	uint8_t lo = mem_read(U++);
	return ((uint16_t)hi << 8) | lo;
}

static void do_pshs(uint8_t mask, char *disasm, size_t maxlen)
{
	char list[64] = "";
	if (mask & 0x80) { push_s16(PC); strcat(list, "PC,"); }
	if (mask & 0x40) { push_s16(U);  strcat(list, "U,"); }
	if (mask & 0x20) { push_s16(Y);  strcat(list, "Y,"); }
	if (mask & 0x10) { push_s16(X);  strcat(list, "X,"); }
	if (mask & 0x08) { push_s8(DP);  strcat(list, "DP,"); }
	if (mask & 0x04) { push_s8(B);   strcat(list, "B,"); }
	if (mask & 0x02) { push_s8(A);   strcat(list, "A,"); }
	if (mask & 0x01) { push_s8(CC);  strcat(list, "CC,"); }
	size_t len = strlen(list);
	if (len > 0 && list[len - 1] == ',') list[len - 1] = '\0';
	if (disasm) snprintf(disasm, maxlen, "%s", list);
}

static void do_puls(uint8_t mask, char *disasm, size_t maxlen)
{
	char list[64] = "";
	if (mask & 0x01) { CC = pull_s8();  strcat(list, "CC,"); }
	if (mask & 0x02) { A  = pull_s8();  strcat(list, "A,"); }
	if (mask & 0x04) { B  = pull_s8();  strcat(list, "B,"); }
	if (mask & 0x08) { DP = pull_s8();  strcat(list, "DP,"); }
	if (mask & 0x10) { X  = pull_s16(); strcat(list, "X,"); }
	if (mask & 0x20) { Y  = pull_s16(); strcat(list, "Y,"); }
	if (mask & 0x40) { U  = pull_s16(); strcat(list, "U,"); }
	if (mask & 0x80) { PC = pull_s16(); strcat(list, "PC,"); }
	size_t len = strlen(list);
	if (len > 0 && list[len - 1] == ',') list[len - 1] = '\0';
	if (disasm) snprintf(disasm, maxlen, "%s", list);
}

static void do_pshu(uint8_t mask, char *disasm, size_t maxlen)
{
	char list[64] = "";
	if (mask & 0x80) { push_u16(PC); strcat(list, "PC,"); }
	if (mask & 0x40) { push_u16(S);  strcat(list, "S,"); }
	if (mask & 0x20) { push_u16(Y);  strcat(list, "Y,"); }
	if (mask & 0x10) { push_u16(X);  strcat(list, "X,"); }
	if (mask & 0x08) { push_u8(DP);  strcat(list, "DP,"); }
	if (mask & 0x04) { push_u8(B);   strcat(list, "B,"); }
	if (mask & 0x02) { push_u8(A);   strcat(list, "A,"); }
	if (mask & 0x01) { push_u8(CC);  strcat(list, "CC,"); }
	size_t len = strlen(list);
	if (len > 0 && list[len - 1] == ',') list[len - 1] = '\0';
	if (disasm) snprintf(disasm, maxlen, "%s", list);
}

static void do_pulu(uint8_t mask, char *disasm, size_t maxlen)
{
	char list[64] = "";
	if (mask & 0x01) { CC = pull_u8();  strcat(list, "CC,"); }
	if (mask & 0x02) { A  = pull_u8();  strcat(list, "A,"); }
	if (mask & 0x04) { B  = pull_u8();  strcat(list, "B,"); }
	if (mask & 0x08) { DP = pull_u8();  strcat(list, "DP,"); }
	if (mask & 0x10) { X  = pull_u16(); strcat(list, "X,"); }
	if (mask & 0x20) { Y  = pull_u16(); strcat(list, "Y,"); }
	if (mask & 0x40) { S  = pull_u16(); strcat(list, "S,"); }
	if (mask & 0x80) { PC = pull_u16(); strcat(list, "PC,"); }
	size_t len = strlen(list);
	if (len > 0 && list[len - 1] == ',') list[len - 1] = '\0';
	if (disasm) snprintf(disasm, maxlen, "%s", list);
}

// 6809 TFR / EXG helpers
static inline const char* get_tfr_reg_name(int r)
{
	switch (r)
	{
		case 0x0: return "D";
		case 0x1: return "X";
		case 0x2: return "Y";
		case 0x3: return "U";
		case 0x4: return "S";
		case 0x5: return "PC";
		case 0x8: return "A";
		case 0x9: return "B";
		case 0xA: return "CC";
		case 0xB: return "DP";
		default: return "?";
	}
}

static inline uint16_t read_tfr_reg(int r)
{
	switch (r)
	{
		case 0x0: return get_D();
		case 0x1: return X;
		case 0x2: return Y;
		case 0x3: return U;
		case 0x4: return S;
		case 0x5: return PC;
		case 0x8: return A;
		case 0x9: return B;
		case 0xA: return CC;
		case 0xB: return DP;
		default: return 0;
	}
}

static inline void write_tfr_reg(int r, uint16_t val)
{
	switch (r)
	{
		case 0x0: set_D(val); break;
		case 0x1: X = val; break;
		case 0x2: Y = val; break;
		case 0x3: U = val; break;
		case 0x4: S = val; break;
		case 0x5: PC = val; break;
		case 0x8: A = (uint8_t)val; break;
		case 0x9: B = (uint8_t)val; break;
		case 0xA: CC = (uint8_t)val; break;
		case 0xB: DP = (uint8_t)val; break;
		default: break;
	}
}

static inline void do_tfr(char *disasm, size_t maxlen)
{
	uint8_t pb = ram[PC++];
	int r1 = (pb >> 4) & 0x0F;
	int r2 = pb & 0x0F;
	uint16_t val = read_tfr_reg(r1);
	write_tfr_reg(r2, val);
	if (disasm) snprintf(disasm, maxlen, "%s,%s", get_tfr_reg_name(r1), get_tfr_reg_name(r2));
}

static inline void do_exg(char *disasm, size_t maxlen)
{
	uint8_t pb = ram[PC++];
	int r1 = (pb >> 4) & 0x0F;
	int r2 = pb & 0x0F;
	uint16_t val1 = read_tfr_reg(r1);
	uint16_t val2 = read_tfr_reg(r2);
	write_tfr_reg(r1, val2);
	write_tfr_reg(r2, val1);
	if (disasm) snprintf(disasm, maxlen, "%s,%s", get_tfr_reg_name(r1), get_tfr_reg_name(r2));
}


typedef struct
{
	int opcode;
	int cycles;
	int bytes;
	char *mnemonic;
	int addressmode;
	char *hexcode;
} Opcode;

static Opcode optable[256];

char *amode[7] = {"NULL      ","IMMEDIATE","DIRECT   ","INDEXED ","EXTENDED","INHERENT","RELATIVE  "};

static inline uint16_t* get_indexed_reg(int rr)
{
	switch (rr)
	{
		case 0: return &X;
		case 1: return &Y;
		case 2: return &U;
		case 3: return &S;
		default: return &X;
	}
}

static inline const char* get_indexed_reg_name(int rr)
{
	switch (rr)
	{
		case 0: return "X";
		case 1: return "Y";
		case 2: return "U";
		case 3: return "S";
		default: return "?";
	}
}

static uint16_t decode_indexed(char *disasm, size_t maxlen)
{
	uint8_t pb = ram[PC++];
	uint16_t ea = 0;

	if ((pb & 0x80) == 0)
	{
		int rr = (pb >> 5) & 0x03;
		uint16_t *reg = get_indexed_reg(rr);
		int8_t offset = (int8_t)(pb & 0x1F);
		if (offset & 0x10)
		{
			offset |= (int8_t)0xE0;
		}
		ea = (uint16_t)(*reg + (int16_t)offset);
		if (disasm)
		{
			snprintf(disasm, maxlen, "%d,%s", offset, get_indexed_reg_name(rr));
		}
		return ea;
	}

	int rr = (pb >> 5) & 0x03;
	uint16_t *reg = get_indexed_reg(rr);
	const char *rname = get_indexed_reg_name(rr);
	bool indirect = (pb & 0x10) != 0;
	int mode = pb & 0x0F;

	switch (mode)
	{
		case 0x00:
			ea = *reg;
			(*reg)++;
			if (disasm) snprintf(disasm, maxlen, ",%s+", rname);
			break;

		case 0x01:
			ea = *reg;
			(*reg) += 2;
			if (disasm) snprintf(disasm, maxlen, indirect ? "[,%s++]" : ",%s++", rname);
			break;

		case 0x02:
			(*reg)--;
			ea = *reg;
			if (disasm) snprintf(disasm, maxlen, ",-%s", rname);
			break;

		case 0x03:
			(*reg) -= 2;
			ea = *reg;
			if (disasm) snprintf(disasm, maxlen, indirect ? "[,--%s]" : ",--%s", rname);
			break;

		case 0x04:
			ea = *reg;
			if (disasm) snprintf(disasm, maxlen, indirect ? "[,%s]" : ",%s", rname);
			break;

		case 0x05:
			ea = (uint16_t)(*reg + (int16_t)(int8_t)B);
			if (disasm) snprintf(disasm, maxlen, indirect ? "[B,%s]" : "B,%s", rname);
			break;

		case 0x06:
			ea = (uint16_t)(*reg + (int16_t)(int8_t)A);
			if (disasm) snprintf(disasm, maxlen, indirect ? "[A,%s]" : "A,%s", rname);
			break;

		case 0x08:
		{
			int8_t off8 = (int8_t)ram[PC++];
			ea = (uint16_t)(*reg + (int16_t)off8);
			if (disasm) snprintf(disasm, maxlen, indirect ? "[%d,%s]" : "%d,%s", off8, rname);
			break;
		}

		case 0x09:
		{
			int16_t off16 = (int16_t)(((uint16_t)ram[PC] << 8) | ram[PC + 1]);
			PC += 2;
			ea = (uint16_t)(*reg + off16);
			if (disasm) snprintf(disasm, maxlen, indirect ? "[$%04X,%s]" : "$%04X,%s", (uint16_t)off16, rname);
			break;
		}

		case 0x0B:
			ea = (uint16_t)(*reg + (int16_t)get_D());
			if (disasm) snprintf(disasm, maxlen, indirect ? "[D,%s]" : "D,%s", rname);
			break;

		case 0x0C:
		{
			int8_t off8 = (int8_t)ram[PC++];
			ea = (uint16_t)(PC + (int16_t)off8);
			if (disasm) snprintf(disasm, maxlen, indirect ? "[$%04X,PCR]" : "$%04X,PCR", ea);
			break;
		}

		case 0x0D:
		{
			int16_t off16 = (int16_t)(((uint16_t)ram[PC] << 8) | ram[PC + 1]);
			PC += 2;
			ea = (uint16_t)(PC + off16);
			if (disasm) snprintf(disasm, maxlen, indirect ? "[$%04X,PCR]" : "$%04X,PCR", ea);
			break;
		}

		case 0x0F:
		{
			ea = ((uint16_t)ram[PC] << 8) | ram[PC + 1];
			PC += 2;
			if (disasm) snprintf(disasm, maxlen, "[$%04X]", ea);
			indirect = true;
			break;
		}

		default:
			ea = *reg;
			if (disasm) snprintf(disasm, maxlen, "?pb=%02X", pb);
			break;
	}

	if (indirect)
	{
		uint16_t indirect_ea = mem_read16(ea);
		return indirect_ea;
	}

	return ea;
}

static void importOpcodes(void)
{
	for (int i = 0; i < 256; i++)
	{
		optable[i].opcode = i;
		optable[i].cycles = 0;
		optable[i].bytes = 0;
		optable[i].mnemonic = "???";
		optable[i].addressmode = 0;
		optable[i].hexcode = "??";
	}
}


static void loadRom(void)
{
	for (int c = 32768; c < 49152; c++)
	{
		ram[c] = rom[c - 32768];
	}
	// Mirror vectors ($BFF0-$BFFF) to high memory ($FFF0-$FFFF)
	for (int c = 0xFFF0; c <= 0xFFFF; c++)
	{
		ram[c] = rom[c - 0xC000];
	}
}

static inline void step_cpu(bool quiet)
{
	bool executed = false;
if (!quiet)
		{
			printf("%04X\t", PC);
		}
		
		
		uint8_t op = ram[PC];
		switch (op)
		{
			// Direct mode single-operand instructions ($00-$0F)
			case 0x00: // NEG direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_neg8(mem_read(addr)));
				if (!quiet) printf("\tNEG [00]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x03: // COM direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_com8(mem_read(addr)));
				if (!quiet) printf("\tCOM [03]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x04: // LSR direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_lsr8(mem_read(addr)));
				if (!quiet) printf("\tLSR [04]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x06: // ROR direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_ror8(mem_read(addr)));
				if (!quiet) printf("\tROR [06]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x07: // ASR direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_asr8(mem_read(addr)));
				if (!quiet) printf("\tASR [07]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x08: // LSL direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_asl8(mem_read(addr)));
				if (!quiet) printf("\tLSL [08]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x09: // ROL direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_rol8(mem_read(addr)));
				if (!quiet) printf("\tROL [09]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x0A: // DEC direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_dec8(mem_read(addr)));
				if (!quiet) printf("\tDEC [0A]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x0C: // INC direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_inc8(mem_read(addr)));
				if (!quiet) printf("\tINC [0C]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x0D: // TST direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				set_flags_logic8(mem_read(addr));
				CC &= ~CC_V;
				if (!quiet) printf("\tTST [0D]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x0E: // JMP direct
			{
				PC++;
				uint16_t target = ((uint16_t)DP << 8) | ram[PC++];
				PC = target;
				if (!quiet) printf("\tJMP [0E]\t$%04X", target);
				executed = true;
				break;
			}
			case 0x0F: // CLR direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, 0);
				set_flags_clr();
				if (!quiet) printf("\tCLR [0F]\t$%04X", addr);
				executed = true;
				break;
			}

			// Page 2 Prefix Byte ($10)
			case 0x10:
			{
				PC++;
				uint8_t op2 = ram[PC++];
				switch (op2)
				{
					// Long branches ($10 $21 to $10 $2F)
					case 0x3F: // SWI2: stack everything, vector $FFF4 (I and F unchanged)
						CC |= CC_E;
						push_s16(PC); push_s16(U); push_s16(Y); push_s16(X);
						push_s8(DP); push_s8(B); push_s8(A); push_s8(CC);
						PC = mem_read16(0xFFF4);
						if (!quiet) printf("\tSWI2 [103F]");
						executed = true;
						break;
					case 0x21: do_lbranch(false, "LBRN", quiet); executed = true; break;
					case 0x22: do_lbranch(!(CC & (CC_C | CC_Z)), "LBHI", quiet); executed = true; break;
					case 0x23: do_lbranch((CC & (CC_C | CC_Z)) != 0, "LBLS", quiet); executed = true; break;
					case 0x24: do_lbranch(!(CC & CC_C), "LBCC", quiet); executed = true; break;
					case 0x25: do_lbranch((CC & CC_C) != 0, "LBCS", quiet); executed = true; break;
					case 0x26: do_lbranch(!(CC & CC_Z), "LBNE", quiet); executed = true; break;
					case 0x27: do_lbranch((CC & CC_Z) != 0, "LBEQ", quiet); executed = true; break;
					case 0x28: do_lbranch(!(CC & CC_V), "LBVC", quiet); executed = true; break;
					case 0x29: do_lbranch((CC & CC_V) != 0, "LBVS", quiet); executed = true; break;
					case 0x2A: do_lbranch(!(CC & CC_N), "LBPL", quiet); executed = true; break;
					case 0x2B: do_lbranch((CC & CC_N) != 0, "LBMI", quiet); executed = true; break;
					case 0x2C: do_lbranch((!(((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))), "LBGE", quiet); executed = true; break;
					case 0x2D: do_lbranch(((((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))), "LBLT", quiet); executed = true; break;
					case 0x2E: do_lbranch((!(((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))) && !(CC & CC_Z), "LBGT", quiet); executed = true; break;
					case 0x2F: do_lbranch(((((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))) || (CC & CC_Z), "LBLE", quiet); executed = true; break;

					// 16-bit compares
					case 0x83: // CMPD immediate
					{
						uint16_t val16 = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						do_sub16(get_D(), val16);
						if (!quiet) printf("\tCMPD [1083]\t#$%04X", val16);
						executed = true;
						break;
					}
					case 0x93: // CMPD direct
					{
						uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
						uint16_t val16 = mem_read16(addr);
						do_sub16(get_D(), val16);
						if (!quiet) printf("\tCMPD [1093]\t$%04X", addr);
						executed = true;
						break;
					}
					case 0xA3: // CMPD indexed
					{
						char disasm[32];
						uint16_t ea = decode_indexed(disasm, sizeof(disasm));
						uint16_t val16 = mem_read16(ea);
						do_sub16(get_D(), val16);
						if (!quiet) printf("\tCMPD [10A3]\t%s", disasm);
						executed = true;
						break;
					}
					case 0xB3: // CMPD extended
					{
						uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						uint16_t val16 = mem_read16(addr);
						do_sub16(get_D(), val16);
						if (!quiet) printf("\tCMPD [10B3]\t$%04X", addr);
						executed = true;
						break;
					}
					case 0x8C: // CMPY immediate
					{
						uint16_t val16 = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						do_sub16(Y, val16);
						if (!quiet) printf("\tCMPY [108C]\t#$%04X", val16);
						executed = true;
						break;
					}
					case 0x9C: // CMPY direct
					{
						uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
						uint16_t val16 = mem_read16(addr);
						do_sub16(Y, val16);
						if (!quiet) printf("\tCMPY [109C]\t$%04X", addr);
						executed = true;
						break;
					}
					case 0xAC: // CMPY indexed
					{
						char disasm[32];
						uint16_t ea = decode_indexed(disasm, sizeof(disasm));
						uint16_t val16 = mem_read16(ea);
						do_sub16(Y, val16);
						if (!quiet) printf("\tCMPY [10AC]\t%s", disasm);
						executed = true;
						break;
					}
					case 0xBC: // CMPY extended
					{
						uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						uint16_t val16 = mem_read16(addr);
						do_sub16(Y, val16);
						if (!quiet) printf("\tCMPY [10BC]\t$%04X", addr);
						executed = true;
						break;
					}

					// LDY
					case 0x8E: // LDY immediate
					{
						Y = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						set_flags_nz16(Y);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDY [108E]\t#$%04X", Y);
						executed = true;
						break;
					}
					case 0x9E: // LDY direct
					{
						uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
						Y = mem_read16(addr);
						set_flags_nz16(Y);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDY [109E]\t$%04X", addr);
						executed = true;
						break;
					}
					case 0xAE: // LDY indexed
					{
						char disasm[32];
						uint16_t ea = decode_indexed(disasm, sizeof(disasm));
						Y = mem_read16(ea);
						set_flags_nz16(Y);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDY [10AE]\t%s", disasm);
						executed = true;
						break;
					}
					case 0xBE: // LDY extended
					{
						uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						Y = mem_read16(addr);
						set_flags_nz16(Y);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDY [10BE]\t$%04X", addr);
						executed = true;
						break;
					}

					// STY
					case 0x9F: // STY direct
					{
						uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
						mem_write16(addr, Y);
						set_flags_nz16(Y);
						CC &= ~CC_V;
						if (!quiet) printf("\tSTY [109F]\t$%04X", addr);
						executed = true;
						break;
					}
					case 0xAF: // STY indexed
					{
						char disasm[32];
						uint16_t ea = decode_indexed(disasm, sizeof(disasm));
						mem_write16(ea, Y);
						set_flags_nz16(Y);
						CC &= ~CC_V;
						if (!quiet) printf("\tSTY [10AF]\t%s", disasm);
						executed = true;
						break;
					}
					case 0xBF: // STY extended
					{
						uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						mem_write16(addr, Y);
						set_flags_nz16(Y);
						CC &= ~CC_V;
						if (!quiet) printf("\tSTY [10BF]\t$%04X", addr);
						executed = true;
						break;
					}

					// LDS
					case 0xCE: // LDS immediate
					{
						S = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						set_flags_nz16(S);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDS [10CE]\t#$%04X", S);
						executed = true;
						break;
					}
					case 0xDE: // LDS direct
					{
						uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
						S = mem_read16(addr);
						set_flags_nz16(S);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDS [10DE]\t$%04X", addr);
						executed = true;
						break;
					}
					case 0xEE: // LDS indexed
					{
						char disasm[32];
						uint16_t ea = decode_indexed(disasm, sizeof(disasm));
						S = mem_read16(ea);
						set_flags_nz16(S);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDS [10EE]\t%s", disasm);
						executed = true;
						break;
					}
					case 0xFE: // LDS extended
					{
						uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						S = mem_read16(addr);
						set_flags_nz16(S);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDS [10FE]\t$%04X", addr);
						executed = true;
						break;
					}

					// STS
					case 0xDF: // STS direct
					{
						uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
						mem_write16(addr, S);
						set_flags_nz16(S);
						CC &= ~CC_V;
						if (!quiet) printf("\tSTS [10DF]\t$%04X", addr);
						executed = true;
						break;
					}
					case 0xEF: // STS indexed
					{
						char disasm[32];
						uint16_t ea = decode_indexed(disasm, sizeof(disasm));
						mem_write16(ea, S);
						set_flags_nz16(S);
						CC &= ~CC_V;
						if (!quiet) printf("\tSTS [10EF]\t%s", disasm);
						executed = true;
						break;
					}
					case 0xFF: // STS extended
					{
						uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						mem_write16(addr, S);
						set_flags_nz16(S);
						CC &= ~CC_V;
						if (!quiet) printf("\tSTS [10FF]\t$%04X", addr);
						executed = true;
						break;
					}

					default:
						if (!quiet) printf("\t*unknown page 2 opcode 10 %02X*", op2);
						break;
				}
				break;
			}

			// Page 3 Prefix Byte ($11)
			case 0x11:
			{
				PC++;
				uint8_t op2 = ram[PC++];
				switch (op2)
				{
					case 0x3F: // SWI3: stack everything, vector $FFF2 (I and F unchanged)
						CC |= CC_E;
						push_s16(PC); push_s16(U); push_s16(Y); push_s16(X);
						push_s8(DP); push_s8(B); push_s8(A); push_s8(CC);
						PC = mem_read16(0xFFF2);
						if (!quiet) printf("\tSWI3 [113F]");
						executed = true;
						break;
					// Page 3 is SWI3 + CMPU/CMPS.  (The first version had CMPU at
					// $8C and CMPS at $8E, and no $83: the ROM's CMPU #$03D9
					// ran its operand as COM <$D9.  Found with an XRoar trace.)
					case 0x83: // CMPU immediate
					{
						uint16_t val16 = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						do_sub16(U, val16);
						if (!quiet) printf("\tCMPU [1183]\t#$%04X", val16);
						executed = true;
						break;
					}
					case 0x8C: // CMPS immediate
					{
						uint16_t val16 = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						do_sub16(S, val16);
						if (!quiet) printf("\tCMPS [118C]\t#$%04X", val16);
						executed = true;
						break;
					}
					case 0x93: // CMPU direct
					{
						uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
						do_sub16(U, mem_read16(addr));
						if (!quiet) printf("\tCMPU [1193]\t<$%02X", addr & 0xFF);
						executed = true;
						break;
					}
					case 0x9C: // CMPS direct
					{
						uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
						do_sub16(S, mem_read16(addr));
						if (!quiet) printf("\tCMPS [119C]\t<$%02X", addr & 0xFF);
						executed = true;
						break;
					}
					case 0xA3: // CMPU indexed
					{
						char disasm[32];
						uint16_t ea = decode_indexed(disasm, sizeof(disasm));
						do_sub16(U, mem_read16(ea));
						if (!quiet) printf("\tCMPU [11A3]\t%s", disasm);
						executed = true;
						break;
					}
					case 0xAC: // CMPS indexed
					{
						char disasm[32];
						uint16_t ea = decode_indexed(disasm, sizeof(disasm));
						do_sub16(S, mem_read16(ea));
						if (!quiet) printf("\tCMPS [11AC]\t%s", disasm);
						executed = true;
						break;
					}
					case 0xB3: // CMPU extended
					{
						uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						do_sub16(U, mem_read16(addr));
						if (!quiet) printf("\tCMPU [11B3]\t$%04X", addr);
						executed = true;
						break;
					}
					case 0xBC: // CMPS extended
					{
						uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						do_sub16(S, mem_read16(addr));
						if (!quiet) printf("\tCMPS [11BC]\t$%04X", addr);
						executed = true;
						break;
					}
					default:
						if (!quiet) printf("\t*unknown page 3 opcode 11 %02X*", op2);
						break;
				}
				break;
			}

			case 0x12: // NOP
			{
				PC++;
				if (!quiet) printf("\tNOP [12]");
				executed = true;
				break;
			}
			case 0x13: // SYNC
			{
				PC++;
				cpu_sync = true;
				if (!quiet) printf("\tSYNC [13]");
				executed = true;
				break;
			}
			case 0x16: // LBRA (Page 1)
			{
				PC++;
				do_lbranch(true, "LBRA", quiet);
				executed = true;
				break;
			}
			case 0x17: // LBSR (Page 1)
			{
				PC++;
				int16_t offset = (int16_t)(((uint16_t)ram[PC] << 8) | ram[PC+1]);
				PC += 2;
				uint16_t target = (uint16_t)(PC + offset);
				push_s16(PC);
				if (!quiet) printf("\tLBSR [17]\t$%04X", target);
				PC = target;
				executed = true;
				break;
			}
			case 0x19: // DAA
			{
				PC++;
				// Decimal Addition Adjust on accumulator A
				uint8_t msn = A >> 4;
				uint8_t lsn = A & 0x0F;
				uint8_t cf = 0;
				if ((CC & CC_C) || msn > 9 || (msn > 8 && lsn > 9)) cf = 0x60;
				if ((CC & CC_H) || lsn > 9) cf |= 0x06;
				uint16_t res = (uint16_t)A + (uint16_t)cf;
				A = (uint8_t)res;
				set_flags_nz8(A);
				if (res & 0x100) CC |= CC_C;
				if (!quiet) printf("\tDAA [19]");
				executed = true;
				break;
			}
			case 0x1A: // ORCC immediate
			{
				PC++;
				uint8_t val = ram[PC++];
				CC |= val;
				if (!quiet) printf("\tORCC [1A]\t#$%02X", val);
				executed = true;
				break;
			}
			case 0x1C: // ANDCC immediate
			{
				PC++;
				uint8_t val = ram[PC++];
				CC &= val;
				if (!quiet) printf("\tANDCC [1C]\t#$%02X", val);
				executed = true;
				break;
			}
			case 0x1D: // SEX (Sign Extend)
			{
				PC++;
				if (B & 0x80) A = 0xFF; else A = 0x00;
				set_flags_nz16(get_D());
				CC &= ~CC_V;
				if (!quiet) printf("\tSEX [1D]");
				executed = true;
				break;
			}
			case 0x1E: // EXG
			{
				PC++;
				char disasm[32];
				do_exg(disasm, sizeof(disasm));
				if (!quiet) printf("\tEXG [1E]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x1F: // TFR
			{
				PC++;
				char disasm[32];
				do_tfr(disasm, sizeof(disasm));
				if (!quiet) printf("\tTFR [1F]\t%s", disasm);
				executed = true;
				break;
			}

			// Page 1 Relative Branches ($20-$2F)
			case 0x20: PC++; do_branch(true, "BRA", quiet); executed = true; break;
			case 0x21: PC++; do_branch(false, "BRN", quiet); executed = true; break;
			case 0x22: PC++; do_branch(!(CC & (CC_C | CC_Z)), "BHI", quiet); executed = true; break;
			case 0x23: PC++; do_branch((CC & (CC_C | CC_Z)) != 0, "BLS", quiet); executed = true; break;
			case 0x24: PC++; do_branch(!(CC & CC_C), "BCC", quiet); executed = true; break;
			case 0x25: PC++; do_branch((CC & CC_C) != 0, "BCS", quiet); executed = true; break;
			case 0x26: PC++; do_branch(!(CC & CC_Z), "BNE", quiet); executed = true; break;
			case 0x27: PC++; do_branch((CC & CC_Z) != 0, "BEQ", quiet); executed = true; break;
			case 0x28: PC++; do_branch(!(CC & CC_V), "BVC", quiet); executed = true; break;
			case 0x29: PC++; do_branch((CC & CC_V) != 0, "BVS", quiet); executed = true; break;
			case 0x2A: PC++; do_branch(!(CC & CC_N), "BPL", quiet); executed = true; break;
			case 0x2B: PC++; do_branch((CC & CC_N) != 0, "BMI", quiet); executed = true; break;
			case 0x2C: PC++; do_branch((!(((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))), "BGE", quiet); executed = true; break;
			case 0x2D: PC++; do_branch(((((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))), "BLT", quiet); executed = true; break;
			case 0x2E: PC++; do_branch((!(((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))) && !(CC & CC_Z), "BGT", quiet); executed = true; break;
			case 0x2F: PC++; do_branch(((((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))) || (CC & CC_Z), "BLE", quiet); executed = true; break;

			// LEA instructions ($30-$33)
			case 0x30: // LEAX
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				X = ea;
				if (X == 0) CC |= CC_Z; else CC &= ~CC_Z;
				if (!quiet) printf("\tLEAX [30]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x31: // LEAY
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				Y = ea;
				if (Y == 0) CC |= CC_Z; else CC &= ~CC_Z;
				if (!quiet) printf("\tLEAY [31]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x32: // LEAS
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				S = ea;
				if (!quiet) printf("\tLEAS [32]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x33: // LEAU
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				U = ea;
				if (!quiet) printf("\tLEAU [33]\t%s", disasm);
				executed = true;
				break;
			}

			// Stack and Subroutine operations ($34-$3B)
			case 0x34: // PSHS
			{
				PC++;
				uint8_t mask = ram[PC++];
				char disasm[64];
				do_pshs(mask, disasm, sizeof(disasm));
				if (!quiet) printf("\tPSHS [34]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x35: // PULS
			{
				PC++;
				uint8_t mask = ram[PC++];
				char disasm[64];
				do_puls(mask, disasm, sizeof(disasm));
				if (!quiet) printf("\tPULS [35]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x36: // PSHU
			{
				PC++;
				uint8_t mask = ram[PC++];
				char disasm[64];
				do_pshu(mask, disasm, sizeof(disasm));
				if (!quiet) printf("\tPSHU [36]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x37: // PULU
			{
				PC++;
				uint8_t mask = ram[PC++];
				char disasm[64];
				do_pulu(mask, disasm, sizeof(disasm));
				if (!quiet) printf("\tPULU [37]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x39: // RTS
			{
				PC++;
				PC = pull_s16();
				if (!quiet) printf("\tRTS [39]\t-> $%04X", PC);
				executed = true;
				break;
			}
			case 0x3A: // ABX (Add B to X unsigned)
			{
				PC++;
				X += (uint16_t)B;
				if (!quiet) printf("\tABX [3A]");
				executed = true;
				break;
			}
			case 0x3B: // RTI (Return from Interrupt)
			{
				PC++;
				CC = pull_s8();
				if (CC & CC_E)
				{
					A = pull_s8();
					B = pull_s8();
					DP = pull_s8();
					X = pull_s16();
					Y = pull_s16();
					U = pull_s16();
				}
				PC = pull_s16();
				if (!quiet) printf("\tRTI [3B]\t-> $%04X", PC);
				executed = true;
				break;
			}
			case 0x3C: // CWAI
			{
				PC++;
				uint8_t val = ram[PC++];
				CC &= val;
				CC |= CC_E;
				push_s16(PC);
				push_s16(U);
				push_s16(Y);
				push_s16(X);
				push_s8(DP);
				push_s8(B);
				push_s8(A);
				push_s8(CC);
				cpu_cwai = true;
				if (!quiet) printf("\tCWAI [3C]");
				executed = true;
				break;
			}
			case 0x3D: // MUL
			{
				PC++;
				uint16_t prod = (uint16_t)A * (uint16_t)B;
				set_D(prod);
				CC &= ~(CC_Z | CC_C);
				if (prod == 0) CC |= CC_Z;
				if (prod & 0x80) CC |= CC_C;
				if (!quiet) printf("\tMUL [3D]");
				executed = true;
				break;
			}
			case 0x3F: // SWI
			{
				PC++;
				CC |= CC_E;
				push_s16(PC);
				push_s16(U);
				push_s16(Y);
				push_s16(X);
				push_s8(DP);
				push_s8(B);
				push_s8(A);
				push_s8(CC);
				CC |= (CC_I | CC_F);
				PC = mem_read16(0xFFFA);	// SWI vector (was $FFF6, the FIRQ vector)
				if (!quiet) printf("\tSWI [3F]\t-> $%04X", PC);
				executed = true;
				break;
			}

			// Inherent A operations ($40-$4F)
			case 0x40: PC++; A = do_neg8(A); if (!quiet) printf("\tNEGA [40]"); executed = true; break;
			case 0x43: PC++; A = do_com8(A); if (!quiet) printf("\tCOMA [43]"); executed = true; break;
			case 0x44: PC++; A = do_lsr8(A); if (!quiet) printf("\tLSRA [44]"); executed = true; break;
			case 0x46: PC++; A = do_ror8(A); if (!quiet) printf("\tRORA [46]"); executed = true; break;
			case 0x47: PC++; A = do_asr8(A); if (!quiet) printf("\tASRA [47]"); executed = true; break;
			case 0x48: PC++; A = do_asl8(A); if (!quiet) printf("\tASLA [48]"); executed = true; break;
			case 0x49: PC++; A = do_rol8(A); if (!quiet) printf("\tROLA [49]"); executed = true; break;
			case 0x4A: PC++; A = do_dec8(A); if (!quiet) printf("\tDECA [4A]"); executed = true; break;
			case 0x4C: PC++; A = do_inc8(A); if (!quiet) printf("\tINCA [4C]"); executed = true; break;
			case 0x4D: PC++; set_flags_logic8(A); CC &= ~CC_V; if (!quiet) printf("\tTSTA [4D]"); executed = true; break;
			case 0x4F: PC++; A = 0; set_flags_clr(); if (!quiet) printf("\tCLRA [4F]"); executed = true; break;

			// Inherent B operations ($50-$5F)
			case 0x50: PC++; B = do_neg8(B); if (!quiet) printf("\tNEGB [50]"); executed = true; break;
			case 0x53: PC++; B = do_com8(B); if (!quiet) printf("\tCOMB [53]"); executed = true; break;
			case 0x54: PC++; B = do_lsr8(B); if (!quiet) printf("\tLSRB [54]"); executed = true; break;
			case 0x56: PC++; B = do_ror8(B); if (!quiet) printf("\tRORB [56]"); executed = true; break;
			case 0x57: PC++; B = do_asr8(B); if (!quiet) printf("\tASRB [57]"); executed = true; break;
			case 0x58: PC++; B = do_asl8(B); if (!quiet) printf("\tASLB [58]"); executed = true; break;
			case 0x59: PC++; B = do_rol8(B); if (!quiet) printf("\tROLB [59]"); executed = true; break;
			case 0x5A: PC++; B = do_dec8(B); if (!quiet) printf("\tDECB [5A]"); executed = true; break;
			case 0x5C: PC++; B = do_inc8(B); if (!quiet) printf("\tINCB [5C]"); executed = true; break;
			case 0x5D: PC++; set_flags_logic8(B); CC &= ~CC_V; if (!quiet) printf("\tTSTB [5D]"); executed = true; break;
			case 0x5F: PC++; B = 0; set_flags_clr(); if (!quiet) printf("\tCLRB [5F]"); executed = true; break;

			// Indexed mode single-operand / control ($60-$6F)
			case 0x60: // NEG indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_neg8(mem_read(ea)));
				if (!quiet) printf("\tNEG [60]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x63: // COM indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_com8(mem_read(ea)));
				if (!quiet) printf("\tCOM [63]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x64: // LSR indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_lsr8(mem_read(ea)));
				if (!quiet) printf("\tLSR [64]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x66: // ROR indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_ror8(mem_read(ea)));
				if (!quiet) printf("\tROR [66]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x67: // ASR indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_asr8(mem_read(ea)));
				if (!quiet) printf("\tASR [67]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x68: // LSL indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_asl8(mem_read(ea)));
				if (!quiet) printf("\tLSL [68]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x69: // ROL indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_rol8(mem_read(ea)));
				if (!quiet) printf("\tROL [69]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x6A: // DEC indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_dec8(mem_read(ea)));
				if (!quiet) printf("\tDEC [6A]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x6C: // INC indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_inc8(mem_read(ea)));
				if (!quiet) printf("\tINC [6C]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x6D: // TST indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				set_flags_logic8(mem_read(ea));
				CC &= ~CC_V;
				if (!quiet) printf("\tTST [6D]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x6E: // JMP indexed
			{
				PC++;
				char disasm[32];
				uint16_t target = decode_indexed(disasm, sizeof(disasm));
				PC = target;
				if (!quiet) printf("\tJMP [6E]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x6F: // CLR indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, 0);
				set_flags_clr();
				if (!quiet) printf("\tCLR [6F]\t%s", disasm);
				executed = true;
				break;
			}

			// Extended mode single-operand / control ($70-$7F)
			case 0x70: // NEG extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_neg8(mem_read(addr)));
				if (!quiet) printf("\tNEG [70]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x73: // COM extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_com8(mem_read(addr)));
				if (!quiet) printf("\tCOM [73]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x74: // LSR extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_lsr8(mem_read(addr)));
				if (!quiet) printf("\tLSR [74]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x76: // ROR extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_ror8(mem_read(addr)));
				if (!quiet) printf("\tROR [76]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x77: // ASR extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_asr8(mem_read(addr)));
				if (!quiet) printf("\tASR [77]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x78: // LSL extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_asl8(mem_read(addr)));
				if (!quiet) printf("\tLSL [78]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x79: // ROL extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_rol8(mem_read(addr)));
				if (!quiet) printf("\tROL [79]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x7A: // DEC extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_dec8(mem_read(addr)));
				if (!quiet) printf("\tDEC [7A]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x7C: // INC extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_inc8(mem_read(addr)));
				if (!quiet) printf("\tINC [7C]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x7D: // TST extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				set_flags_logic8(mem_read(addr));
				CC &= ~CC_V;
				if (!quiet) printf("\tTST [7D]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x7E: // JMP extended
			{
				PC++;
				uint16_t target = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC = target;
				if (!quiet) printf("\tJMP [7E]\t$%04X", target);
				executed = true;
				break;
			}
			case 0x7F: // CLR extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, 0);
				set_flags_clr();
				if (!quiet) printf("\tCLR [7F]\t$%04X", addr);
				executed = true;
				break;
			}

			// Immediate instructions ($80-$8E)
			case 0x80: PC++; A = do_sub8(A, ram[PC++]); if (!quiet) printf("\tSUBA [80]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x81: PC++; do_sub8(A, ram[PC++]); if (!quiet) printf("\tCMPA [81]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x82: PC++; A = do_sbc8(A, ram[PC++]); if (!quiet) printf("\tSBCA [82]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x83: { PC++; uint16_t v = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_subd(v); if (!quiet) printf("\tSUBD [83]\t#$%04X", v); executed = true; break; }
			case 0x84: PC++; A &= ram[PC++]; set_flags_logic8(A); if (!quiet) printf("\tANDA [84]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x85: PC++; do_bit8(A, ram[PC++]); if (!quiet) printf("\tBITA [85]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x86: PC++; A = ram[PC++]; set_flags_nz8(A); CC &= ~CC_V; if (!quiet) printf("\tLDA [86]\t#$%02X", A); executed = true; break;
			case 0x88: PC++; A ^= ram[PC++]; set_flags_logic8(A); if (!quiet) printf("\tEORA [88]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x89: PC++; A = do_adc8(A, ram[PC++]); if (!quiet) printf("\tADCA [89]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x8A: PC++; A |= ram[PC++]; set_flags_logic8(A); if (!quiet) printf("\tORA [8A]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x8B: PC++; A = do_add8(A, ram[PC++]); if (!quiet) printf("\tADDA [8B]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x8C: // CMPX immediate
			{
				PC++;
				uint16_t val16 = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				do_sub16(X, val16);
				if (!quiet) printf("\tCMPX [8C]\t#$%04X", val16);
				executed = true;
				break;
			}
			case 0x8D: // BSR
			{
				PC++;
				int8_t offset = (int8_t)ram[PC++];
				uint16_t target = (uint16_t)(PC + (int16_t)offset);
				push_s16(PC);
				if (!quiet) printf("\tBSR [8D]\t$%04X", target);
				PC = target;
				executed = true;
				break;
			}
			case 0x8E: // LDX immediate
			{
				PC++;
				X = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				set_flags_nz16(X);
				CC &= ~CC_V;
				if (!quiet) printf("\tLDX [8E]\t#$%04X", X);
				executed = true;
				break;
			}

			// Direct instructions ($90-$9F)
			case 0x90: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A = do_sub8(A, mem_read(ea)); if (!quiet) printf("\tSUBA [90]\t$%04X", ea); executed = true; break; }
			case 0x91: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; do_sub8(A, mem_read(ea)); if (!quiet) printf("\tCMPA [91]\t$%04X", ea); executed = true; break; }
			case 0x92: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A = do_sbc8(A, mem_read(ea)); if (!quiet) printf("\tSBCA [92]\t$%04X", ea); executed = true; break; }
			case 0x93: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; do_subd(mem_read16(ea)); if (!quiet) printf("\tSUBD [93]\t$%04X", ea); executed = true; break; }
			case 0x94: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A &= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tANDA [94]\t$%04X", ea); executed = true; break; }
			case 0x95: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; do_bit8(A, mem_read(ea)); if (!quiet) printf("\tBITA [95]\t$%04X", ea); executed = true; break; }
			case 0x96: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A = mem_read(ea); set_flags_nz8(A); CC &= ~CC_V; if (!quiet) printf("\tLDA [96]\t$%04X", ea); executed = true; break; }
			case 0x97: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; mem_write(ea, A); set_flags_nz8(A); CC &= ~CC_V; if (!quiet) printf("\tSTA [97]\t$%04X", ea); executed = true; break; }
			case 0x98: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A ^= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tEORA [98]\t$%04X", ea); executed = true; break; }
			case 0x99: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A = do_adc8(A, mem_read(ea)); if (!quiet) printf("\tADCA [99]\t$%04X", ea); executed = true; break; }
			case 0x9A: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A |= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tORA [9A]\t$%04X", ea); executed = true; break; }
			case 0x9B: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A = do_add8(A, mem_read(ea)); if (!quiet) printf("\tADDA [9B]\t$%04X", ea); executed = true; break; }
			case 0x9C: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; do_sub16(X, mem_read16(ea)); if (!quiet) printf("\tCMPX [9C]\t$%04X", ea); executed = true; break; }
			case 0x9D: // JSR direct
			{
				PC++;
				uint16_t target = ((uint16_t)DP << 8) | ram[PC++];
				push_s16(PC);
				PC = target;
				if (!quiet) printf("\tJSR [9D]\t$%04X", target);
				executed = true;
				break;
			}
			case 0x9E: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; X = mem_read16(ea); set_flags_nz16(X); CC &= ~CC_V; if (!quiet) printf("\tLDX [9E]\t$%04X", ea); executed = true; break; }
			case 0x9F: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; mem_write16(ea, X); set_flags_nz16(X); CC &= ~CC_V; if (!quiet) printf("\tSTX [9F]\t$%04X", ea); executed = true; break; }

			// Indexed instructions ($A0-$AF)
			case 0xA0: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A = do_sub8(A, mem_read(ea)); if (!quiet) printf("\tSUBA [A0]\t%s", dis); executed = true; break; }
			case 0xA1: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); do_sub8(A, mem_read(ea)); if (!quiet) printf("\tCMPA [A1]\t%s", dis); executed = true; break; }
			case 0xA2: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A = do_sbc8(A, mem_read(ea)); if (!quiet) printf("\tSBCA [A2]\t%s", dis); executed = true; break; }
			case 0xA3: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); do_subd(mem_read16(ea)); if (!quiet) printf("\tSUBD [A3]\t%s", dis); executed = true; break; }
			case 0xA4: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A &= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tANDA [A4]\t%s", dis); executed = true; break; }
			case 0xA5: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); do_bit8(A, mem_read(ea)); if (!quiet) printf("\tBITA [A5]\t%s", dis); executed = true; break; }
			case 0xA6: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A = mem_read(ea); set_flags_nz8(A); CC &= ~CC_V; if (!quiet) printf("\tLDA [A6]\t%s", dis); executed = true; break; }
			case 0xA7: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); mem_write(ea, A); set_flags_nz8(A); CC &= ~CC_V; if (!quiet) printf("\tSTA [A7]\t%s", dis); executed = true; break; }
			case 0xA8: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A ^= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tEORA [A8]\t%s", dis); executed = true; break; }
			case 0xA9: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A = do_adc8(A, mem_read(ea)); if (!quiet) printf("\tADCA [A9]\t%s", dis); executed = true; break; }
			case 0xAA: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A |= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tORA [AA]\t%s", dis); executed = true; break; }
			case 0xAB: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A = do_add8(A, mem_read(ea)); if (!quiet) printf("\tADDA [AB]\t%s", dis); executed = true; break; }
			case 0xAC: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); do_sub16(X, mem_read16(ea)); if (!quiet) printf("\tCMPX [AC]\t%s", dis); executed = true; break; }
			case 0xAD: // JSR indexed
			{
				PC++;
				char dis[32];
				uint16_t target = decode_indexed(dis, sizeof(dis));
				push_s16(PC);
				PC = target;
				if (!quiet) printf("\tJSR [AD]\t%s", dis);
				executed = true;
				break;
			}
			case 0xAE: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); X = mem_read16(ea); set_flags_nz16(X); CC &= ~CC_V; if (!quiet) printf("\tLDX [AE]\t%s", dis); executed = true; break; }
			case 0xAF: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); mem_write16(ea, X); set_flags_nz16(X); CC &= ~CC_V; if (!quiet) printf("\tSTX [AF]\t%s", dis); executed = true; break; }

			// Extended instructions ($B0-$BF)
			case 0xB0: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A = do_sub8(A, mem_read(ea)); if (!quiet) printf("\tSUBA [B0]\t$%04X", ea); executed = true; break; }
			case 0xB1: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_sub8(A, mem_read(ea)); if (!quiet) printf("\tCMPA [B1]\t$%04X", ea); executed = true; break; }
			case 0xB2: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A = do_sbc8(A, mem_read(ea)); if (!quiet) printf("\tSBCA [B2]\t$%04X", ea); executed = true; break; }
			case 0xB3: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_subd(mem_read16(ea)); if (!quiet) printf("\tSUBD [B3]\t$%04X", ea); executed = true; break; }
			case 0xB4: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A &= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tANDA [B4]\t$%04X", ea); executed = true; break; }
			case 0xB5: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_bit8(A, mem_read(ea)); if (!quiet) printf("\tBITA [B5]\t$%04X", ea); executed = true; break; }
			case 0xB6: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A = mem_read(ea); set_flags_nz8(A); CC &= ~CC_V; if (!quiet) printf("\tLDA [B6]\t$%04X", ea); executed = true; break; }
			case 0xB7: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; mem_write(ea, A); set_flags_nz8(A); CC &= ~CC_V; if (!quiet) printf("\tSTA [B7]\t$%04X", ea); executed = true; break; }
			case 0xB8: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A ^= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tEORA [B8]\t$%04X", ea); executed = true; break; }
			case 0xB9: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A = do_adc8(A, mem_read(ea)); if (!quiet) printf("\tADCA [B9]\t$%04X", ea); executed = true; break; }
			case 0xBA: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A |= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tORA [BA]\t$%04X", ea); executed = true; break; }
			case 0xBB: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A = do_add8(A, mem_read(ea)); if (!quiet) printf("\tADDA [BB]\t$%04X", ea); executed = true; break; }
			case 0xBC: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_sub16(X, mem_read16(ea)); if (!quiet) printf("\tCMPX [BC]\t$%04X", ea); executed = true; break; }
			case 0xBD: // JSR extended
			{
				PC++;
				uint16_t target = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				push_s16(PC);
				PC = target;
				if (!quiet) printf("\tJSR [BD]\t$%04X", target);
				executed = true;
				break;
			}
			case 0xBE: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; X = mem_read16(ea); set_flags_nz16(X); CC &= ~CC_V; if (!quiet) printf("\tLDX [BE]\t$%04X", ea); executed = true; break; }
			case 0xBF: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; mem_write16(ea, X); set_flags_nz16(X); CC &= ~CC_V; if (!quiet) printf("\tSTX [BF]\t$%04X", ea); executed = true; break; }

			// Immediate instructions ($C0-$CE)
			case 0xC0: PC++; B = do_sub8(B, ram[PC++]); if (!quiet) printf("\tSUBB [C0]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xC1: PC++; do_sub8(B, ram[PC++]); if (!quiet) printf("\tCMPB [C1]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xC2: PC++; B = do_sbc8(B, ram[PC++]); if (!quiet) printf("\tSBCB [C2]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xC3: { PC++; uint16_t v = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_addd(v); if (!quiet) printf("\tADDD [C3]\t#$%04X", v); executed = true; break; }
			case 0xC4: PC++; B &= ram[PC++]; set_flags_logic8(B); if (!quiet) printf("\tANDB [C4]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xC5: PC++; do_bit8(B, ram[PC++]); if (!quiet) printf("\tBITB [C5]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xC6: PC++; B = ram[PC++]; set_flags_nz8(B); CC &= ~CC_V; if (!quiet) printf("\tLDB [C6]\t#$%02X", B); executed = true; break;
			case 0xC8: PC++; B ^= ram[PC++]; set_flags_logic8(B); if (!quiet) printf("\tEORB [C8]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xC9: PC++; B = do_adc8(B, ram[PC++]); if (!quiet) printf("\tADCB [C9]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xCA: PC++; B |= ram[PC++]; set_flags_logic8(B); if (!quiet) printf("\tORB [CA]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xCB: PC++; B = do_add8(B, ram[PC++]); if (!quiet) printf("\tADDB [CB]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xCC: // LDD immediate
			{
				PC++;
				uint16_t val16 = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				set_D(val16);
				set_flags_nz16(val16);
				CC &= ~CC_V;
				if (!quiet) printf("\tLDD [CC]\t#$%04X", val16);
				executed = true;
				break;
			}
			case 0xCE: // LDU immediate
			{
				PC++;
				U = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				set_flags_nz16(U);
				CC &= ~CC_V;
				if (!quiet) printf("\tLDU [CE]\t#$%04X", U);
				executed = true;
				break;
			}

			// Direct instructions ($D0-$DF)
			case 0xD0: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B = do_sub8(B, mem_read(ea)); if (!quiet) printf("\tSUBB [D0]\t$%04X", ea); executed = true; break; }
			case 0xD1: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; do_sub8(B, mem_read(ea)); if (!quiet) printf("\tCMPB [D1]\t$%04X", ea); executed = true; break; }
			case 0xD2: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B = do_sbc8(B, mem_read(ea)); if (!quiet) printf("\tSBCB [D2]\t$%04X", ea); executed = true; break; }
			case 0xD3: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; do_addd(mem_read16(ea)); if (!quiet) printf("\tADDD [D3]\t$%04X", ea); executed = true; break; }
			case 0xD4: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B &= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tANDB [D4]\t$%04X", ea); executed = true; break; }
			case 0xD5: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; do_bit8(B, mem_read(ea)); if (!quiet) printf("\tBITB [D5]\t$%04X", ea); executed = true; break; }
			case 0xD6: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B = mem_read(ea); set_flags_nz8(B); CC &= ~CC_V; if (!quiet) printf("\tLDB [D6]\t$%04X", ea); executed = true; break; }
			case 0xD7: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; mem_write(ea, B); set_flags_nz8(B); CC &= ~CC_V; if (!quiet) printf("\tSTB [D7]\t$%04X", ea); executed = true; break; }
			case 0xD8: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B ^= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tEORB [D8]\t$%04X", ea); executed = true; break; }
			case 0xD9: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B = do_adc8(B, mem_read(ea)); if (!quiet) printf("\tADCB [D9]\t$%04X", ea); executed = true; break; }
			case 0xDA: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B |= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tORB [DA]\t$%04X", ea); executed = true; break; }
			case 0xDB: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B = do_add8(B, mem_read(ea)); if (!quiet) printf("\tADDB [DB]\t$%04X", ea); executed = true; break; }
			case 0xDC: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; uint16_t v = mem_read16(ea); set_D(v); set_flags_nz16(v); CC &= ~CC_V; if (!quiet) printf("\tLDD [DC]\t$%04X", ea); executed = true; break; }
			case 0xDD: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; mem_write16(ea, get_D()); set_flags_nz16(get_D()); CC &= ~CC_V; if (!quiet) printf("\tSTD [DD]\t$%04X", ea); executed = true; break; }
			case 0xDE: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; U = mem_read16(ea); set_flags_nz16(U); CC &= ~CC_V; if (!quiet) printf("\tLDU [DE]\t$%04X", ea); executed = true; break; }
			case 0xDF: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; mem_write16(ea, U); set_flags_nz16(U); CC &= ~CC_V; if (!quiet) printf("\tSTU [DF]\t$%04X", ea); executed = true; break; }

			// Indexed instructions ($E0-$EF)
			case 0xE0: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B = do_sub8(B, mem_read(ea)); if (!quiet) printf("\tSUBB [E0]\t%s", dis); executed = true; break; }
			case 0xE1: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); do_sub8(B, mem_read(ea)); if (!quiet) printf("\tCMPB [E1]\t%s", dis); executed = true; break; }
			case 0xE2: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B = do_sbc8(B, mem_read(ea)); if (!quiet) printf("\tSBCB [E2]\t%s", dis); executed = true; break; }
			case 0xE3: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); do_addd(mem_read16(ea)); if (!quiet) printf("\tADDD [E3]\t%s", dis); executed = true; break; }
			case 0xE4: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B &= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tANDB [E4]\t%s", dis); executed = true; break; }
			case 0xE5: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); do_bit8(B, mem_read(ea)); if (!quiet) printf("\tBITB [E5]\t%s", dis); executed = true; break; }
			case 0xE6: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B = mem_read(ea); set_flags_nz8(B); CC &= ~CC_V; if (!quiet) printf("\tLDB [E6]\t%s", dis); executed = true; break; }
			case 0xE7: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); mem_write(ea, B); set_flags_nz8(B); CC &= ~CC_V; if (!quiet) printf("\tSTB [E7]\t%s", dis); executed = true; break; }
			case 0xE8: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B ^= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tEORB [E8]\t%s", dis); executed = true; break; }
			case 0xE9: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B = do_adc8(B, mem_read(ea)); if (!quiet) printf("\tADCB [E9]\t%s", dis); executed = true; break; }
			case 0xEA: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B |= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tORB [EA]\t%s", dis); executed = true; break; }
			case 0xEB: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B = do_add8(B, mem_read(ea)); if (!quiet) printf("\tADDB [EB]\t%s", dis); executed = true; break; }
			case 0xEC: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); uint16_t v = mem_read16(ea); set_D(v); set_flags_nz16(v); CC &= ~CC_V; if (!quiet) printf("\tLDD [EC]\t%s", dis); executed = true; break; }
			case 0xED: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); mem_write16(ea, get_D()); set_flags_nz16(get_D()); CC &= ~CC_V; if (!quiet) printf("\tSTD [ED]\t%s", dis); executed = true; break; }
			case 0xEE: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); U = mem_read16(ea); set_flags_nz16(U); CC &= ~CC_V; if (!quiet) printf("\tLDU [EE]\t%s", dis); executed = true; break; }
			case 0xEF: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); mem_write16(ea, U); set_flags_nz16(U); CC &= ~CC_V; if (!quiet) printf("\tSTU [EF]\t%s", dis); executed = true; break; }

			// Extended instructions ($F0-$FF)
			case 0xF0: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B = do_sub8(B, mem_read(ea)); if (!quiet) printf("\tSUBB [F0]\t$%04X", ea); executed = true; break; }
			case 0xF1: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_sub8(B, mem_read(ea)); if (!quiet) printf("\tCMPB [F1]\t$%04X", ea); executed = true; break; }
			case 0xF2: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B = do_sbc8(B, mem_read(ea)); if (!quiet) printf("\tSBCB [F2]\t$%04X", ea); executed = true; break; }
			case 0xF3: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_addd(mem_read16(ea)); if (!quiet) printf("\tADDD [F3]\t$%04X", ea); executed = true; break; }
			case 0xF4: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B &= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tANDB [F4]\t$%04X", ea); executed = true; break; }
			case 0xF5: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_bit8(B, mem_read(ea)); if (!quiet) printf("\tBITB [F5]\t$%04X", ea); executed = true; break; }
			case 0xF6: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B = mem_read(ea); set_flags_nz8(B); CC &= ~CC_V; if (!quiet) printf("\tLDB [F6]\t$%04X", ea); executed = true; break; }
			case 0xF7: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; mem_write(ea, B); set_flags_nz8(B); CC &= ~CC_V; if (!quiet) printf("\tSTB [F7]\t$%04X", ea); executed = true; break; }
			case 0xF8: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B ^= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tEORB [F8]\t$%04X", ea); executed = true; break; }
			case 0xF9: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B = do_adc8(B, mem_read(ea)); if (!quiet) printf("\tADCB [F9]\t$%04X", ea); executed = true; break; }
			case 0xFA: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B |= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tORB [FA]\t$%04X", ea); executed = true; break; }
			case 0xFB: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B = do_add8(B, mem_read(ea)); if (!quiet) printf("\tADDB [FB]\t$%04X", ea); executed = true; break; }
			case 0xFC: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; uint16_t v = mem_read16(ea); set_D(v); set_flags_nz16(v); CC &= ~CC_V; if (!quiet) printf("\tLDD [FC]\t$%04X", ea); executed = true; break; }
			case 0xFD: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; mem_write16(ea, get_D()); set_flags_nz16(get_D()); CC &= ~CC_V; if (!quiet) printf("\tSTD [FD]\t$%04X", ea); executed = true; break; }
			case 0xFE: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; U = mem_read16(ea); set_flags_nz16(U); CC &= ~CC_V; if (!quiet) printf("\tLDU [FE]\t$%04X", ea); executed = true; break; }
			case 0xFF: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; mem_write16(ea, U); set_flags_nz16(U); CC &= ~CC_V; if (!quiet) printf("\tSTU [FF]\t$%04X", ea); executed = true; break; }

			default:
			{
				if (quiet)
				{
					printf("\nUnhandled opcode %02X at PC=%04X\n", ram[PC], PC);
				}
				else
				{
					printf("\t\t*default* [%02X]", ram[PC]);
				}
				PC++;
				break;
			}
		}
		
		if (!quiet)
		{
			if (executed)
			{
				printf(" - executed");
			}

			printf("\nA\tB\tD\tX\tY\tU\tS\tCC:EFHINZVC\n");
			printf("%02X\t%02X\t%04X\t%04X\t%04X\t%04X\t%04X\t%02X [%c%c%c%c%c%c%c%c]\n\n\n",
				A, B, get_D(), X, Y, U, S, CC,
				(CC & CC_E) ? 'E' : '.',
				(CC & CC_F) ? 'F' : '.',
				(CC & CC_H) ? 'H' : '.',
				(CC & CC_I) ? 'I' : '.',
				(CC & CC_N) ? 'N' : '.',
				(CC & CC_Z) ? 'Z' : '.',
				(CC & CC_V) ? 'V' : '.',
				(CC & CC_C) ? 'C' : '.');
		}
}

// ---- Cycle timing -----------------------------------------------------------
// Cycles per instruction from the MC6809 datasheet.  0 = not a 6809
// instruction (counted as 2).  Indexed modes add idx_extra(); PSH/PUL add
// one per byte; long branches on page 2 add one when taken; RTI takes 15
// when it pulls the whole register set.

static const uint8_t cyc_page1[256] =
{
	6,0,0,6,6,0,6,6,6,6,6,0,6,6,3,6,	// 0x00 direct
	0,0,2,4,0,0,5,9,0,2,3,0,3,2,8,6,	// 0x10
	3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,	// 0x20 branches
	4,4,4,4,5,5,5,5,0,5,3,6,20,11,0,19,	// 0x30
	2,0,0,2,2,0,2,2,2,2,2,0,2,2,0,2,	// 0x40 A
	2,0,0,2,2,0,2,2,2,2,2,0,2,2,0,2,	// 0x50 B
	6,0,0,6,6,0,6,6,6,6,6,0,6,6,3,6,	// 0x60 indexed
	7,0,0,7,7,0,7,7,7,7,7,0,7,7,4,7,	// 0x70 extended
	2,2,2,4,2,2,2,0,2,2,2,2,4,7,3,0,	// 0x80 immediate
	4,4,4,6,4,4,4,4,4,4,4,4,6,7,5,5,	// 0x90 direct
	4,4,4,6,4,4,4,4,4,4,4,4,6,7,5,5,	// 0xA0 indexed
	5,5,5,7,5,5,5,5,5,5,5,5,7,8,6,6,	// 0xB0 extended
	2,2,2,4,2,2,2,0,2,2,2,2,3,0,3,0,	// 0xC0 immediate
	4,4,4,6,4,4,4,4,4,4,4,4,5,5,5,5,	// 0xD0 direct
	4,4,4,6,4,4,4,4,4,4,4,4,5,5,5,5,	// 0xE0 indexed
	5,5,5,7,5,5,5,5,5,5,5,5,6,6,6,6,	// 0xF0 extended
};

static uint8_t cyc_page2[256], cyc_page3[256];

static void init_cycle_tables(void)
{
	memset(cyc_page2, 0, sizeof(cyc_page2));
	memset(cyc_page3, 0, sizeof(cyc_page3));
	for (int op = 0x21; op <= 0x2F; op++)
	{
		cyc_page2[op] = 5;			// long branches (+1 taken)
	}
	cyc_page2[0x3F] = 20;				// SWI2
	cyc_page2[0x83] = 5; cyc_page2[0x8C] = 5; cyc_page2[0x8E] = 4;	// CMPD CMPY LDY imm
	cyc_page2[0x93] = 7; cyc_page2[0x9C] = 7; cyc_page2[0x9E] = 6; cyc_page2[0x9F] = 6;
	cyc_page2[0xA3] = 7; cyc_page2[0xAC] = 7; cyc_page2[0xAE] = 6; cyc_page2[0xAF] = 6;
	cyc_page2[0xB3] = 8; cyc_page2[0xBC] = 8; cyc_page2[0xBE] = 7; cyc_page2[0xBF] = 7;
	cyc_page2[0xCE] = 4;				// LDS imm
	cyc_page2[0xDE] = 6; cyc_page2[0xDF] = 6;
	cyc_page2[0xEE] = 6; cyc_page2[0xEF] = 6;
	cyc_page2[0xFE] = 7; cyc_page2[0xFF] = 7;

	cyc_page3[0x3F] = 20;				// SWI3
	cyc_page3[0x83] = 5; cyc_page3[0x8C] = 5;	// CMPU CMPS imm
	cyc_page3[0x93] = 7; cyc_page3[0x9C] = 7;
	cyc_page3[0xA3] = 7; cyc_page3[0xAC] = 7;
	cyc_page3[0xB3] = 8; cyc_page3[0xBC] = 8;
}

// Extra cycles for an indexed postbyte (datasheet table "Indexed addressing")
static int idx_extra(uint8_t pb)
{
	static const uint8_t extra[16] = { 2, 3, 2, 3, 0, 1, 1, 0, 1, 4, 0, 4, 1, 5, 0, 5 };
	if (!(pb & 0x80))
	{
		return 1;			// 5-bit offset
	}
	if (pb & 0x10)				// indirect
	{
		return (pb & 0x0F) == 0x0F ? 5 : extra[pb & 0x0F] + 3;
	}
	return extra[pb & 0x0F];
}

static int push_bytes(uint8_t mask)
{
	static const uint8_t size[8] = { 1, 1, 1, 1, 2, 2, 2, 2 };	// CC A B DP X Y U/S PC
	int n = 0;
	for (int b = 0; b < 8; b++)
	{
		if (mask & (1 << b))
		{
			n += size[b];
		}
	}
	return n;
}

static void push_entire_state(void)
{
	push_s16(PC); push_s16(U); push_s16(Y); push_s16(X);
	push_s8(DP); push_s8(B); push_s8(A); push_s8(CC);
}

// One step: take a pending interrupt, wait (SYNC/CWAI), or run one
// instruction.  Returns the CPU cycles used.
static int cpu_step(void)
{
	bool irq = pia_irq(&pia0);	// PIA0 -> IRQ
	bool firq = pia_irq(&pia1);	// PIA1 -> FIRQ

	if (cpu_sync)
	{
		if (!irq && !firq)
		{
			return 4;		// waiting for an interrupt line
		}
		cpu_sync = false;		// any line ends SYNC, even if masked
	}

	if (firq && !(CC & CC_F))
	{
		if (!cpu_cwai)
		{
			CC &= (uint8_t)~CC_E;
			push_s16(PC);
			push_s8(CC);
		}
		cpu_cwai = false;
		CC |= CC_I | CC_F;
		PC = mem_read16(0xFFF6);
		return 10;
	}
	if (irq && !(CC & CC_I))
	{
		if (!cpu_cwai)
		{
			CC |= CC_E;
			push_entire_state();
		}
		cpu_cwai = false;
		CC |= CC_I;
		PC = mem_read16(0xFFF8);
		return 19;
	}
	if (cpu_cwai)
	{
		return 4;			// stacked, waiting for an unmasked interrupt
	}

	uint16_t pc0 = PC;
	uint8_t op = ram[pc0];
	int cycles;
	bool long_branch = false;
	if (op == 0x10 || op == 0x11)
	{
		uint8_t op2 = ram[(uint16_t)(pc0 + 1)];
		cycles = (op == 0x10 ? cyc_page2 : cyc_page3)[op2];
		if (cycles == 0)
		{
			cycles = 2;
		}
		uint8_t mode = op2 & 0xF0;
		if (mode == 0xA0 || (op == 0x10 && mode == 0xE0))
		{
			cycles += idx_extra(ram[(uint16_t)(pc0 + 2)]);
		}
		long_branch = op == 0x10 && op2 >= 0x22 && op2 <= 0x2F;
	}
	else
	{
		cycles = cyc_page1[op] ? cyc_page1[op] : 2;
		uint8_t mode = op & 0xF0;
		if ((op >= 0x30 && op <= 0x33) || mode == 0x60 || mode == 0xA0 || mode == 0xE0)
		{
			cycles += idx_extra(ram[(uint16_t)(pc0 + 1)]);
		}
		else if (op >= 0x34 && op <= 0x37)
		{
			cycles += push_bytes(ram[(uint16_t)(pc0 + 1)]);
		}
	}

	step_cpu(true);

	if (long_branch && PC != (uint16_t)(pc0 + 4))
	{
		cycles += 1;
	}
	if (op == 0x3B && (CC & CC_E))
	{
		cycles += 9;			// RTI pulled the whole register set
	}
	return cycles;
}

// ---- Sound ------------------------------------------------------------------
// The Dragon's audio is a mux (PIA0 CA2/CB2 pick DAC, tape, cartridge or
// none; PIA1 CB2 switches it on) plus single-bit sound on PIA1 PB1.  Gains
// and offsets are the voltages XRoar's author measured on a real Dragon
// (max 4.70 V), indexed by single-bit sound: off, on+low, on+high.

static const float mux_gain[4][3] =
{
	{ 4.50f / 4.70f, 2.84f / 4.70f, 3.40f / 4.70f },	// DAC
	{ 0.50f / 4.70f, 0.40f / 4.70f, 0.50f / 4.70f },	// tape
	{ 4.70f / 4.70f, 2.84f / 4.70f, 3.40f / 4.70f },	// cartridge
	{ 0, 0, 0 },						// none
};
static const float mux_offset[4][3] =
{
	{ 0.20f / 4.70f, 0.18f / 4.70f, 1.30f / 4.70f },
	{ 2.05f / 4.70f, 1.60f / 4.70f, 2.35f / 4.70f },
	{ 0.00f / 4.70f, 0.18f / 4.70f, 1.30f / 4.70f },
	{ 0, 0, 0 },
};
static const float sbs_offset[3] = { 0, 0, 3.90f / 4.70f };

#define AUDIO_BUF	4096

static float audio_level;		// bus level now, 0..1
static unsigned audio_rate;		// samples per second, 0 = off
static unsigned audio_per_frame;	// samples per 50 Hz frame
static unsigned audio_phase;		// position in the current sample (cycles x samples)
static float audio_sum;			// level x time in the current sample
static float hp_in, hp_out;		// high-pass (the TV's coupling capacitor)
static int16_t audio_buf[AUDIO_BUF];
static unsigned audio_count;

static void audio_update_level(void)
{
	bool sbs_on = (pia1.ddrb & 0x02) != 0;			// PB1 is an output
	bool sbs_high = (pia1.orb & 0x02) != 0;
	unsigned sindex = sbs_on ? (sbs_high ? 2 : 1) : 0;

	if (pia_cx2(pia1.crb))					// PIA1 CB2: sound mux on
	{
		unsigned source = (pia_cx2(pia0.crb) ? 2 : 0) | (pia_cx2(pia0.cra) ? 1 : 0);
		float raw = source == 0 ? (float)(pia_out_a(&pia1) & 0xFC) / 252.0f
			  : source == 1 ? (tape_input_level() ? 1.0f : 0.0f) : 0.0f;	// 1 = tape (AUDIO ON)
		audio_level = raw * mux_gain[source][sindex] + mux_offset[source][sindex];
	}
	else if (sbs_on)
	{
		audio_level = sbs_offset[sindex];		// single-bit sound only
	}
	// else: nothing drives the audio line; it holds its level
}

// The level held for this many cycles: add it to the samples it covers
static void audio_run(int cycles, float level)
{
	if (audio_rate == 0)
	{
		return;
	}
	unsigned units = (unsigned)cycles * audio_per_frame;	// in (cycles x samples per frame)
	while (audio_phase + units >= DRAGON_FRAME_CYCLES)
	{
		unsigned take = DRAGON_FRAME_CYCLES - audio_phase;
		audio_sum += level * (float)take;
		float x = audio_sum / (float)DRAGON_FRAME_CYCLES;
		hp_out = 0.995f * (hp_out + x - hp_in);		// ~40 Hz high-pass at 48 kHz
		hp_in = x;
		float y = hp_out * 30000.0f;
		if (y > 32767.0f) y = 32767.0f;
		if (y < -32768.0f) y = -32768.0f;
		if (audio_count < AUDIO_BUF)
		{
			audio_buf[audio_count++] = (int16_t)y;
		}
		audio_sum = 0;
		audio_phase = 0;
		units -= take;
	}
	audio_phase += units;
	audio_sum += level * (float)units;
}

// ---- Platform API (dragon.h) ---------------------------------------------

static unsigned frame_cycle;		// cycles into this 50 Hz frame
static uint64_t total_cycles;
static unsigned irq_count;		// IRQs taken (tests)

void dragon_init(const uint8_t *rom_image)
{
	importOpcodes();
	init_cycle_tables();
	memcpy(rom, rom_image, sizeof(rom));
	memset(ram, 0, sizeof(ram));
	loadRom();
	A = B = DP = 0;
	X = Y = U = S = 0;
	CC = CC_I | CC_F;	// interrupts masked after reset
	pia_reset(&pia0);
	pia_reset(&pia1);
	cpu_sync = cpu_cwai = false;
	kbd_clear_matrix();
	kbd_q_head = kbd_q_tail = 0;
	kbd_hold_cycles = kbd_release_cycles = 0;
	frame_cycle = 0;
	total_cycles = 0;
	audio_level = 0;
	audio_count = 0;
	tape_reset();
	dragon_reset();
}

void dragon_reset(void)
{
	// The reset button resets the CPU and the PIAs (RAM is kept)
	pia_reset(&pia0);
	pia_reset(&pia1);
	sam_reg = 0;
	tape_set_motor(false);
	cpu_sync = cpu_cwai = false;
	CC = CC_I | CC_F;
	DP = 0;
	// 6809 Reset Vector at $FFFE-$FFFF
	PC = ((uint16_t)ram[0xFFFE] << 8) | ram[0xFFFF];
}

// The VDG's sync pulses: HS on PIA0 CA1 every line (57 cycles), FS on
// PIA0 CB1 once a frame (312 lines, PAL).  FS falls as the picture ends
// and rises again 32 lines later; BASIC's 50 Hz IRQ is on that rise.
#define FS_FALL_LINE	0
#define FS_RISE_LINE	32

void dragon_run_frame(void)
{
	unsigned line = frame_cycle / DRAGON_LINE_CYCLES;
	unsigned next_line = (line + 1) * DRAGON_LINE_CYCLES;
	if (line == 0)
	{
		pia_set_cx1(&pia0.crb, &pia0.cb1, false);	// FS falls
	}

	while (frame_cycle < DRAGON_FRAME_CYCLES)
	{
		bool in_irq = (CC & CC_I) == 0 && pia_irq(&pia0);
		float level = audio_level;
		int cycles = cpu_step();
		if (in_irq && (CC & CC_I))
		{
			irq_count++;
		}
		audio_run(cycles, level);
		tape_run(cycles, (pia_out_a(&pia1) >> 2) & 63);
		if (dragon_tape_motor())
		{
			audio_update_level();	// the tape can be heard through the mux
		}
		kbd_update_cycles(cycles);
		frame_cycle += (unsigned)cycles;
		total_cycles += (unsigned)cycles;

		while (frame_cycle >= next_line && line < DRAGON_FRAME_LINES)
		{
			line++;
			next_line += DRAGON_LINE_CYCLES;
			pia_set_cx1(&pia0.cra, &pia0.ca1, false);	// HS pulse
			pia_set_cx1(&pia0.cra, &pia0.ca1, true);
			if (line == FS_RISE_LINE)
			{
				pia_set_cx1(&pia0.crb, &pia0.cb1, true);	// FS rises: 50 Hz IRQ
			}
		}
	}
	frame_cycle -= DRAGON_FRAME_CYCLES;	// carry the overshoot into the next frame
}

unsigned dragon_sam(void)
{
	return sam_reg;
}

uint64_t dragon_cycles(void)
{
	return total_cycles;
}

unsigned dragon_irqs(void)
{
	return irq_count;
}

void dragon_audio_rate(unsigned rate)
{
	audio_rate = rate;
	audio_per_frame = rate / 50;
	audio_phase = 0;
	audio_sum = 0;
	audio_count = 0;
}

unsigned dragon_audio_take(int16_t *buf, unsigned max)
{
	unsigned n = audio_count < max ? audio_count : max;
	memcpy(buf, audio_buf, n * sizeof(int16_t));
	memmove(audio_buf, audio_buf + n, (audio_count - n) * sizeof(int16_t));
	audio_count -= n;
	return n;
}

const uint8_t *dragon_memory(void)
{
	return ram;
}

void dragon_regs(DragonRegs *regs)
{
	regs->pc = PC; regs->x = X; regs->y = Y; regs->u = U; regs->s = S;
	regs->a = A; regs->b = B; regs->dp = DP; regs->cc = CC;
	regs->wait = cpu_sync ? 1 : (cpu_cwai ? 2 : 0);
}

void dragon_set_matrix(const uint8_t matrix[8])
{
	// Typed keys own the matrix until they are all done
	if (dragon_typing())
	{
		return;
	}
	memcpy(key_matrix, matrix, sizeof(key_matrix));
}

void dragon_type(const char *text)
{
	kbd_enqueue_string(text);
}

bool dragon_typing(void)
{
	return kbd_q_head != kbd_q_tail || kbd_hold_cycles > 0 || kbd_release_cycles > 0;
}

bool dragon_char_key(char c, int *row, int *col, bool *shift)
{
	return kbd_map_char(c, row, col, shift);
}
