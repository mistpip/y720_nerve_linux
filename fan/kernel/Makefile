# SPDX-License-Identifier: GPL-2.0-only
obj-m := y720_cooling.o
Y720_VERSION_DIR := $(if $(src),$(src),$(CURDIR))
Y720_VERSION := $(shell cat $(Y720_VERSION_DIR)/../VERSION 2>/dev/null || cat $(Y720_VERSION_DIR)/../../VERSION)
ccflags-y += -DY720_VERSION='"$(Y720_VERSION)"'
KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
