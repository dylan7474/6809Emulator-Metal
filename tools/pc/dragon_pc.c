/*
 * dragon_pc.c - run the bare-metal core on a PC, headless, for tests
 *
 *   dragon_pc [-f frames] [-t text] [-p picture.ppm] [-s scale] [-w sound.wav]
 *             [-i tape.cas|tape.wav] [-o record-prefix] rom
 *
 * -a text is typed once the tape motor has been off for a second after
 * running (after a CLOAD), e.g. -t 'CLOAD\r' -a 'RUN\r'.
 * -i inserts a tape (played in real time when the Dragon starts its
 * motor); -o records anything the Dragon saves to <prefix>.wav + .cas.
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

// ---- tape files ----
static FILE *tape_in;
static unsigned tape_read(void *ctx, uint32_t off, uint8_t *buf, unsigned len)
{
	(void)ctx;
	if (fseek(tape_in, off, SEEK_SET) != 0)
	{
		return 0;
	}
	return (unsigned)fread(buf, 1, len, tape_in);
}

static const char *rec_prefix;
static FILE *rec_wav, *rec_cas;

static void wav_header(FILE *f, uint32_t data, uint32_t rate, uint16_t bits)
{
	uint8_t h[44];
	uint32_t riff = data + 36, fmtlen = 16, byterate = rate * bits / 8;
	uint16_t pcm = 1, ch = 1, align = bits / 8;
	memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4);
	memcpy(h + 8, "WAVEfmt ", 8); memcpy(h + 16, &fmtlen, 4);
	memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4);
	memcpy(h + 28, &byterate, 4); memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
	memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
	fseek(f, 0, SEEK_SET);
	fwrite(h, 1, 44, f);
}

static void rec_begin(void *ctx)
{
	(void)ctx;
	char name[512];
	snprintf(name, sizeof(name), "%s.wav", rec_prefix);
	rec_wav = fopen(name, "wb");
	snprintf(name, sizeof(name), "%s.cas", rec_prefix);
	rec_cas = fopen(name, "wb");
	if (rec_wav)
	{
		wav_header(rec_wav, 0, DRAGON_TAPE_WAV_RATE, 8);
	}
}

static void rec_wavdata(void *ctx, const uint8_t *s, unsigned n)
{
	(void)ctx;
	if (rec_wav) fwrite(s, 1, n, rec_wav);
}

static void rec_casdata(void *ctx, const uint8_t *b, unsigned n)
{
	(void)ctx;
	if (rec_cas) fwrite(b, 1, n, rec_cas);
}

static void rec_end(void *ctx, const char *name)
{
	(void)ctx;
	if (rec_wav)
	{
		long len = ftell(rec_wav) - 44;
		wav_header(rec_wav, (uint32_t)len, DRAGON_TAPE_WAV_RATE, 8);
		fclose(rec_wav);
		rec_wav = NULL;
	}
	if (rec_cas)
	{
		printf("Recorded \"%s\": %ld bytes of CAS\n", name, ftell(rec_cas));
		fclose(rec_cas);
		rec_cas = NULL;
	}
}

static const DragonTapeRecorder recorder = { NULL, rec_begin, rec_wavdata, rec_casdata, rec_end };

int main(int argc, char **argv)
{
	int frames = 200, scale = 2, opt;
	const char *text = NULL, *ppm = NULL, *wav = NULL, *tape = NULL, *after = NULL, *ramdump = NULL;
	bool verbose = false;
	while ((opt = getopt(argc, argv, "f:t:p:s:w:i:o:a:vr:")) != -1)
	{
		switch (opt)
		{
		case 'f': frames = atoi(optarg); break;
		case 't': text = optarg; break;
		case 'p': ppm = optarg; break;
		case 's': scale = atoi(optarg); break;
		case 'w': wav = optarg; break;
		case 'i': tape = optarg; break;
		case 'o': rec_prefix = optarg; break;
		case 'a': after = optarg; break;
		case 'v': verbose = true; break;		// screen + counter at each motor stop
		case 'r': ramdump = optarg; break;		// 64 KB memory at each motor stop
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
	if (tape)
	{
		tape_in = fopen(tape, "rb");
		if (!tape_in)
		{
			perror(tape);
			return 1;
		}
		fseek(tape_in, 0, SEEK_END);
		uint32_t size = (uint32_t)ftell(tape_in);
		const char *dot = strrchr(tape, '.');
		int kind = (dot && (!strcmp(dot, ".wav") || !strcmp(dot, ".WAV"))) ? DRAGON_TAPE_WAV : DRAGON_TAPE_CAS;
		if (!dragon_tape_insert(kind, size, tape_read, NULL))
		{
			fprintf(stderr, "%s: not a usable tape\n", tape);
			return 1;
		}
		DragonTapeProgram progs[16];
		unsigned n = dragon_tape_programs(progs, 16);
		printf("Tape %s: %u.%01u s", tape, dragon_tape_len_ms() / 1000, dragon_tape_len_ms() % 1000 / 100);
		for (unsigned i = 0; i < n; i++)
		{
			printf("%s\"%s\" (type %u) at %u.%01u s", i ? ", " : "; programs: ", progs[i].name, progs[i].type,
			       progs[i].ms / 1000, progs[i].ms % 1000 / 100);
		}
		printf("\n");
	}
	if (rec_prefix)
	{
		dragon_tape_recorder(&recorder);
	}
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
		static bool motor_ran, motor_was;
		static int motor_off_frames;
		bool motor = dragon_tape_motor();
		if (motor_was && !motor)
		{
			if (verbose)
			{
				vdg_text(dragon_memory(), screen);
				DragonRegs r;
				dragon_regs(&r);
				printf("motor off at frame %d, tape %u.%03u s, PC=%04X: [%.32s] [%.32s]\n", i,
				       dragon_tape_pos_ms() / 1000, dragon_tape_pos_ms() % 1000, r.pc, screen, screen + 33);
			}
			if (ramdump)
			{
				FILE *d = fopen(ramdump, "wb");
				if (d)
				{
					fwrite(dragon_memory(), 1, 65536, d);
					fclose(d);
				}
			}
		}
		motor_was = motor;
		if (motor)
		{
			motor_ran = true;
			motor_off_frames = 0;
		}
		else if (motor_ran && after && ++motor_off_frames == 50)
		{
			dragon_type(after);	// the load has finished
			after = NULL;
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
	if (tape)
	{
		printf("Tape counter %u.%01u s of %u.%01u s\n", dragon_tape_pos_ms() / 1000,
		       dragon_tape_pos_ms() % 1000 / 100, dragon_tape_len_ms() / 1000, dragon_tape_len_ms() % 1000 / 100);
	}
	if (rec_prefix)
	{
		dragon_tape_record_finish();
	}
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
