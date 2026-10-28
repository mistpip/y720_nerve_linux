# SPDX-License-Identifier: MIT
CC ?= cc
CFLAGS ?= -O2 -g -fstack-protector-strong -fPIE
WARN = -std=c11 -Wall -Wextra -Wpedantic -Werror -Wformat=2 -Wshadow
CPPFLAGS += -D_FORTIFY_SOURCE=2
LDFLAGS += -pie -Wl,-z,relro,-z,now
BUILD ?= build
PROGRAMS = $(BUILD)/y720-fan $(BUILD)/y720-backlight
TESTS = $(BUILD)/test-c-fan $(BUILD)/test-c-backlight $(BUILD)/test-c-identity

.PHONY: all clean test
all: $(PROGRAMS)

$(BUILD):
	mkdir -p "$@"

$(BUILD)/y720-fan: fan/y720_fan.c common/cli.h Makefile | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $< -o $@ $(LDFLAGS) -lm

$(BUILD)/y720-backlight: backlight/y720_backlight.c common/cli.h Makefile | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $< -o $@ $(LDFLAGS) -lm

$(BUILD)/test-c-%: tests/test_c_%.c fan/y720_fan.c backlight/y720_backlight.c common/cli.h Makefile | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -fsanitize=address,undefined $< -o $@ $(LDFLAGS) -lm

test: all $(TESTS)
	$(BUILD)/test-c-fan
	$(BUILD)/test-c-backlight
	$(BUILD)/test-c-identity
	python3 -m unittest discover -s tests -p 'test_c*.py' -v

clean:
	rm -f -- $(PROGRAMS) $(TESTS)
