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

These are the same gaps as the ncurses version: graphics modes (`PMODE`
and the SAM display offset), the 50 Hz interrupt (`TIMER`), sound,
cassette, joysticks and cycle-exact timing. The core runs 17,800
instructions per 50 Hz frame, the same as the ncurses version.

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

### cmdline.txt options

- `scale=N` uses a smaller whole-number scale than the largest that fits.
- `keylog=1` writes every USB key event (with its frame number) to the log.

### Logs on the card

- `bootlog-<board>.txt` holds every start-up step, USB changes and a speed
  line every 10 s. The previous boot's log is kept as
  `bootlog-<board>-prev.txt`.
- `speed-<board>.txt` is written once, 60 s after start. It gives the CPU
  time per 20 ms frame and the headroom.
- If it crashes, the end of the log is shown on screen and saved.

`<board>` comes from the Pi's model name, for example `zero-2-w` or
`zero-w`, so several boards can share one card.

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
```

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
