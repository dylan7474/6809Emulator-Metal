# 6809Emulator-Metal

A Dragon 32 (Motorola 6809) that boots straight into BASIC on a Raspberry
Pi, with no Linux underneath. The CPU core is the one from
[6809Emulator](https://github.com/dylan7474/6809Emulator). It runs on
[Circle](https://github.com/rsta2/circle) (through
[circle-stdlib](https://github.com/smuehlst/circle-stdlib)) and uses the
same build and card layout as ClassicPi-Metal.

One SD card works in every Pi: the firmware picks the right kernel.

| Pi | Kernel |
|---|---|
| Zero, Zero W, Pi 1 | `kernel.img` (32-bit, ARMv6) |
| Pi 2 | `kernel7.img` (32-bit) |
| Zero 2 W, Pi 3 | `kernel8.img` (64-bit) |
| Pi 4, 400 | `kernel8-rpi4.img` (64-bit, with Circle's armstub) |
| Pi 5, 500 | `kernel_2712.img` (64-bit) |

## What works

- Boots the Dragon 32 BASIC ROM from the SD card to the `OK` prompt.
- HDMI picture: the MC6847 text screen (green, or orange with the colour
  set bit) and the eight-colour semigraphics-4 blocks that BASIC's `CLS n`,
  `SET`/`RESET` and `CHR$(128-255)` use. The picture is scaled by a whole
  number and centred (5x on 1080p, 4x on 1024x768).
- Real speed: every 6809 instruction takes its datasheet cycle count
  (checked against XRoar's cycle-timed trace: all 2.6 million instructions
  of a BASIC run agree), and 17,784 cycles make one 50 Hz PAL frame. A
  `FOR` loop, the cursor blink and `TIMER` run at Dragon speed.
- The 50 Hz frame interrupt (the VDG's field sync on PIA0 CB1), so
  `TIMER`, `SOUND` and `PLAY` timing work. Both MC6821 PIAs are emulated:
  data direction and control registers, Cx1 interrupt flags, IRQ and FIRQ,
  plus `SYNC` and `CWAI`.
- Sound over HDMI (Pi Zero to Pi 4; Circle has no HDMI sound on the Pi 5):
  the 6-bit DAC through the sound mux, plus single-bit sound, at 48 kHz.
  Levels are the voltages measured on a real Dragon (from XRoar).
  `SOUND 89,10` plays middle C for 0.8 s; `PLAY` works.
- USB keyboards, UK layout. Symbols type what the key says (Shift+2 is `"`,
  Shift+; is `:`, Shift+' is `@`). Several keyboards can be used at once,
  and they can be plugged in and out while it runs.

| PC key | Dragon key |
|---|---|
| Enter | ENTER |
| Esc (or Pause) | BREAK |
| Home | CLEAR |
| Backspace / Left | Left arrow (delete) |
| Up, Down, Right | arrows |
| F1 | reset the CPU (your program is kept) |
| F2 | turbo on/off (runs as fast as the Pi can) |

## Not yet

Graphics modes (`PMODE` and the SAM display offset), cassette,
joysticks, the SAM's fast-CPU poke (`POKE 65495,0`) and cartridges.

## SD card

You need your own Dragon 32 ROM (16384 bytes). Put it in the root of the
card as `d32.rom` (`dragon32.rom` and `dragon.rom` also work). ROMs are
never committed: `*.rom` and `local/` are gitignored.

```sh
make sdcard                    # all kernels + Pi firmware -> build/sdcard/
cp ~/my/d32.rom local/
tools/make-sdimage.sh          # -> build/6809Emulator-Metal.img (64 MB)
```

Flash the image with Raspberry Pi Imager ("Use custom") or `dd`. You can
also copy `build/sdcard/` plus `d32.rom` onto any FAT32 card.

### Wi-Fi and the clock

The Dragon has no clock (`TIMER` only counts 50ths of a second since
power-on), but the files the Pi writes to the card (saved tapes, logs) are
dated from the Pi's clock. Put a `wpa_supplicant.conf` on the card (copy
`wpa_supplicant.conf.example` and fill in your network) and the Pi joins
Wi-Fi at power-on and sets its clock from internet time (NTP) a few seconds
later; the time is UK time with summer time unless `tz=` says otherwise.
The network only gets the idle part of each frame, so the Dragon's speed
never changes. Without the file, files are dated 1 January 1980.

`tools/make-sdimage.sh` adds `local/wpa_supplicant.conf` when it exists;
the image then holds your Wi-Fi password, so keep it private.

### cmdline.txt options

- `scale=N` uses a smaller whole-number scale than the largest that fits.
- `keylog=1` writes every USB key event (with its frame number) to the log.
- `sound=off` turns HDMI sound off.
- `wifi=off` never starts Wi-Fi; `ntp=server` picks the time server
  (default `pool.ntp.org`); `tz=` a POSIX time zone (default
  `GMT0BST,M3.5.0/1,M10.5.0`, the UK).
- `keys=raw` puts every key you hold straight onto the Dragon's keyboard
  matrix, for games that read two keys at once. By default keys are fed
  one at a time: the Dragon's ROM only notices a new key when no other is
  down, and PC typists press the next key before letting go of the last.
- `debug=1` adds a status line every 5 s (frames, IRQs, the Dragon's PC,
  SYNC/CWAI, sound queue) and saves the log to the card each time, so a
  freeze shows when it happened. The saves can cause small hiccups.
  It also draws a status line under the picture every second (uptime,
  Circle's 100 Hz interrupt tick count, frames, Dragon IRQs, USB key
  reports and drops, keyboards, SD saves OK/failed, sound queue), which
  keeps working even if the SD card or USB stop.

While it runs, the green ACT LED blinks twice a second. If the picture
freezes but the LED keeps blinking, the Pi is fine and the Dragon is
stuck; if the LED stops too, the Pi itself has hung.

### Logs on the card

- `bootlog-<board>.txt` holds every start-up step, USB changes and a speed
  line every 10 s. The previous boot's log is kept as
  `bootlog-<board>-prev.txt`.
- `speed-<board>.txt` is written once, 60 s after start. It gives the CPU
  time per 20 ms frame and the headroom.
- If it crashes, the end of the log is shown on screen and saved.

`<board>` comes from the Pi's model name, for example `zero-2-w` or
`zero-w`, so several boards can share one card.

On the Pi Zero, 1, 2 and 3 Circle is built with `USE_USB_FIQ`, which
moves the USB controller's 8000 interrupts a second to the FIQ, so HDMI
sound can't delay them (without it a Zero W lost key presses).

## Building

You need the `aarch64-none-elf` and `arm-none-eabi` GNU toolchains (on
Arch, from the AUR: `arm-gnu-toolchain-aarch64-none-elf-bin` and
`arm-gnu-toolchain-arm-none-eabi-bin`), plus `mtools`, `dosfstools`,
`rsync` and `curl`.

```sh
git clone --recursive <this repo>
make              # every kernel (each target builds its own circle-stdlib first, ~2 min)
make pi3          # just one
make test         # PC build: keyboard-map test + BASIC smoke test (local/d32.rom)
```

### Testing on the PC

`make test` builds the same core, video and keyboard code for the PC
(`build/pc/dragon_pc`). It boots the ROM, types BASIC and checks the
screen. `dragon_pc -p out.ppm` also saves the picture the Pi would show.

```sh
build/pc/dragon_pc -f 300 -t 'PRINT 2+3*4\r' -p shot.ppm local/d32.rom
build/pc/dragon_pc -f 400 -t 'SOUND 89,10\r' -w sound.wav local/d32.rom
```

It types the text once BASIC shows `OK`. `-w` saves the sound as a WAV
file and `-p` saves the picture.

### QEMU

```sh
tools/qemu-sd.sh            # build/qemu-sd.img with local/d32.rom
make qemu                   # 64-bit kernel in QEMU's Pi 3; log on the terminal
```

QEMU never runs the Pi firmware or `config.txt`, so it can't catch card
or firmware problems. Only a real Pi can (see ClassicPi-Metal's
`gpu_mem` story).

## Layout

```
src/core/dragon_cpu.c   6809 + Dragon memory map + keyboard matrix (from 6809Emulator)
src/core/vdg.c          MC6847 renderer and font (text + semigraphics-4)
src/core/dragon.h       the core's API: init, run a frame, keys, picture
src/keymap_usb.c        USB HID (UK) -> Dragon matrix
src/kernel.cpp          Circle: SD, HDMI, USB, 50 Hz loop, logs
tools/pc/dragon_pc.c    the core on a PC, for tests
```

## Licence

The core (`src/core/`, `src/keymap_usb.*`, tests) is GPL-2.0-or-later, like
6809Emulator. The Circle kernel files are GPL-3.0-or-later. Because Circle
is GPL-3.0, the kernels are distributed under the GPL-3.0 (see `LICENSE`).
