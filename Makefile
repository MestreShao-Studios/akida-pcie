# SPDX-License-Identifier: GPL-2.0
# Out-of-tree build for the native Akida AKD1000 PCIe driver (Linux 7.0).
# Requires CONFIG_DW_EDMA (in-tree dw-edma) available as module or built-in.

obj-m += akida-pcie.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

info: all
	modinfo ./akida-pcie.ko

.PHONY: all clean info
