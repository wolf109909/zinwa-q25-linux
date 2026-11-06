# SPDX-License-Identifier: GPL-2.0
# Zinwa Q25 OCA72xxx Smart Audio PA Driver (I2C + ASoC)

ifneq ($(KERNELRELEASE),)
ccflags-y += -I$(src)/../include
snd-soc-oca72xxx-objs := oca72xxx.o oca72xxx_device.o oca72xxx_monitor.o oca72xxx_bin_parse.o oca72xxx_dsp.o oca72xxx_acf_bin.o
obj-m += snd-soc-oca72xxx.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
