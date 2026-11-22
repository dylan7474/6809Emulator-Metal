/*
 * vdg.c - Motorola MC6847 video (text + semigraphics-4) for the Dragon 32
 *
 * Copyright (C) 2026 Dylan Jones
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The 6847 draws 32 x 16 character cells of 8 x 12 pixels (256 x 192).
 * Each byte of video RAM at $0400 is one cell:
 *
 *   bit 7 = 0  text: bits 0-5 = character (@ A-Z [ \ ] up left, then
 *              space ! " ... ?), bit 6 = 0 -> inverse (light on dark)
 *   bit 7 = 1  semigraphics-4: bits 4-6 = colour, bits 0-3 = which
 *              quarter of the cell is lit (3 top-left, 2 top-right,
 *              1 bottom-left, 0 bottom-right); unlit quarters are black
 *
 * PIA1 port B ($FF22) bit 3 is the colour set (CSS): green text, or
 * orange text.  Bit 7 (A/G) switches to the graphics modes (bits 6-4 =
 * GM2-GM0, see gmode[] below).  The SAM decides which bytes each line
 * shows (start = its F0-F6 bits x 512, rows from its V0-V2 bits); that
 * is also how semigraphics-8/12/24 work.  Bit 4 (GM0) in text mode gives
 * semigraphics-6.  Behaviour checked against XRoar's MC6847/MC6883
 * models (colours, SG6, border).
 *
 * The font is drawn here to match the 6847's internal 5 x 7 characters
 * (placed at pixel columns 2-6, rows 3-9 of each cell).
 */
#include "dragon.h"
#include <string.h>

const uint8_t vdg_palette[VDG_COLOURS][3] =
{
	{  48, 210,   0 },	// green
	{ 255, 255,  64 },	// yellow
	{  32,  32, 255 },	// blue
	{ 224,  24,  24 },	// red
	{ 236, 236, 236 },	// buff
	{  24, 216, 216 },	// cyan
	{ 224,  24, 224 },	// magenta
	{ 255, 132,   0 },	// orange
	{   0,   0,   0 },	// black
	{   0,  60,   0 },	// dark green (text on green)
	{  80,  32,   0 },	// dark orange (text on orange)
};

