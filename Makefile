#
# 6809Emulator-Metal - bare-metal Dragon 32 for the Raspberry Pi
#
#   make            build every kernel in TARGETS into build/
#   make pi3        just one target: pi1 = Zero / Zero W / Pi 1, pi2 = Pi 2,
#                   pi3 = Zero 2 W / Pi 3, pi4 = Pi 4 / 400, pi5 = Pi 5 / 500
#   make sdcard     gather everything for the SD card in build/sdcard/
#   make test       PC build of the same core: keyboard map test + BASIC
#                   smoke test (needs local/d32.rom, else it skips)
#   make pc         just the PC build (build/pc/dragon_pc)
#   make clean      remove our objects and kernels (keeps the built circle-stdlib copies)
#   make distclean  also remove the circle-stdlib copies and firmware
#   make qemu       run the 64-bit kernel in QEMU (see tools/qemu-sd.sh)
#
# Each target builds in its own build/<target>/ copy of circle-stdlib and
# src/, because Circle and Rules.mk build in place.  The submodule under
# libs/ is only ever read.
#

TARGETS   ?= pi1 pi2 pi3 pi4 pi5
CC        ?= cc
PC_CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra

# target -> Circle model number, toolchain prefix, newlib arch, kernel name
pi3_RASPPI = 3
pi3_PREFIX = aarch64-none-elf-
pi3_ARCH   = aarch64-none-circle
pi3_KERNEL = kernel8.img

pi1_RASPPI = 1
pi1_PREFIX = arm-none-eabi-
pi1_ARCH   = arm-none-circle
pi1_KERNEL = kernel.img

pi2_RASPPI = 2
pi2_PREFIX = arm-none-eabi-
pi2_ARCH   = arm-none-circle
pi2_KERNEL = kernel7.img

pi4_RASPPI = 4
pi4_PREFIX = aarch64-none-elf-
pi4_ARCH   = aarch64-none-circle
pi4_KERNEL = kernel8-rpi4.img

pi5_RASPPI = 5
pi5_PREFIX = aarch64-none-elf-
pi5_ARCH   = aarch64-none-circle
pi5_KERNEL = kernel_2712.img

# Extra Circle options per target.  USE_USB_FIQ (Pi 1-3 only; the Pi 4/5
# USB is different): the USB controller's 8000 interrupts a second go to
# the FIQ, so other interrupts (HDMI sound) can't delay them.  Without it
# a Zero W with HDMI sound lost key presses, saw stray USB "devices
# changed" events and once lost USB altogether (2026-11-22).
pi1_OPTIONS = -o USE_USB_FIQ
pi2_OPTIONS = -o USE_USB_FIQ
pi3_OPTIONS = -o USE_USB_FIQ

STDLIB    = libs/circle-stdlib

# Parts of circle-stdlib we never build (~310 MB of each copy):
# networking, TLS, JSON, test framework and the LVGL graphics add-on.
STDLIB_UNUSED = --exclude=/libs/mongoose/ --exclude=/libs/mbedtls/ --exclude=/libs/json/ \
		--exclude=/libs/doctest/ --exclude=/libs/circle/addon/lvgl/

# Copies keep their build output (excluded files are never deleted)
RSYNC_SRC = rsync -a --delete --exclude=.git --exclude='*.o' --exclude='*.d' \
		--exclude='*.elf' --exclude='*.img' --exclude='*.map' --exclude='*.lst'

.PHONY: all sdcard clean distclean submodules qemu pc test FORCE $(TARGETS)

all: $(TARGETS)

submodules:
	@test -f $(STDLIB)/libs/circle/Rules.mk || git submodule update --init --recursive

# ---- per-target rules ------------------------------------------------------
define TARGET_RULES

# 0. The Circle options this target was built with: a change rebuilds its copy
build/$(1)/circle-options: FORCE
	@mkdir -p build/$(1)
	@echo '$($(1)_OPTIONS)' | cmp -s - $$@ || echo '$($(1)_OPTIONS)' > $$@

