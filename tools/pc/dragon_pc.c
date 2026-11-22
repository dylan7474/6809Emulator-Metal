/*
 * dragon_pc.c - run the bare-metal core on a PC, headless, for tests
 *
 *   dragon_pc [-f frames] [-t text] [-p picture.ppm] [-s scale] [-w sound.wav] rom
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
#include <string.h>

static uint8_t picture[VDG_HEIGHT][VDG_WIDTH];

int main(int argc, char **argv)
{
	int frames = 200, scale = 2, opt;
	const char *text = NULL, *ppm = NULL, *wav = NULL;
	while ((opt = getopt(argc, argv, "f:t:p:s:w:")) != -1)
	{
		switch (opt)
		{
		case 'f': frames = atoi(optarg); break;
		case 't': text = optarg; break;
		case 'p': ppm = optarg; break;
		case 's': scale = atoi(optarg); break;
		case 'w': wav = optarg; break;
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
	FILE *wf = NULL;
	if (wav)
	{
		wf = fopen(wav, "wb");
		if (!wf)
		{
			perror(wav);
			return 1;
		}
		static const uint8_t hdr[44] = { 0 };
		fwrite(hdr, 1, 44, wf);		// header filled in at the end
		dragon_audio_rate(48000);
	}
	char screen[16 * 33 + 1];
	int ok_frame = -1;
	for (int i = 0; i < frames; i++)
	{
		dragon_run_frame();
		if (wf)
		{
			int16_t buf[2048];
			unsigned n = dragon_audio_take(buf, 2048);
			fwrite(buf, 2, n, wf);
		}
		if (ok_frame < 0)
		{
			vdg_text(dragon_memory(), screen);
			if (strstr(screen, "OK\n") != NULL || strstr(screen, "OK ") != NULL)
			{
				ok_frame = i;		// BASIC is up
			}
		}
		if (ok_frame >= 0 && i == ok_frame + 10 && text)
		{
			dragon_type(text);	// type 0.2 s later, once it waits for keys
		}
	}

	vdg_text(dragon_memory(), screen);
	printf("+--------------------------------+\n");
	for (char *line = screen; *line; line += 33)
	{
		printf("|%.32s|\n", line);
	}
	printf("+--------------------------------+\n");
	printf("BASIC ready at frame %d; %d frames, %llu cycles, %u IRQs\n", ok_frame, frames,
	       (unsigned long long)dragon_cycles(), dragon_irqs());
	if (dragon_typing())
	{
		printf("WARNING: still typing; give it more frames (-f)\n");
	}

	if (wf)
	{
		uint32_t data = (uint32_t)ftell(wf) - 44, rate = 48000, byterate = 96000;
		uint8_t h[44];
		memcpy(h, "RIFF", 4); uint32_t riff = data + 36; memcpy(h + 4, &riff, 4);
		memcpy(h + 8, "WAVEfmt ", 8); uint32_t fmtlen = 16; memcpy(h + 16, &fmtlen, 4);
		uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
		memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4);
		memcpy(h + 28, &byterate, 4); memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
		memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
		fseek(wf, 0, SEEK_SET);
		fwrite(h, 1, 44, wf);
		fclose(wf);
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
