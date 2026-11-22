/*
 * tape.c - the Dragon's cassette deck: real-time .CAS/.WAV input and
 * .WAV + .CAS recording
 *
 * Copyright (C) 2026 Dylan Jones
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Dragon tape format: one cycle of tone per bit, "0" long and "1" short,
 * bytes least significant bit first.  A real Dragon's CSAVE writes each
 * bit as one sine cycle through the 6-bit DAC, starting at mid level
 * (32) and rising; measured in this core (and the same as XRoar uses):
 * 813 CPU cycles for a 0 (~1090 Hz), 435 for a 1 (~2050 Hz).  A block is
 * leader ($55s), sync $3C, type, length, data, checksum.  Type 0 is the
 * name block (8-character name, then file type), $FF ends a file.
 *
 * Input: .CAS bits are played as square waves of those lengths (high
 * half, then low).  The ROM waits ~0.5 s after starting the motor, so on
 * a real tape there is extra leader there; many .CAS files have short
 * leaders, so after each motor start leader is added until there is
 * enough.  .WAV is read at its own rate: DC removed, then a Schmitt
 * trigger at 15% of the recent peak (copes with quiet, noisy recordings
 * of real tapes).
 *
 * Output: while the motor runs, the DAC level is sampled into a 22050 Hz
 * 8-bit .WAV and its rising mid-level crossings (= bit starts) are timed
 * into bits for the .CAS.
 */
#include "dragon.h"
#include "tape.h"
#include <string.h>

#define CPU_HZ		((uint64_t)DRAGON_FRAME_CYCLES * 50)	// 889200
#define BIT0_CYCLES	813
#define BIT1_CYCLES	435
#define BIT_SPLIT	624		// decoding: shorter than this = 1
#define LEADER_WANTED	140		// leader bytes wanted after a motor start
#define BYTE_CYCLES_55	(4 * BIT0_CYCLES + 4 * BIT1_CYCLES)	// one $55 byte

// ---- Input tape -------------------------------------------------------------

static struct
{
	int kind;			// 0 = none
	DragonTapeRead read;
	void *ctx;
	uint32_t size;
	bool play;			// PLAY down
	bool motor;			// relay on
	bool level;			// what the comparator sees
	uint64_t pos;			// tape position, CPU cycles (the counter)
	uint64_t len;			// tape length, CPU cycles

	uint8_t buf[1024];		// read cache
	uint32_t buf_off;
	unsigned buf_len;

	// .CAS
	uint32_t byte_off;		// next byte
	int bit_idx;			// next bit in it
	int bit_len, bit_left;		// the bit playing now
	unsigned pad_bits;		// extra leader still to play
	unsigned pad_pos;
	bool cur_pad;			// the bit playing now is padding
	unsigned tail_bits;		// leader bits played after the end of the file
	bool ended;

	// .WAV
	unsigned channels, bytes_per_sample, rate;
	uint32_t data_off, frames;
	uint32_t next_frame;		// next sample to look at
	float dc, peak;
} in;

static int src_byte(uint32_t off)
{
	if (off >= in.size)
	{
		return -1;
	}
	if (off < in.buf_off || off >= in.buf_off + in.buf_len)
	{
		in.buf_off = off;
		in.buf_len = in.read(in.ctx, off, in.buf, sizeof(in.buf));
		if (in.buf_len == 0)
		{
			return -1;
		}
	}
	return in.buf[off - in.buf_off];
}

static uint32_t src_le(uint32_t off, int n)
{
	uint32_t v = 0;
	for (int i = n - 1; i >= 0; i--)
	{
		int b = src_byte(off + (uint32_t)i);
		v = (v << 8) | (uint32_t)(b < 0 ? 0 : b);
	}
	return v;
}

static uint32_t cycles_to_ms(uint64_t c)
{
	return (uint32_t)(c * 1000 / CPU_HZ);
}

// ---- .CAS input ----

static uint64_t cas_byte_cycles(int byte)
{
	int ones = __builtin_popcount((unsigned)byte);
	return (uint64_t)ones * BIT1_CYCLES + (uint64_t)(8 - ones) * BIT0_CYCLES;
}