# 1. Our own copy of circle-stdlib, configured and built for this target (~2 min)
build/$(1)/circle-stdlib/install/$($(1)_ARCH)/lib/libc.a: build/$(1)/circle-options | submodules
	rm -rf build/$(1)/circle-stdlib
	mkdir -p build/$(1)
	rsync -a --exclude=.git --exclude=/build/ --exclude=/install/ \
		--exclude='*.o' --exclude='*.d' --exclude='*.a' --exclude=Config.mk \
		--exclude=Config2.mk $(STDLIB_UNUSED) $(STDLIB)/ build/$(1)/circle-stdlib/
	mkdir -p build/$(1)/circle-stdlib/build/circle-newlib build/$(1)/circle-stdlib/install
	cd build/$(1)/circle-stdlib && ./configure -r $($(1)_RASPPI) -p $($(1)_PREFIX) $($(1)_OPTIONS) && $$(MAKE)

# 2. The kernel: a refreshed copy of src/, then Circle's Rules.mk
$(1): build/$(1)/circle-stdlib/install/$($(1)_ARCH)/lib/libc.a
	$(RSYNC_SRC) src/ build/$(1)/src/
	$$(MAKE) -C build/$(1)/src CIRCLESTDLIB=../circle-stdlib
	cp build/$(1)/src/$($(1)_KERNEL) build/$($(1)_KERNEL)
	@ls -l build/$($(1)_KERNEL)
$(if $(filter pi4,$(1)),	$$(MAKE) -C build/pi4/circle-stdlib/libs/circle/boot/armstub armstub8-rpi4.bin
	cp build/pi4/circle-stdlib/libs/circle/boot/armstub/armstub8-rpi4.bin build/)

endef
$(foreach t,$(TARGETS),$(eval $(call TARGET_RULES,$(t))))

# ---- PC build + tests (same core, renderer and keyboard map) ---------------
CORE_SRC = src/core/dragon_cpu.c src/core/vdg.c src/core/tape.c
CORE_H   = src/core/dragon.h src/core/tape.h

build/pc/dragon_pc: tools/pc/dragon_pc.c $(CORE_SRC) $(CORE_H)
	mkdir -p build/pc
	$(CC) $(PC_CFLAGS) -o $@ tools/pc/dragon_pc.c $(CORE_SRC)

build/pc/keymap_test: tests/keymap_test.c src/keymap_usb.c src/keymap_usb.h $(CORE_SRC) $(CORE_H)
	mkdir -p build/pc
	$(CC) $(PC_CFLAGS) -o $@ tests/keymap_test.c src/keymap_usb.c $(CORE_SRC)

pc: build/pc/dragon_pc

test: build/pc/dragon_pc build/pc/keymap_test
	build/pc/keymap_test
	tests/pc_smoke.sh $(ROM)

# ---- SD card ----------------------------------------------------------------
# Pi firmware pinned to the revision Circle uses.  bootcode.bin, start.elf
# and fixup.dat boot every Pi up to the Pi 3, start4.elf/fixup4.dat the Pi 4;
# the Pi 5 boots from its EEPROM and needs only its device tree.
# config.txt picks the kernel by model.  Add d32.rom yourself.
FIRMWARE_REV = 0641c5bf25d185d5bf25d8dcafd1da6dc8fdbf68
FIRMWARE_URL = https://raw.githubusercontent.com/raspberrypi/firmware/$(FIRMWARE_REV)/boot
FIRMWARE     = bootcode.bin start.elf fixup.dat start4.elf fixup4.dat LICENCE.broadcom \
	       bcm2708-rpi-zero-w.dtb bcm2710-rpi-zero-2-w.dtb \
	       bcm2711-rpi-4-b.dtb bcm2711-rpi-400.dtb \
	       bcm2712-rpi-5-b.dtb bcm2712d0-rpi-5-b.dtb bcm2712-rpi-500.dtb \
	       overlays/bcm2712d0.dtbo

# One file per target, so adding a name to FIRMWARE fetches just that file.
# Bump FIRMWARE_REV -> run `make distclean` (or delete build/firmware).
build/firmware/%:
	mkdir -p $(dir $@)
	curl -fsSL --retry 3 --retry-all-errors -o $@.part $(FIRMWARE_URL)/$*
	mv $@.part $@