// 64 characters, 7 rows each, 5 dots per row ('#' = dot)
static const char *const font_src[64][7] =
{
	{ " ### ", "#   #", "#  ##", "# # #", "# ## ", "#    ", " ####" },	// @
	{ "  #  ", " # # ", "#   #", "#   #", "#####", "#   #", "#   #" },	// A
	{ "#### ", " #  #", " #  #", " ### ", " #  #", " #  #", "#### " },	// B
	{ " ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### " },	// C
	{ "#### ", " #  #", " #  #", " #  #", " #  #", " #  #", "#### " },	// D
	{ "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####" },	// E
	{ "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    " },	// F
	{ " ####", "#    ", "#    ", "#  ##", "#   #", "#   #", " ####" },	// G
	{ "#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #" },	// H
	{ " ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### " },	// I
	{ "    #", "    #", "    #", "    #", "#   #", "#   #", " ### " },	// J
	{ "#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #" },	// K
	{ "#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####" },	// L
	{ "#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #" },	// M
	{ "#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #" },	// N
	{ "#####", "#   #", "#   #", "#   #", "#   #", "#   #", "#####" },	// O
	{ "#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    " },	// P
	{ " ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #" },	// Q
	{ "#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #" },	// R
	{ " ### ", "#   #", " #   ", "  #  ", "   # ", "#   #", " ### " },	// S
	{ "#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  " },	// T
	{ "#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### " },	// U
	{ "#   #", "#   #", "#   #", " # # ", " # # ", "  #  ", "  #  " },	// V
	{ "#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #" },	// W
	{ "#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #" },	// X
	{ "#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  " },	// Y
	{ "#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####" },	// Z
	{ " ### ", " #   ", " #   ", " #   ", " #   ", " #   ", " ### " },	// [
	{ "     ", "#    ", " #   ", "  #  ", "   # ", "    #", "     " },	// backslash
	{ " ### ", "   # ", "   # ", "   # ", "   # ", "   # ", " ### " },	// ]
	{ "  #  ", " ### ", "# # #", "  #  ", "  #  ", "  #  ", "  #  " },	// up arrow
	{ "     ", "  #  ", " #   ", "#####", " #   ", "  #  ", "     " },	// left arrow
	{ "     ", "     ", "     ", "     ", "     ", "     ", "     " },	// space
	{ "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  " },	// !
	{ " # # ", " # # ", " # # ", "     ", "     ", "     ", "     " },	// "
	{ " # # ", " # # ", "#####", " # # ", "#####", " # # ", " # # " },	// #
	{ "  #  ", " ####", "# #  ", " ### ", "  # #", "#### ", "  #  " },	// $
	{ "##   ", "##  #", "   # ", "  #  ", " #   ", "#  ##", "   ##" },	// %
	{ " #   ", "# #  ", "# #  ", " #   ", "# # #", "#  # ", " ## #" },	// &
	{ "  #  ", "  #  ", " #   ", "     ", "     ", "     ", "     " },	// '
	{ "   # ", "  #  ", " #   ", " #   ", " #   ", "  #  ", "   # " },	// (
	{ " #   ", "  #  ", "   # ", "   # ", "   # ", "  #  ", " #   " },	// )
	{ "     ", "  #  ", "# # #", " ### ", "# # #", "  #  ", "     " },	// *
	{ "     ", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "     " },	// +
	{ "     ", "     ", "     ", "     ", " ##  ", "  #  ", " #   " },	// ,
	{ "     ", "     ", "     ", "#####", "     ", "     ", "     " },	// -
	{ "     ", "     ", "     ", "     ", "     ", " ##  ", " ##  " },	// .
	{ "     ", "    #", "   # ", "  #  ", " #   ", "#    ", "     " },	// /
	{ " ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### " },	// 0
	{ "  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### " },	// 1
	{ " ### ", "#   #", "    #", "  ## ", " #   ", "#    ", "#####" },	// 2
	{ " ### ", "#   #", "    #", "  ## ", "    #", "#   #", " ### " },	// 3
	{ "   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # " },	// 4
	{ "#####", "#    ", "#### ", "    #", "    #", "#   #", " ### " },	// 5
	{ " ### ", "#    ", "#    ", "#### ", "#   #", "#   #", " ### " },	// 6
	{ "#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#    " },	// 7
	{ " ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### " },	// 8
	{ " ### ", "#   #", "#   #", " ####", "    #", "    #", " ### " },	// 9
	{ "     ", " ##  ", " ##  ", "     ", " ##  ", " ##  ", "     " },	// :
	{ "     ", " ##  ", " ##  ", "     ", " ##  ", "  #  ", " #   " },	// ;
	{ "   # ", "  #  ", " #   ", "#    ", " #   ", "  #  ", "   # " },	// <
	{ "     ", "     ", "#####", "     ", "#####", "     ", "     " },	// =
	{ " #   ", "  #  ", "   # ", "    #", "   # ", "  #  ", " #   " },	// >
	{ " ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  " },	// ?
};

// font[ch][row]: 12 rows of 8 pixels, bit 7 = leftmost
static uint8_t font[64][12];
static bool font_ready;

static void build_font(void)
{
	memset(font, 0, sizeof(font));
	for (int ch = 0; ch < 64; ch++)
	{
		for (int r = 0; r < 7; r++)
		{
			uint8_t bits = 0;
			for (int c = 0; c < 5; c++)
			{
				if (font_src[ch][r][c] == '#')
				{
					bits |= (uint8_t)(0x40 >> c);	// columns 1-5 of 0-7 -> bit 6..2
				}
			}
			font[ch][r + 3] = bits;
		}
	}
	font_ready = true;
}

// Where the picture starts: the SAM's F0-F6 bits x 512 ($0400 for BASIC)
static unsigned video_base(void)
{
	return ((dragon_sam() >> 3) & 0x7F) * 512u;
}