static int cas_next_bit(void)
{
	if (in.pad_bits > 0)
	{
		in.pad_bits--;
		return (in.pad_pos++ & 1) ? 0 : 1;	// $55, LSB first: 1 0 1 0 ...
	}
	int byte = src_byte(in.byte_off);
	if (byte < 0)
	{
		// End of file: a few extra leader bits so the ROM sees the edge that
		// ends the last data bit (some .CAS files stop right after it)
		if (in.tail_bits < 8)
		{
			return (in.tail_bits++ & 1) ? 0 : 1;
		}
		return -1;
	}
	int bit = (byte >> in.bit_idx) & 1;
	if (++in.bit_idx == 8)
	{
		in.bit_idx = 0;
		in.byte_off++;
	}
	return bit;
}

static void cas_advance(int cycles)
{
	while (cycles > 0 && !in.ended)
	{
		if (in.bit_left == 0)
		{
			in.cur_pad = in.pad_bits > 0;
			int bit = cas_next_bit();
			if (bit < 0)
			{
				in.ended = true;
				in.level = false;
				return;
			}
			in.bit_len = in.bit_left = bit ? BIT1_CYCLES : BIT0_CYCLES;
		}
		int take = cycles < in.bit_left ? cycles : in.bit_left;
		in.bit_left -= take;
		cycles -= take;
		if (!in.cur_pad)
		{
			in.pos += (uint64_t)take;	// padding doesn't move the counter
		}
		in.level = in.bit_left * 2 > in.bit_len;	// first half high
	}
}

// Motor started: if a short leader follows, add some (see top of file)
static void cas_motor_started(void)
{
	unsigned count = 0;
	while (count < LEADER_WANTED && src_byte(in.byte_off + (in.bit_idx ? 1 : 0) + count) == 0x55)
	{
		count++;
	}
	if (count > 0 && count < LEADER_WANTED)
	{
		in.pad_bits = (LEADER_WANTED - count) * 8;
		in.pad_pos = 0;
	}
}

static void cas_seek(uint64_t target)
{
	uint64_t t = 0;
	uint32_t off = 0;
	int b;
	while ((b = src_byte(off)) >= 0)
	{
		uint64_t c = cas_byte_cycles(b);
		if (t + c > target)
		{
			break;
		}
		t += c;
		off++;
	}
	in.byte_off = off;
	in.bit_idx = 0;
	in.bit_left = 0;
	in.pad_bits = 0;
	in.tail_bits = 0;
	in.pos = t;
	in.ended = false;
	in.level = false;
}

// ---- .WAV input ----

static bool wav_open(void)
{
	if (src_le(0, 4) != 0x46464952 || src_le(8, 4) != 0x45564157)	// "RIFF" "WAVE"
	{
		return false;
	}
	uint32_t off = 12;
	bool fmt = false;
	while (off + 8 <= in.size)
	{
		uint32_t id = src_le(off, 4), len = src_le(off + 4, 4);
		if (id == 0x20746D66)		// "fmt "
		{
			if (src_le(off + 8, 2) != 1)
			{
				return false;	// not PCM
			}
			in.channels = src_le(off + 10, 2);
			in.rate = src_le(off + 12, 4);
			unsigned bits = src_le(off + 22, 2);
			in.bytes_per_sample = bits / 8;
			fmt = in.channels >= 1 && in.channels <= 2 && in.rate >= 4000
			      && (bits == 8 || bits == 16);
		}
		else if (id == 0x61746164 && fmt)	// "data"
		{
			in.data_off = off + 8;
			uint32_t avail = in.size - in.data_off;
			if (len > avail)
			{
				len = avail;
			}
			in.frames = len / (in.channels * in.bytes_per_sample);
			return in.frames > 0;
		}
		off += 8 + len + (len & 1);
	}
	return false;
}

