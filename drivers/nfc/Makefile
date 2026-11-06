# SPDX-License-Identifier: GPL-2.0
# Zinwa Q25 ST21NFC I2C NFC Controller Driver

ifneq ($(KERNELRELEASE),)
obj-m += st21nfc.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
