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
 * orange text.  Graphics modes (PMODE) and the SAM's display offset are
 * not emulated yet: the picture is always the text screen at $0400.
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

void vdg_render(const uint8_t *memory, uint8_t picture[VDG_HEIGHT][VDG_WIDTH])
{
	if (!font_ready)
	{
		build_font();
	}
	const uint8_t *vram = memory + 0x0400;
	bool css = (memory[0xFF22] & 0x08) != 0;
	uint8_t text_light = css ? VDG_ORANGE : VDG_GREEN;
	uint8_t text_dark = css ? VDG_DARK_ORANGE : VDG_DARK_GREEN;

	for (int cy = 0; cy < 16; cy++)
	{
		for (int cx = 0; cx < 32; cx++)
		{
			uint8_t v = vram[cy * 32 + cx];
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

void vdg_text(const uint8_t *memory, char *buf)
{
	// Character codes 0-63 in the 6847's order, as ASCII
	static const char ascii[] =
		"@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_ !\"#$%&'()*+,-./0123456789:;<=>?";
	const uint8_t *vram = memory + 0x0400;
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