// One sample, channels mixed, -1..1
static float wav_sample(uint32_t frame)
{
	uint32_t off = in.data_off + frame * in.channels * in.bytes_per_sample;
	float sum = 0;
	for (unsigned c = 0; c < in.channels; c++)
	{
		if (in.bytes_per_sample == 1)
		{
			int b = src_byte(off);
			sum += (float)((b < 0 ? 128 : b) - 128) / 128.0f;
			off += 1;
		}
		else
		{
			int16_t v = (int16_t)src_le(off, 2);
			sum += (float)v / 32768.0f;
			off += 2;
		}
	}
	return sum / (float)in.channels;
}

static void wav_advance(int cycles)
{
	in.pos += (uint64_t)cycles;
	uint64_t want = in.pos * in.rate / CPU_HZ;
	if (want > in.frames)
	{
		want = in.frames;
		in.ended = true;
	}
	float dc_k = 20.0f / (float)in.rate;		// ~ 3 Hz DC tracking
	float peak_k = 1.0f - 10.0f / (float)in.rate;	// peak falls ~ 10 /s
	while (in.next_frame < want)
	{
		float x = wav_sample(in.next_frame++);
		in.dc += (x - in.dc) * dc_k;
		float y = x - in.dc;
		float a = y < 0 ? -y : y;
		in.peak = a > in.peak ? a : in.peak * peak_k;
		float th = in.peak * 0.15f;
		if (th < 0.004f)
		{
			th = 0.004f;		// silence: don't chase noise
		}
		if (y > th)
		{
			in.level = true;
		}
		else if (y < -th)
		{
			in.level = false;
		}
	}
}

static void wav_seek(uint64_t target)
{
	in.pos = target;
	in.next_frame = (uint32_t)(target * in.rate / CPU_HZ);
	in.dc = 0;
	in.peak = 0;
	in.ended = in.next_frame >= in.frames;
}

// ---- Output (recording) --------------------------------------------------

#define PENDING_MAX	4096		// samples held before we know it's a real save

static const DragonTapeRecorder *rec;

static struct
{
	bool active;			// recording a tape now
	bool pending;			// DAC moving with the motor on: maybe a save
	bool eof;			// end-of-file block written
	uint64_t t;			// time, CPU cycles
	unsigned dac, last_dac;
	uint64_t last_change;
	uint64_t wav_next;		// next sample time x DRAGON_TAPE_WAV_RATE
	uint8_t wbuf[PENDING_MAX];
	unsigned wn;
	uint8_t cbuf[256];
	unsigned cn;
	unsigned bytes;			// .CAS bytes decoded this recording

	// bit decoder
	bool have_start;
	uint64_t start;
	uint8_t byte;
	int nbits;

	// block parser (for the name and the end)
	int st, left, name_n;
	uint8_t type;
	char name[9];
	bool got_name;
} out;

static void out_flush(void)
{
	if (!out.active || rec == NULL)
	{
		return;
	}
	if (out.wn)
	{
		rec->wav(rec->ctx, out.wbuf, out.wn);
		out.wn = 0;
	}
	if (out.cn)
	{
		rec->cas(rec->ctx, out.cbuf, out.cn);
		out.cn = 0;
	}
}

static void out_sample(uint8_t s)
{
	out.wbuf[out.wn++] = s;
	if (out.wn == sizeof(out.wbuf))
	{
		if (out.active)
		{
			out_flush();
		}
		else
		{
			out.pending = false;	// moved a lot, never decoded bytes: not a save
			out.wn = 0;
			out.cn = 0;
		}
	}
}

static void parse_byte(uint8_t b)
{
	switch (out.st)
	{
	case 0:				// leader, waiting for sync
		if (b == 0x3C)
		{
			out.st = 1;
		}
		break;
	case 1:
		out.type = b;
		out.st = 2;
		break;
	case 2:
		out.left = b + 1;	// data + checksum
		out.name_n = 0;
		if (out.type == 0xFF)
		{
			out.eof = true;
		}
		out.st = 3;
		break;
	default:
		if (out.type == 0x00 && !out.got_name && out.name_n < 8)
		{
			out.name[out.name_n++] = (char)b;
			if (out.name_n == 8)
			{
				out.got_name = true;
			}
		}
		if (--out.left == 0)
		{
			out.st = 0;
		}
		break;
	}
}

