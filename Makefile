ifneq ($(KERNELRELEASE),)
# kbuild part

obj-m := hid-g13.o hid-g15.o hid-g15v2.o hid-g19.o hid-gfb.o hid-g110.o hid-ginput.o

else

KVERSION = $(shell uname -r)
KDIR := /lib/modules/$(KVERSION)/build
PWD := $(shell pwd)

#MODULE_INSTALL_DIR := /lib/modules/$(KVERSION)/updates/g-series
#MODS := hid-g13.ko hid-g15.ko hid-g15v2.ko hid-g19.ko hid-gfb.ko hid-g110.ko hid-ginput.ko

default:
	$(MAKE) -C $(KDIR) M=$(PWD)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

TAGS:
	$(MAKE) -C $(KDIR) M=$(PWD) TAGS

endif
