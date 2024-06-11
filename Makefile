KVERS = $(shell uname -r)
PWD := $(CURDIR)

obj-m += raid.o

raid-objs := raid_block.o

EXTRA_CFLAGS=-g -O0

build: kernel_modules

kernel_modules:
	make -C /lib/modules/$(KVERS)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(KVERS)/build M=$(PWD) clean