static void begin_recording(void)
{
	out.active = true;
	out.pending = false;
	rec->begin(rec->ctx);
	// 0.3 s of quiet before the tone, then what was held while pending
	uint8_t q[256];
	memset(q, 128, sizeof(q));
	for (unsigned n = DRAGON_TAPE_WAV_RATE * 3 / 10; n > 0;)
	{
		unsigned k = n < sizeof(q) ? n : sizeof(q);
		rec->wav(rec->ctx, q, k);
		n -= k;
	}
	out_flush();
}

static void push_bit(int bit)
{
	out.byte |= (uint8_t)(bit << out.nbits);
	if (++out.nbits < 8)
	{
		return;
	}
	parse_byte(out.byte);
	out.cbuf[out.cn++] = out.byte;
	out.bytes++;
	out.byte = 0;
	out.nbits = 0;
	if (!out.active && out.bytes >= 2 && rec != NULL)
	{
		begin_recording();
	}
	if (out.cn == sizeof(out.cbuf))
	{
		out_flush();
	}
}

static void reset_out(void)
{
	memset(&out, 0, sizeof(out));
	out.dac = out.last_dac = 32;
}

static void finish_recording(void)
{
	if (out.active && rec != NULL)
	{
		out_flush();
		char name[9];
		int n = out.got_name ? 8 : 0;
		memcpy(name, out.name, 8);
		while (n > 0 && (name[n - 1] == ' ' || name[n - 1] == 0))
		{
			n--;
		}
		name[n] = '\0';
		rec->end(rec->ctx, name);
	}
	uint64_t t = out.t;
	unsigned dac = out.dac;
	reset_out();
	out.t = t;
	out.dac = out.last_dac = dac;
}

// ---- Hooks from the CPU side ------------------------------------------------

void tape_reset(void)
{
	in.motor = false;
	finish_recording();
}

void tape_set_motor(bool on)
{
	if (on == in.motor)
	{
		return;
	}
	in.motor = on;
	if (on)
	{
		if (in.kind == DRAGON_TAPE_CAS && in.play)
		{
			cas_motor_started();
		}
		out.wav_next = out.t * DRAGON_TAPE_WAV_RATE;	// tape starts moving now
	}
	else
	{
		if (out.have_start)
		{
			// the last bit before the motor stopped (the trailing $55)
			uint64_t period = out.last_change - out.start + 30;
			push_bit(period < BIT_SPLIT);
			out.have_start = false;
		}
		out_flush();
		if (out.active && out.eof)
		{
			finish_recording();
		}
		else if (!out.active)
		{
			out.pending = false;
			out.wn = out.cn = 0;
			out.bytes = 0;
			out.nbits = 0;
			out.byte = 0;
			out.have_start = false;
		}
	}
}

bool tape_input_level(void)
{
	return in.kind && in.play && in.level;
}

void tape_run(int cycles, unsigned dac)
{
	if (in.kind && in.play && in.motor && !in.ended)
	{
		if (in.kind == DRAGON_TAPE_CAS)
		{
			cas_advance(cycles);
		}
		else
		{
			wav_advance(cycles);
		}
	}

	out.t += (uint64_t)cycles;
	if (rec == NULL || !in.motor)
	{
		out.dac = out.last_dac = dac;
		return;
	}

	// .WAV: the level held over this time
	if (out.active || out.pending)
	{
		uint64_t now = out.t * DRAGON_TAPE_WAV_RATE;
		while (out.wav_next <= now)
		{
			out_sample((uint8_t)(out.last_dac * 4 + 2));
			out.wav_next += CPU_HZ;
		}
	}

	// A quiet spell ends the bit being timed (last bit of a block)
	if (out.have_start && out.t - out.last_change > 2000)
	{
		uint64_t period = out.last_change - out.start + 30;
		push_bit(period < BIT_SPLIT);
		out.have_start = false;
	}

	if (dac != out.last_dac)
	{
		if (!out.active && !out.pending)
		{
			out.pending = true;
			out.wav_next = out.t * DRAGON_TAPE_WAV_RATE;
		}
		bool rising = out.last_dac < 32 && dac >= 32;
		if (rising || (!out.have_start && dac >= 32 && out.last_dac >= 32))
		{
			if (out.have_start)
			{
				push_bit(out.t - out.start < BIT_SPLIT);
			}
			out.start = out.t;
			out.have_start = true;
		}
		out.last_dac = dac;
		out.last_change = out.t;
	}
	out.dac = dac;
}

