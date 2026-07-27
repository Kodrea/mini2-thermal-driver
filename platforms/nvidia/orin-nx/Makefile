obj-m += rs300.o
rs300-y := src/rs300.o
ccflags-y += -I/usr/src/nvidia/nvidia-oot/include

ifdef DEBUG
ccflags-y += -DDEBUG
endif

KERNELRELEASE ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNELRELEASE)/build
SRCDIR ?= $(CURDIR)/src
KBUILD_EXTRA_SYMBOLS += /usr/src/nvidia/nvidia-oot/Module.symvers

ifneq (,$(findstring n,$(MAKEFLAGS)))
all:
	@echo "DRY-RUN: $(MAKE) -C $(KDIR) M=$(CURDIR) modules"
else
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
endif

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a

dtbo: rs300-cam1.dtbo

rs300-cam1.dtbo: $(SRCDIR)/rs300-cam1-overlay.dts
	dtc -@ -H epapr -I dts -O dtb -o $@ $<
