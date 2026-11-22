#
# Circle kernel Makefile: the Dragon core + the Circle platform layer.
# Run by the top-level Makefile from a per-target copy, build/<target>/src,
# with CIRCLESTDLIB pointing at that target's own circle-stdlib.
#

CIRCLESTDLIB ?= ../circle-stdlib
include $(CIRCLESTDLIB)/Config.mk

OBJS = main.o kernel.o admin.o keymap_usb.o core/dragon_cpu.o core/vdg.o core/tape.o

include $(CIRCLEHOME)/Rules.mk

INCLUDE := -I. $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)
OPTIMIZE = -O3

# Wi-Fi (internet time): Circle's WLAN driver + WPA supplicant
LIBS := $(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
	$(CIRCLEHOME)/addon/wlan/libwlan.a $(CIRCLEHOME)/lib/sound/libsound.a $(CIRCLE_STDLIB_LIBS)

-include $(DEPS)
