/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "../kernel/y720-mapping.h"

int main(void)
{
	unsigned char altered[sizeof(y720_verified_descriptor)];
	unsigned char zero_radio[] = {3, 0};
	unsigned char nonzero_radio[] = {3, 1};
	unsigned int usage;
	struct y720_record_state record = {0};
	struct y720_radio_state radio = {0};
	unsigned char keyboard[33] = {1, 3};
	unsigned char modifiers = 0xff;
	size_t i;

	assert(y720_lookup_key(0xc0054, false) == KEY_MICMUTE);
	assert(y720_lookup_key(0xc0057, false) == KEY_CAMERA_ACCESS_TOGGLE);
	assert(y720_lookup_key(0xc0050, false) == KEY_KBDILLUMTOGGLE);
	assert(y720_lookup_key(0xc0053, false) == KEY_PROG1);
	assert(y720_lookup_key(0x70072, false) == KEY_TOUCHPAD_TOGGLE);
	assert(y720_lookup_key(0x70071, false) == 0);
	assert(y720_lookup_key(0x70071, true) == KEY_RECORD);
	for (usage = 0; usage <= 0xffff; usage++) {
		unsigned int keyboard = 0x70000 | usage;
		unsigned int consumer = 0xc0000 | usage;

		if (usage != 0x72)
			assert(!y720_lookup_key(keyboard, false));
		if (usage != 0x50 && usage != 0x53 && usage != 0x54 && usage != 0x57)
			assert(!y720_lookup_key(consumer, false));
	}
	assert(y720_descriptor_matches(y720_verified_descriptor, sizeof(altered)));
	assert(!y720_descriptor_matches(NULL, sizeof(altered)));
	assert(!y720_descriptor_matches(y720_verified_descriptor, sizeof(altered)-1));
	for (i = 0; i < sizeof(altered); i++) {
		memcpy(altered, y720_verified_descriptor, sizeof(altered));
		altered[i] ^= 1;
		assert(!y720_descriptor_matches(altered, sizeof(altered)));
	}
	assert(y720_zero_radio_packet(3, zero_radio, 2));
	assert(!y720_zero_radio_packet(2, zero_radio, 2));
	assert(!y720_zero_radio_packet(3, zero_radio, 1));
	assert(!y720_zero_radio_packet(3, nonzero_radio, 2));
	assert(!y720_zero_radio_packet(3, NULL, 2));
	y720_radio_arm(&radio, 100, 2000);
	assert(!y720_radio_pulse(&radio, 3, zero_radio, 2, true, true, true, 2099));
	assert(!y720_radio_pulse(&radio, 3, zero_radio, 2, false, true, true, 2100));
	assert(!y720_radio_pulse(&radio, 3, zero_radio, 2, true, false, true, 2100));
	assert(!y720_radio_pulse(&radio, 3, zero_radio, 2, true, true, false, 2100));
	assert(!y720_radio_pulse(&radio, 2, zero_radio, 2, true, true, true, 2100));
	assert(!y720_radio_pulse(&radio, 3, zero_radio, 1, true, true, true, 2100));
	assert(!y720_radio_pulse(&radio, 3, NULL, 2, true, true, true, 2100));
	assert(y720_radio_pulse(&radio, 3, zero_radio, 2, true, true, true, 2100));
	assert(!y720_radio_pulse(&radio, 3, zero_radio, 2, true, true, true, 2249));
	assert(y720_radio_pulse(&radio, 3, zero_radio, 2, true, true, true, 2250));
	y720_radio_arm(&radio, 10000, 5000);
	assert(!y720_radio_pulse(&radio, 3, zero_radio, 2, true, true, true, 14999));
	assert(y720_radio_pulse(&radio, 3, zero_radio, 2, true, true, true, 15000));
	assert(!y720_radio_pulse(&radio, 3, nonzero_radio, 2, true, true, true, 16000));
	assert(radio.standard_seen);
	assert(!y720_radio_pulse(&radio, 3, zero_radio, 2, true, true, true, 17000));
	y720_radio_arm(&radio, 20000, 5000);
	assert(!y720_radio_pulse(&radio, 3, zero_radio, 2, true, true, true, 30000));
	for (usage = 1; usage < 256; ++usage) {
		unsigned char unexpected[] = {3, usage, 0};
		struct y720_radio_state state = {0};

		assert(!y720_radio_pulse(&state, 3, unexpected, 3, true, true, true, 40000));
		assert(!y720_radio_pulse(&state, 3, zero_radio, 2, true, true, true, 50000));
	}
	assert(y720_keyboard_modifiers(1, keyboard, 33, &modifiers) && modifiers == 3);
	assert(!y720_keyboard_modifiers(2, keyboard, 33, &modifiers));
	assert(!y720_keyboard_modifiers(1, keyboard, 32, &modifiers));
	assert(!y720_keyboard_modifiers(1, NULL, 33, &modifiers));
	assert(!y720_keyboard_modifiers(1, keyboard, 33, NULL));
	for (usage = 0; usage < 256; usage++) {
		unsigned int expected = usage ? KEY_F22 : KEY_RECORD;

		assert(!y720_record_event(&record, usage, 0));
		assert(y720_record_event(&record, usage, 1) == expected);
		/* Modifier changes while held must not change the matching release. */
		assert(y720_record_event(&record, usage ^ 3, 1) == expected);
		assert(y720_record_event(&record, usage ^ 0xff, 0) == expected);
		assert(!record.down);
	}
	/* Captured macro: plain F22, then Ctrl+Shift+F22 with modifiers released first. */
	assert(y720_record_event(&record, 0, 1) == KEY_RECORD);
	assert(y720_record_event(&record, 0, 0) == KEY_RECORD);
	assert(y720_record_event(&record, 3, 1) == KEY_F22);
	assert(y720_record_event(&record, 2, 1) == KEY_F22);
	assert(y720_record_event(&record, 0, 0) == KEY_F22);
	puts("PASS: mappings, passthrough, descriptor, radio guards, modifier-aware record state");
	return 0;
}
