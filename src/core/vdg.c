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
 * GM2-GM0, see gmode[] below).  The picture starts where the SAM's F0-F6
 * bits say (x 512 bytes).  Not yet: semigraphics-6/8/12/24.
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

// The eight graphics modes, by GM2-GM0 (PIA1 PB6-PB4) with A/G set:
// bytes per row, bits per pixel, dots per pixel across, lines per row
static const struct { uint8_t bytes, bpp, dot_w, row_h; } gmode[8] =
{
	{ 16, 2, 4, 3 },	// 0 CG1  64 x 64,  4 colours
	{ 16, 1, 2, 3 },	// 1 RG1 128 x 64,  2 colours
	{ 32, 2, 2, 3 },	// 2 CG2 128 x 64,  4 colours
	{ 16, 1, 2, 2 },	// 3 RG2 128 x 96,  2 colours  (PMODE 0)
	{ 32, 2, 2, 2 },	// 4 CG3 128 x 96,  4 colours  (PMODE 1)
	{ 16, 1, 2, 1 },	// 5 RG3 128 x 192, 2 colours  (PMODE 2)
	{ 32, 2, 2, 1 },	// 6 CG6 128 x 192, 4 colours  (PMODE 3)
	{ 32, 1, 1, 1 },	// 7 RG6 256 x 192, 2 colours  (PMODE 4)
};

static void render_graphics(const uint8_t *memory, unsigned mode, bool css,
			    uint8_t picture[VDG_HEIGHT][VDG_WIDTH])
{
	unsigned base = video_base();
	unsigned bytes = gmode[mode].bytes, bpp = gmode[mode].bpp;
	unsigned dot_w = gmode[mode].dot_w, row_h = gmode[mode].row_h;
	// 4 colours: green yellow blue red, or (CSS) buff cyan magenta orange.
	// 2 colours: black + green, or (CSS) black + buff.
	uint8_t first = css ? VDG_BUFF : VDG_GREEN;

	for (unsigned y = 0; y < VDG_HEIGHT; y++)
	{
		const uint8_t *row = memory + ((base + (y / row_h) * bytes) & 0xFFFF);
		uint8_t *out = picture[y];
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
					uint8_t c = (v >> sh) & 1 ? first : VDG_BLACK;
					for (unsigned d = 0; d < dot_w; d++)
					{
						out[x++] = c;
					}
				}
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
	bool css = (vdg_mode & 0x08) != 0;
	if (vdg_mode & 0x80)
	{
		render_graphics(memory, (vdg_mode >> 4) & 7, css, picture);
		return;
	}

	const uint8_t *vram = memory + video_base();
	uint8_t text_light = css ? VDG_ORANGE : VDG_GREEN;
	uint8_t text_dark = css ? VDG_DARK_ORANGE : VDG_DARK_GREEN;

	for (int cy = 0; cy < 16; cy++)
	{
		for (int cx = 0; cx < 32; cx++)
		{
			uint8_t v = vram[(cy * 32 + cx) & 0xFFFF];
			for (int y = 0; y < 12; y++)
			{
				uint8_t *out = &picture[cy * 12 + y][cx * 8];
				if (v & 0x80)
				{
					// Semigraphics-4: 2 x 2 blocks of 4 x 6 pixels
					uint8_t colour = (v >> 4) & 0x07;
					int top = y < 6;
					uint8_t left_on = top ? (v & 0x08) : (v & 0x02);
					uint8_t right_on = top ? (v & 0x04) : (v & 0x01);
					uint8_t l = left_on ? colour : VDG_BLACK;
					uint8_t r = right_on ? colour : VDG_BLACK;
					out[0] = out[1] = out[2] = out[3] = l;
					out[4] = out[5] = out[6] = out[7] = r;
				}
				else
				{
					bool inverse = (v & 0x40) == 0;
					uint8_t ink = inverse ? text_light : text_dark;
					uint8_t paper = inverse ? text_dark : text_light;
					uint8_t bits = font[v & 0x3F][y];
					for (int x = 0; x < 8; x++)
					{
						out[x] = (bits & (0x80 >> x)) ? ink : paper;
					}
				}
			}
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
