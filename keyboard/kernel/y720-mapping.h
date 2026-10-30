/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Y720_MAPPING_H
#define Y720_MAPPING_H

#ifdef __KERNEL__
#include <linux/input-event-codes.h>
#include <linux/string.h>
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <linux/input-event-codes.h>
#endif

/* Exact, unmodified input descriptor read from the authorized machine. */
static const unsigned char y720_verified_descriptor[] = {
	0x06, 0x89, 0xff, 0x09, 0x10, 0xa1, 0x01, 0x85, 0x5a, 0x09, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00,
	0x75, 0x08, 0x95, 0x10, 0xb1, 0x00, 0xc0, 0x06, 0x89, 0xff, 0x09, 0xcd, 0xa1, 0x01, 0x85, 0xcd,
	0x09, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02, 0xb1, 0x00, 0xc0, 0x05, 0x01,
	0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x75, 0x01, 0x95, 0x08, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
	0x15, 0x00, 0x25, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x03, 0x95, 0x05, 0x75, 0x01,
	0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x03, 0x95, 0x1e,
	0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2a, 0xff, 0x00, 0x81, 0x00,
	0xc0, 0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x02, 0x19, 0x00, 0x2a, 0x3c, 0x02, 0x15, 0x00,
	0x26, 0x3c, 0x02, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xc0, 0x05, 0x01, 0x09, 0x0c, 0xa1, 0x01,
	0x85, 0x03, 0x15, 0x00, 0x25, 0x01, 0x09, 0xc6, 0x95, 0x01, 0x75, 0x01, 0x81, 0x06, 0x75, 0x07,
	0x81, 0x03, 0xc0
};

static inline bool y720_descriptor_matches(const unsigned char *data, size_t size)
{
	return data && size == sizeof(y720_verified_descriptor) &&
		!memcmp(data, y720_verified_descriptor, size);
}

/* Zero means leave the normal HID mapper alone, not KEY_RESERVED. */
static inline unsigned int y720_lookup_key(unsigned int usage, bool map_record)
{
	switch (usage) {
	case 0x000c0050: return KEY_KBDILLUMTOGGLE;
	/* Firmware emits this on keypad 7 with NumLock OFF, not on Fn+7.
	 * NumLock ON emits keyboard usage 0x7005f; leave that path untouched.
	 */
	case 0x000c0053: return KEY_PROG1;
	case 0x000c0054: return KEY_MICMUTE;
	case 0x000c0057: return KEY_CAMERA_ACCESS_TOGGLE;
	case 0x00070072: return KEY_TOUCHPAD_TOGGLE; /* keyboard usage F23 */
	case 0x00070071: return map_record ? KEY_RECORD : 0; /* verified plain F22 */
	default: return 0;
	}
}

static inline bool y720_zero_radio_packet(unsigned int id,
					const unsigned char *data, int size)
{
	return data && id == 3 && size == 2 && data[0] == 3 && data[1] == 0;
}

/* Opt-in legacy pulse quirk only, never a generic interpretation of zero HID.
 * A conventional nonzero button report disables reinterpretation until reprobe:
 * its following zeros may be releases rather than independent clicks.
 */
struct y720_radio_state {
	unsigned long long armed_after_ms;
	unsigned long long last_pulse_ms;
	bool have_last;
	bool standard_seen;
};

static inline void y720_radio_arm(struct y720_radio_state *state,
		unsigned long long now_ms, unsigned int settle_ms)
{
	state->armed_after_ms = now_ms + settle_ms;
	state->have_last = false;
	/* Do not forget normal-button evidence across suspend/resume. */
}

static inline bool y720_radio_pulse(struct y720_radio_state *state,
		unsigned int id, const unsigned char *packet, int size,
		bool verified, bool enabled, bool ready, unsigned long long now_ms)
{
	if (!state || !verified || !packet || id != 3 || size < 2 || packet[0] != 3)
		return false;
	if (packet[1]) {
		state->standard_seen = true;
		return false;
	}
	if (size != 2 || !enabled || !ready || state->standard_seen ||
	    now_ms < state->armed_after_ms)
		return false;
	if (state->have_last && (now_ms < state->last_pulse_ms ||
	    now_ms - state->last_pulse_ms < 150))
		return false;
	state->last_pulse_ms = now_ms;
	state->have_last = true;
	return true;
}

static inline bool y720_keyboard_modifiers(unsigned int id,
		const unsigned char *data, int size, unsigned char *modifiers)
{
	if (!data || !modifiers || id != 1 || size != 33 || data[0] != 1)
		return false;
	*modifiers = data[1];
	return true;
}

struct y720_record_state {
	bool down;
	unsigned int key;
};

/* One physical record tap also emits a separate modified F22 on this unit.
 * Keep modified F22 intact, and latch the selected key until its own release.
 */
static inline unsigned int y720_record_event(struct y720_record_state *state,
		unsigned char modifiers, int value)
{
	if (!value) {
		if (!state->down)
			return 0;
		state->down = false;
		return state->key;
	}
	if (!state->down) {
		state->key = modifiers ? KEY_F22 : KEY_RECORD;
		state->down = true;
	}
	return state->key;
}
#endif