// How the SAM walks memory for the VDG, by its V0-V2 bits: bytes per
// memory row and scanlines each row is shown for (the MC6883 data sheet's
// display modes: 1K, 1.5K, 2K, 3K or 6K).  The VDG decides how the bytes
// are drawn.  Text with V = 2, 4 or 6 is how SG8, SG12 and SG24 work: the
// VDG keeps counting its 12-line character rows while the SAM moves to a
// new row of bytes every 3, 2 or 1 lines.
static const struct { uint8_t bytes, lines; } sam_mode[8] =
{
	{ 32, 12 },	// 0 text, SG4, SG6
	{ 16, 3 },	// 1 CG1, RG1
	{ 32, 3 },	// 2 CG2, SG8
	{ 16, 2 },	// 3 RG2
	{ 32, 2 },	// 4 CG3, SG12
	{ 16, 1 },	// 5 RG3
	{ 32, 1 },	// 6 CG6, RG6, SG24
	{ 32, 1 },	// 7 (DMA)
};

// The eight graphics modes, by GM2-GM0 (PIA1 PB6-PB4) with A/G set:
// bytes the VDG reads per line, bits per pixel, dots per pixel across
static const struct { uint8_t bytes, bpp, dot_w; } gmode[8] =
{
	{ 16, 2, 4 },	// 0 CG1  64 x 64,  4 colours
	{ 16, 1, 2 },	// 1 RG1 128 x 64,  2 colours
	{ 32, 2, 2 },	// 2 CG2 128 x 64,  4 colours
	{ 16, 1, 2 },	// 3 RG2 128 x 96,  2 colours  (PMODE 0)
	{ 32, 2, 2 },	// 4 CG3 128 x 96,  4 colours  (PMODE 1)
	{ 16, 1, 2 },	// 5 RG3 128 x 192, 2 colours  (PMODE 2)
	{ 32, 2, 2 },	// 6 CG6 128 x 192, 4 colours  (PMODE 3)
	{ 32, 1, 1 },	// 7 RG6 256 x 192, 2 colours  (PMODE 4)
};

uint8_t vdg_border(const uint8_t *memory)
{
	// Black around text; the colour set's first colour around graphics
	uint8_t vdg_mode = memory[0xFF22];
	if (!(vdg_mode & 0x80))
	{
		return VDG_BLACK;
	}
	return (vdg_mode & 0x08) ? VDG_BUFF : VDG_GREEN;
}

static void render_graphics_line(const uint8_t *row, unsigned mode, bool css, uint8_t *out)
{
	unsigned bytes = gmode[mode].bytes, bpp = gmode[mode].bpp, dot_w = gmode[mode].dot_w;
	// 4 colours: green yellow blue red, or (CSS) buff cyan magenta orange.
	// 2 colours: green on dark green, or (CSS) buff on black.
	uint8_t first = css ? VDG_BUFF : VDG_GREEN;
	uint8_t off = css ? VDG_BLACK : VDG_DARK_GREEN;
	unsigned x = 0;
	for (unsigned i = 0; i < bytes; i++)
	{
		uint8_t v = row[i];
		if (bpp == 2)
		{
			for (int sh = 6; sh >= 0; sh -= 2)
			{
				uint8_t c = (uint8_t)(first + ((v >> sh) & 3));
				for (unsigned d = 0; d < dot_w; d++)
				{
					out[x++] = c;
				}
			}
		}
		else
		{
			for (int sh = 7; sh >= 0; sh--)
			{
				uint8_t c = (v >> sh) & 1 ? first : off;
				for (unsigned d = 0; d < dot_w; d++)
				{
					out[x++] = c;
				}
			}
		}
	}
}

