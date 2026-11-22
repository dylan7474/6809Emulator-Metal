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
  set bit), the eight-colour semigraphics-4 blocks that BASIC's `CLS n`,
  `SET`/`RESET` and `CHR$(128-255)` use, and all eight graphics modes
  (64x64 to 256x192; BASIC's `PMODE 0`-`4`, `PCLS`, `LINE`, `CIRCLE`,
  `PAINT`, `SCREEN`), semigraphics-6 (GM0 set in text mode) and
  semigraphics-8/12/24 (the SAM's row modes with text), and the border:
  black around text, green or buff around graphics. The SAM decides which
  bytes each line shows, so `PMODE`/`PCOPY` pages and games' own screens
  show. The picture is scaled by a whole number and centred (5x on 1080p,
  4x on 1024x768).
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
| F12 | the admin screen (tapes, reset, sound, shut down) |

## The admin screen (F12)

F12 pauses the Dragon and opens a menu (arrow keys, Enter, Esc; the chosen
line has a `>` marker as well as a highlight):

- **Tape deck:** choose a tape from `tapes/` on the SD card, play / stop,
  rewind, back or forward 10 s, jump to a program (on `.cas` tapes),
  eject. The tape counter and length show at the top, and while a tape is
  in, the strip under the Dragon's picture shows its name, counter, PLAY
  or STOP and the motor.
- Reset (the Dragon's reset button: the program in memory is kept), hard
  reset (like switching off and on: memory is cleared, the tape stays
  in; asks first), sound on/off, turbo.
- **Shut down:** finishes any recording, closes every file on the SD card
  and shows "It is now safe to switch off".

## Tapes

Put `.cas` or `.wav` tape files in a `tapes` folder on the SD card (in
folders of your own if you like), choose one with F12, then type `CLOAD`
(or `CLOADM`) as on a real Dragon. In the tape list, folders come first:
Enter opens one, `../`, Backspace or Left goes back up. Tapes
play in real time: the Dragon's motor relay starts and stops them, and
`AUDIO ON` lets you hear them through the TV. `.wav` files can be 8 or
16-bit, mono or stereo, any sample rate (recordings of real tapes work;
the input copes with quiet and noisy recordings). `cmdline.txt`
`tape=NAME.cas` (or `tape=FOLDER/NAME.cas`) inserts a tape at power-on.

The card comes with two demo tapes: `CIRCLES` (`PMODE 4` lines and a
circle) and `COLOURS` (`PMODE 3` with `PAINT`).

`CSAVE "NAME"` (and `CSAVEM`, data files) record to `tapes/NAME.wav` (a
real tape recording: play it into a real Dragon) and `tapes/NAME.cas`.
An existing name is never overwritten: the new one becomes `NAME-2`,
`NAME-3` and so on. Both kinds load in XRoar, and XRoar's load here.

## Roadmap

Done so far: the 6809 with real cycle timing (checked against XRoar),
the PIAs, SAM and 50 Hz interrupt, HDMI picture with every text,
semigraphics and graphics mode and the border, HDMI sound, USB keyboards,
the cassette deck (real-time CAS/WAV loading, WAV+CAS saving), Wi-Fi time,
the admin screen with soft/hard reset and safe shutdown. Tested with
Dunjunz, Blockdown, Return of the Beast and the CyD 3-channel music player.

Next, roughly in order:

- **Fast tape loading**: an admin switch for instant `CLOAD`/`CLOADM`
  (intercepting the ROM's byte and bit readers, as XRoar does); real time
  stays the default.
- **Clean copies of real tapes**: while a WAV recording loads, write a clean
  `.cas` of what was read (like XRoar's tape rewrite).
- **A bigger card**: the image's FAT partition is 64 MB; WAV tapes fill it.
- **Snapshots**: save and restore the whole machine from the admin screen.
- **Printer to a file**: `PRINT#-2` / `LLIST` currently wait for a printer
  that isn't there; send them to `printer.txt` on the card.
- **DragonDOS disks**: the WD2797 floppy controller and cartridge port, with
  `.vdk` / `.dsk` images and your own DragonDOS ROM.
- **Wi-Fi file page**: copy tapes and disks to and from the card from a
  browser (the Zero W already joins Wi-Fi for the clock).
- **Joysticks**: USB gamepads as the Dragon's joysticks (the DAC comparator
  on PIA0 PA7) and fire buttons.
- Later: the SAM's fast-CPU poke (`POKE 65495,0`), cartridges, the Dragon 64
  and Tandy CoCo ROMs, sound on the Pi 5 (Circle has no HDMI sound there).

## Shared core

`src/core/` (CPU, PIAs, SAM, VDG, tape) is also the core of
[6809Emulator](https://github.com/dylan7474/6809Emulator), the terminal
version. Fix it here, then run `tools/sync-core.sh` to copy it across and
commit there too.

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