# Wi-Fi chip firmware (Zero W, Zero 2 W, Pi 3, Pi 4/400, Pi 5): the files and
# revision Circle's addon/wlan/firmware/Makefile names; on the card in firmware/.
WLAN_FW_REV = c9d3ae6584ab79d19a4f94ccf701e888f9f87a53
WLAN_FW_URL = https://github.com/RPi-Distro/firmware-nonfree/raw/$(WLAN_FW_REV)/debian/config/brcm80211
# card name:source path
WLAN_FW = brcmfmac43430-sdio.bin:cypress/cyfmac43430-sdio.bin \
	  brcmfmac43430-sdio.txt:brcm/brcmfmac43430-sdio.txt \
	  brcmfmac43430-sdio.clm_blob:cypress/cyfmac43430-sdio.clm_blob \
	  brcmfmac43436-sdio.bin:brcm/brcmfmac43436-sdio.bin \
	  brcmfmac43436-sdio.txt:brcm/brcmfmac43436-sdio.txt \
	  brcmfmac43436-sdio.clm_blob:brcm/brcmfmac43436-sdio.clm_blob \
	  brcmfmac43436s-sdio.bin:brcm/brcmfmac43436s-sdio.bin \
	  brcmfmac43436s-sdio.txt:brcm/brcmfmac43436s-sdio.txt \
	  brcmfmac43455-sdio.bin:cypress/cyfmac43455-sdio-minimal.bin \
	  brcmfmac43455-sdio.txt:brcm/brcmfmac43455-sdio.txt \
	  brcmfmac43455-sdio.clm_blob:cypress/cyfmac43455-sdio.clm_blob \
	  brcmfmac43456-sdio.bin:brcm/brcmfmac43456-sdio.bin \
	  brcmfmac43456-sdio.txt:brcm/brcmfmac43456-sdio.txt \
	  brcmfmac43456-sdio.clm_blob:brcm/brcmfmac43456-sdio.clm_blob \
	  brcmfmac43455-sdio.raspberrypi,5-model-b.bin:cypress/cyfmac43455-sdio-minimal.bin \
	  brcmfmac43455-sdio.raspberrypi,5-model-b.txt:brcm/brcmfmac43455-sdio.txt \
	  brcmfmac43455-sdio.raspberrypi,5-model-b.clm_blob:cypress/cyfmac43455-sdio.clm_blob
WLAN_FW_FILES = $(addprefix build/wlan-firmware/,$(foreach f,$(WLAN_FW),$(firstword $(subst :, ,$(f)))))

build/wlan-firmware/%:
	mkdir -p build/wlan-firmware
	curl -fsSL --retry 3 --retry-all-errors -o "$@.part" \
		"$(WLAN_FW_URL)/$(word 2,$(subst :, ,$(filter $*:%,$(WLAN_FW))))"
	mv "$@.part" "$@"

sdcard: all $(addprefix build/firmware/,$(FIRMWARE)) $(WLAN_FW_FILES)
	rm -rf build/sdcard
	mkdir -p build/sdcard/overlays
	cd build/firmware && cp --parents $(FIRMWARE) ../sdcard/
	mkdir -p build/sdcard/firmware
	cp build/wlan-firmware/* build/sdcard/firmware/
	cp sdcard/config.txt sdcard/cmdline.txt sdcard/wpa_supplicant.conf.example build/sdcard/
	cp $(foreach t,$(TARGETS),build/$($(t)_KERNEL)) build/sdcard/
	$(if $(filter pi4,$(TARGETS)),cp build/armstub8-rpi4.bin build/sdcard/)
	@echo
	@echo "build/sdcard/ is ready: copy it to a FAT32 card, then add d32.rom"
	@echo "(and wpa_supplicant.conf for Wi-Fi time: see wpa_supplicant.conf.example)."

# ---- housekeeping -------------------------------------------------------------
clean:
	rm -rf build/sdcard build/pc $(foreach t,$(TARGETS),build/$(t)/src \
		build/$($(t)_KERNEL)) build/armstub8-rpi4.bin

distclean: clean
	rm -rf $(foreach t,$(TARGETS),build/$(t)) build/firmware build/wlan-firmware

# Try it in QEMU's Raspberry Pi 3 model (make the card first with tools/qemu-sd.sh).
# Serial output (the log) goes to the terminal.
qemu: pi3
	@test -f build/qemu-sd.img || { echo "Run tools/qemu-sd.sh <d32.rom> first"; exit 1; }
	qemu-system-aarch64 -M raspi3b -kernel build/kernel8.img \
		-append "width=1024 height=768 logdev=ttyS1" \
		-drive file=build/qemu-sd.img,if=sd,format=raw \
		-serial stdio -usb -device usb-kbd
