/*
 * tape.h - the Dragon's cassette deck (part of the core; see dragon.h for
 * the platform API).  These are the hooks dragon_cpu.c calls.
 *
 * Copyright (C) 2026 Dylan Jones
 * GNU GPL version 2 or later (see dragon.h).
 */
#ifndef TAPE_H
#define TAPE_H

#include <stdint.h>
#include <stdbool.h>

void tape_reset(void);
void tape_set_motor(bool on);		// PIA1 CA2 changed
bool tape_input_level(void);		// PIA1 PA0: the cassette input comparator
void tape_run(int cycles, unsigned dac);	// time passed; DAC (0-63) during it

#endif