// ---- Platform API ----------------------------------------------------------

bool dragon_tape_insert(int kind, uint32_t size, DragonTapeRead read, void *ctx)
{
	dragon_tape_eject();
	bool motor = in.motor;
	memset(&in, 0, sizeof(in));
	in.motor = motor;
	in.kind = kind;
	in.size = size;
	in.read = read;
	in.ctx = ctx;
	if (kind == DRAGON_TAPE_WAV)
	{
		if (!wav_open())
		{
			in.kind = 0;
			return false;
		}
		in.len = (uint64_t)in.frames * CPU_HZ / in.rate;
	}
	else if (kind == DRAGON_TAPE_CAS)
	{
		int b;
		for (uint32_t off = 0; (b = src_byte(off)) >= 0; off++)
		{
			in.len += cas_byte_cycles(b);
		}
	}
	else
	{
		in.kind = 0;
		return false;
	}
	in.play = true;
	return true;
}

void dragon_tape_eject(void)
{
	bool motor = in.motor;
	memset(&in, 0, sizeof(in));
	in.motor = motor;
}

bool dragon_tape_inserted(void)
{
	return in.kind != 0;
}

void dragon_tape_play(bool play)
{
	in.play = play && in.kind != 0;
}

bool dragon_tape_playing(void)
{
	return in.play;
}

bool dragon_tape_motor(void)
{
	return in.motor;
}

void dragon_tape_seek_ms(uint32_t ms)
{
	uint64_t target = (uint64_t)ms * CPU_HZ / 1000;
	if (target > in.len)
	{
		target = in.len;
	}
	if (in.kind == DRAGON_TAPE_CAS)
	{
		cas_seek(target);
	}
	else if (in.kind == DRAGON_TAPE_WAV)
	{
		wav_seek(target);
	}
}

uint32_t dragon_tape_pos_ms(void)
{
	return cycles_to_ms(in.pos);
}

uint32_t dragon_tape_len_ms(void)
{
	return cycles_to_ms(in.len);
}

// Name blocks: leader, $3C, type 0, length >= 9, 8-char name, file type
unsigned dragon_tape_programs(DragonTapeProgram *list, unsigned max)
{
	if (in.kind != DRAGON_TAPE_CAS)
	{
		return 0;
	}
	unsigned n = 0;
	uint64_t t = 0;
	uint64_t leader_t = 0;
	bool in_leader = false;
	int b;
	for (uint32_t off = 0; (b = src_byte(off)) >= 0 && n < max; off++)
	{
		if (b == 0x55)
		{
			if (!in_leader)
			{
				in_leader = true;
				leader_t = t;
			}
		}
		else
		{
			if (b == 0x3C && in_leader && src_byte(off + 1) == 0x00 && src_byte(off + 2) >= 9)
			{
				DragonTapeProgram *p = &list[n++];
				int k = 0;
				for (; k < 8; k++)
				{
					int c = src_byte(off + 3 + (uint32_t)k);
					p->name[k] = (char)(c >= 32 && c < 127 ? c : ' ');
				}
				while (k > 0 && p->name[k - 1] == ' ')
				{
					k--;
				}
				p->name[k] = '\0';
				p->type = (uint8_t)src_byte(off + 11);
				p->ms = cycles_to_ms(leader_t);
			}
			in_leader = false;
		}
		t += cas_byte_cycles(b);
	}
	return n;
}

void dragon_tape_recorder(const DragonTapeRecorder *r)
{
	finish_recording();
	rec = r;
}

void dragon_tape_record_finish(void)
{
	finish_recording();
}

bool dragon_tape_recording(void)
{
	return out.active;
}
