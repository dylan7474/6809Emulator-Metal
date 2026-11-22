/*
 * dragon_pc.c - run the bare-metal core on a PC, headless, for tests
 *
 *   dragon_pc [-f frames] [-t text] [-p picture.ppm] [-s scale] rom
 *
 * Boots the ROM, types text (escapes as the ncurses version's --inject:
 * \r ENTER, \b BREAK, \c CLEAR) once BASIC is up, runs the given number
 * of 50 Hz frames, prints the text screen and can save the picture the
 * Pi would show.  Same core, renderer and keyboard code as the Pi.
 *
 * Copyright (C) 2026 Dylan Jones
 * GNU GPL version 2 or later (see src/core/dragon.h).
 */
#include "../../src/core/dragon.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static uint8_t picture[VDG_HEIGHT][VDG_WIDTH];

int main(int argc, char **argv)
{
	int frames = 200, scale = 2, opt;
	const char *text = NULL, *ppm = NULL;
	while ((opt = getopt(argc, argv, "f:t:p:s:")) != -1)
	{
		switch (opt)
		{
		case 'f': frames = atoi(optarg); break;
		case 't': text = optarg; break;
		case 'p': ppm = optarg; break;
		case 's': scale = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s [-f frames] [-t text] [-p out.ppm] [-s scale] rom\n", argv[0]);
			return 2;
		}
	}
	if (optind >= argc)
	{
		fprintf(stderr, "no ROM given\n");
		return 2;
	}

	static uint8_t rom[DRAGON_ROM_SIZE];
	FILE *f = fopen(argv[optind], "rb");
	if (!f || fread(rom, 1, sizeof(rom), f) != sizeof(rom))
	{
		fprintf(stderr, "cannot read a 16 KB ROM from %s\n", argv[optind]);
		return 1;
	}
	fclose(f);

	dragon_init(rom);
	for (int i = 0; i < frames; i++)
	{
		if (i == 20 && text)		// BASIC is up after ~16 frames
		{
			dragon_type(text);
		}
		dragon_run_frame();
	}

	char screen[16 * 33 + 1];
	vdg_text(dragon_memory(), screen);
	printf("+--------------------------------+\n");
	for (char *line = screen; *line; line += 33)
	{
		printf("|%.32s|\n", line);
	}
	printf("+--------------------------------+\n");
	if (dragon_typing())
	{
		printf("WARNING: still typing; give it more frames (-f)\n");
	}

	if (ppm)
	{
		vdg_render(dragon_memory(), picture);
		FILE *out = fopen(ppm, "wb");
		if (!out)
		{
			perror(ppm);
			return 1;
		}
		fprintf(out, "P6\n%d %d\n255\n", VDG_WIDTH * scale, VDG_HEIGHT * scale);
		for (int y = 0; y < VDG_HEIGHT * scale; y++)
		{
			for (int x = 0; x < VDG_WIDTH * scale; x++)
			{
				fwrite(vdg_palette[picture[y / scale][x / scale]], 1, 3, out);
			}
		}
		fclose(out);
	}
	return 0;
}
