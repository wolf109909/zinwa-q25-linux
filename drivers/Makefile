# SPDX-License-Identifier: GPL-2.0
# Zinwa Q25 Out-of-Tree Hardware Drivers for Mainline Linux 7.0+ (MT6789 / MT8781)

ifneq ($(KERNELRELEASE),)
obj-m += keyboard/
obj-m += touchscreen/
obj-m += display/
obj-m += power/
obj-m += audio/
obj-m += nfc/
obj-m += leds/
else
KDIR ?= /lib/modules/$(shell uname -r)/build
KBUILD_MODPOST_WARN ?= $(if $(wildcard $(KDIR)/Module.symvers),0,1)

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) KBUILD_MODPOST_WARN=$(KBUILD_MODPOST_WARN) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
