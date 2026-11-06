# SPDX-License-Identifier: GPL-2.0
# Zinwa Q25 Camera Flash / Torch V4L2 Subdev Drivers

ifneq ($(KERNELRELEASE),)
obj-m += flashlight-gpio-v4l2.o
obj-m += aw36518.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