// One scanline of text / semigraphics: r = the VDG's line within its
// 12-line character row.  GM0 is the VDG's INT/EXT line on the Dragon:
// with it set, bytes with bit 7 are semigraphics-6 and others are shown
// as their own bit pattern (no external character ROM is fitted).
static void render_text_line(const uint8_t *row, unsigned r, bool css, bool ext, uint8_t *out)
{
	uint8_t text_light = css ? VDG_ORANGE : VDG_GREEN;
	uint8_t text_dark = css ? VDG_DARK_ORANGE : VDG_DARK_GREEN;
	for (unsigned cx = 0; cx < 32; cx++, out += 8)
	{
		uint8_t v = row[cx];
		if (v & 0x80)
		{
			unsigned bits;
			uint8_t colour;
			if (ext)
			{
				// Semigraphics-6: 2 x 3 blocks (bits 5-0, top left first);
				// colour 2 or 3 of the colour set, by bit 6
				bits = (v >> (r < 4 ? 4 : r < 8 ? 2 : 0)) & 3;
				colour = (uint8_t)((css ? VDG_BUFF : VDG_GREEN) + ((v >> 6) & 3));
			}
			else
			{
				// Semigraphics-4: 2 x 2 blocks, colour in bits 6-4
				bits = (r < 6 ? v >> 2 : v) & 3;
				colour = (v >> 4) & 0x07;
			}
			uint8_t lc = (bits & 2) ? colour : VDG_BLACK;
			uint8_t rc = (bits & 1) ? colour : VDG_BLACK;
			out[0] = out[1] = out[2] = out[3] = lc;
			out[4] = out[5] = out[6] = out[7] = rc;
		}
		else
		{
			bool inverse = (v & 0x40) == 0;
			uint8_t ink = inverse ? text_light : text_dark;
			uint8_t paper = inverse ? text_dark : text_light;
			uint8_t bits = ext ? v : font[v & 0x3F][r];
			for (int x = 0; x < 8; x++)
			{
				out[x] = (bits & (0x80 >> x)) ? ink : paper;
			}
		}
	}
}

void vdg_render(const uint8_t *memory, uint8_t picture[VDG_HEIGHT][VDG_WIDTH])
{
	if (!font_ready)
	{
		build_font();
	}
	uint8_t vdg_mode = memory[0xFF22];		// PIA1 port B: A/G GM2 GM1 GM0 CSS
	bool graphics = (vdg_mode & 0x80) != 0;
	bool css = (vdg_mode & 0x08) != 0;
	bool gm0 = (vdg_mode & 0x10) != 0;
	unsigned gm = (vdg_mode >> 4) & 7;
	unsigned base = video_base();
	unsigned v = dragon_sam() & 7;
	unsigned row_bytes = sam_mode[v].bytes, row_lines = sam_mode[v].lines;

	for (unsigned y = 0; y < VDG_HEIGHT; y++)
	{
		// The bytes for this line, from the SAM (wrapping at 64K)
		uint8_t row[32];
		unsigned addr = base + (y / row_lines) * row_bytes;
		for (unsigned i = 0; i < 32; i++)
		{
			row[i] = memory[(addr + i) & 0xFFFF];
		}
		if (graphics)
		{
			render_graphics_line(row, gm, css, picture[y]);
		}
		else
		{
			render_text_line(row, y % 12, css, gm0, picture[y]);
		}
	}
}

uint8_t vdg_font_row(char c, int row)
{
	static const char order[] =
		"@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_ !\"#$%&'()*+,-./0123456789:;<=>?";
	if (!font_ready)
	{
		build_font();
	}
	if (c >= 'a' && c <= 'z')
	{
		c = (char)(c - 32);
	}
	const char *p = c ? strchr(order, c) : NULL;
	if (p == NULL || row < 0 || row > 11)
	{
		return 0;
	}
	return font[p - order][row];
}

void vdg_text(const uint8_t *memory, char *buf)
{
	// Character codes 0-63 in the 6847's order, as ASCII
	static const char ascii[] =
		"@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_ !\"#$%&'()*+,-./0123456789:;<=>?";
	const uint8_t *vram = memory + video_base();
	char *p = buf;
	for (int row = 0; row < 16; row++)
	{
		for (int col = 0; col < 32; col++)
		{
			uint8_t v = vram[row * 32 + col];
			*p++ = (v & 0x80) ? '#' : ascii[v & 0x3F];	// '#' = a block graphic
		}
		*p++ = '\n';
	}
	*p = '\0';
}
