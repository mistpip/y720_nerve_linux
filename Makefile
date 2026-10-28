# SPDX-License-Identifier: MIT
KDIR ?= /lib/modules/$(shell uname -r)/build
.PHONY: all cli gui modules test clean
all: cli
cli:
	$(MAKE) -C userspace
gui:
	meson setup gui/build gui --prefix=/usr/local
	ninja -C gui/build
modules:
	$(MAKE) -C fan/kernel KDIR=$(KDIR)
	$(MAKE) -C keyboard/kernel KDIR=$(KDIR)
test: cli
	bash scripts/test.sh
clean:
	$(MAKE) -C userspace clean
	rm -rf build gui/build
# Kernel build products: use make -C fan/kernel clean and keyboard/kernel clean
# with the same KDIR used to compile. Never remove source directories.
