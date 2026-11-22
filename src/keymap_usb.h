/*
 * keymap_usb.h - USB keyboard (UK layout) -> Dragon 32 keyboard matrix
 *
 * Copyright (C) 2026 Dylan Jones
 * GNU GPL version 2 or later (see dragon.h).
 */
#ifndef KEYMAP_USB_H
#define KEYMAP_USB_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// USB HID usages the platform handles itself (not Dragon keys)
#define USB_KEY_F1	0x3A	// reset
#define USB_KEY_F2	0x3B	// turbo on/off
#define USB_KEY_LSHIFT	0xE1
#define USB_KEY_RSHIFT	0xE5

// Build the Dragon's matrix from every key held down.  held[usage] is
// true for each USB usage currently down (modifiers are 0xE0-0xE7).
// Symbols follow what the PC key types (Shift+2 = " like a Dragon,
// Shift+; = : which is unshifted on a Dragon); other keys keep SHIFT
// as pressed.
void keymap_matrix(const bool held[256], uint8_t matrix[8]);

// Typing feeder: the Dragon's ROM only sees a new key when no other key
// is down, but PC typists press the next key before letting go of the
// last (rollover), so keys were lost.  The feeder presents one key at a
// time: held at least KEYFEED_HOLD frames, then KEYFEED_GAP frames with
// nothing down before the next.  Keys pressed meanwhile wait in order; a
// key still held stays down (so held arrows work).  Call keyfeed_event()
// for every USB key change (modifiers too) and keyfeed_frame() once per
// 50 Hz frame before running it.
#define KEYFEED_HOLD	2
#define KEYFEED_GAP	2
void keyfeed_reset(void);
void keyfeed_event(uint8_t usage, bool down, unsigned frame);
void keyfeed_frame(unsigned frame, uint8_t matrix[8]);
unsigned keyfeed_waiting(void);		// keys queued (for the status line)

// One key: false if it is not a Dragon key.
bool keymap_usb(uint8_t usage, bool pc_shift, int *row, int *col, bool *shift);

#ifdef __cplusplus
}
#endif

#endif
