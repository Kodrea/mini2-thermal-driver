obj-m += rs300.o

KERNELRELEASE ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNELRELEASE)/build
SRCDIR ?= $(CURDIR)/../src

all: $(PWD)/rs300.c
	$(MAKE) -C $(KDIR) M=$(PWD) modules

$(PWD)/rs300.c:
	ln -sf $(SRCDIR)/rs300.c $@

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.dtbo *.dts.preprocessed
	@# Only remove rs300.c if it is the symlink this Makefile created.
	@# A real rs300.c in $(PWD) is a DKMS-copied source; deleting it would
	@# wipe the build input when DKMS runs `make clean` before `make`.
	@test -L $(PWD)/rs300.c && rm -f $(PWD)/rs300.c || true

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

# Zero 3W overlay (requires cpp preprocessing for dt-bindings includes).
# Source: src/rs300-rk3566e-overlay.dts
# Output: radxa-zero3-rs300-thermal.dtbo (name matches Radxa's /boot/dtbo
# convention and the dtbo filename deployed on the reference board)
dtbo: radxa-zero3-rs300-thermal.dtbo

radxa-zero3-rs300-thermal.dtbo: $(SRCDIR)/rs300-rk3566e-overlay.dts
	cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
		-I $(KDIR)/include \
		$< -o $(basename $@).dts.preprocessed
	dtc -q -@ -H epapr -O dtb -o $@ $(basename $@).dts.preprocessed
	@rm -f $(basename $@).dts.preprocessed
	@echo "Built $@ ($$(stat -c%s $@) bytes)"
