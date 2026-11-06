# SPDX-License-Identifier: GPL-2.0
# Zinwa Q25 Power Supply & Charger Drivers (UPM6922 primary charger + UPM6722 charge pump)

ifneq ($(KERNELRELEASE),)
ccflags-y += -I$(src)/../include
obj-m += upm6922_charger.o
obj-m += upm6722.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
