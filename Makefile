# SPDX-License-Identifier: MIT
KDIR ?= /lib/modules/$(shell uname -r)/build
PREFIX ?= /usr/local
.PHONY: all cli gui modules test clean
all: cli
cli:
	$(MAKE) -C userspace
gui:
	@if test -f gui/build/build.ninja; then meson setup --reconfigure gui/build gui --prefix="$(PREFIX)"; else meson setup gui/build gui --prefix="$(PREFIX)"; fi
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
