# SPDX-License-Identifier: GPL-2.0-only
# Input only. Deliberately no y720_hid-rgb module in this distribution.
obj-m := y720_hid.o
KDIR ?= /lib/modules/$(shell uname -r)/build
.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
