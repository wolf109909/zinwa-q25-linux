# SPDX-License-Identifier: GPL-2.0
# Out-of-tree Kbuild Makefile for Zinwa Q25 Keyboard & Trackpad drivers (bbqX0kbd & q20_keyboard)

ccflags-$(CONFIG_DRM_MEDIATEK_V2) += -I$(srctree)/drivers/gpu/drm/mediatek/mediatek_v2/

obj-$(CONFIG_KEYBOARD_BBQX0_KEYBOARD) += bbqX0kbd.o
obj-$(CONFIG_KEYBOARD_Q20_KEYBOARD)   += q20_keyboard.o

# When built out-of-tree via M=...
ifeq ($(CONFIG_KEYBOARD_BBQX0_KEYBOARD),)
obj-m += bbqX0kbd.o
endif
ifeq ($(CONFIG_KEYBOARD_Q20_KEYBOARD),)
obj-m += q20_keyboard.o
endif

bbqX0kbd-y := bbqX0kbd_main.o bbqX0kbd_i2cHelper.o
