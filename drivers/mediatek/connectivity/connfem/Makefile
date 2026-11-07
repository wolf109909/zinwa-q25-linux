subdir-ccflags-y += -I/home/maxsun/zinwa-q25/drivers/include -include /home/maxsun/zinwa-q25/drivers/include/linux/q25_compat.h -Wno-error
ccflags-y += -I/home/maxsun/zinwa-q25/drivers/include -include /home/maxsun/zinwa-q25/drivers/include/linux/q25_compat.h -Wno-error
all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) M=$(M) -C $(KERNEL_SRC) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean

