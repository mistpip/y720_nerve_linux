// SPDX-License-Identifier: GPL-2.0-only
/* Native input mapping only. No ACPI/EC writes or keyboard Feature commands. */
#include <linux/atomic.h>
#include <linux/dmi.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include "y720-mapping.h"

static bool map_record = true;
module_param(map_record, bool, 0400);
MODULE_PARM_DESC(map_record, "Map verified unmodified record-key F22 to KEY_RECORD; preserve modified F22 (default true)");

static bool radio_zero_pulse;
module_param(radio_zero_pulse, bool, 0400);
MODULE_PARM_DESC(radio_zero_pulse, "Opt in to verified legacy Fn+F7 zero-report pulse; default false, no direct radio writes");

struct y720_hid {
	struct input_dev *record_input;
	struct input_dev *radio_input;
	spinlock_t lock;
	struct y720_record_state record;
	struct y720_radio_state radio;
	u8 modifiers;
	atomic64_t zero_reports;
	atomic64_t radio_pulses;
	atomic64_t radio_suppressed;
	atomic64_t record_plain_presses;
	atomic64_t record_modified_presses;
	bool verified;
	bool ready;
};

static const struct dmi_system_id verified_dmi[] = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "80Y4"),
			DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Y720-15IKB"),
			DMI_EXACT_MATCH(DMI_BIOS_VERSION, "4GCN38WW"),
		},
	},
	{ }
};

static bool y720_verified(struct hid_device *hdev)
{
	return hdev->bus == BUS_USB && hdev->vendor == 0x048d &&
		hdev->product == 0xc100 && hdev->version == 0x0110 &&
		dmi_check_system(verified_dmi) &&
		y720_descriptor_matches(hdev->dev_rdesc, hdev->dev_rsize);
}

static bool y720_match(struct hid_device *hdev, bool ignore_special_driver)
{
	/* hid-generic checks ID tables, not other drivers' .match callbacks.
	 * Therefore a rejected same-ID descriptor would otherwise go unbound.
	 * Probe such devices in generic pass-through mode, without our mappings.
	 */
	return !ignore_special_driver;
}

static int y720_hid_mapping(struct hid_device *hdev, struct hid_input *hi,
			      struct hid_field *field, struct hid_usage *usage,
			      unsigned long **bit, int *max)
{
	struct y720_hid *data = hid_get_drvdata(hdev);
	unsigned int key = y720_lookup_key(usage->hid, map_record);

	if (!data->verified)
		return 0;
	if (usage->hid == HID_GD_RFKILL_BTN && field->application == HID_GD_WIRELESS_RADIO_CTLS) {
		data->radio_input = hi->input;
		return 0; /* Keep the standard KEY_RFKILL mapping and nonzero path. */
	}
	if (usage->hid == 0x00070071 && map_record) {
		data->record_input = hi->input;
		/* .event chooses between these, preserving real modified chords. */
		hid_map_usage_clear(hi, usage, bit, max, EV_KEY, KEY_F22);
		input_set_capability(hi->input, EV_KEY, KEY_RECORD);
		return 1;
	}
	if (!key)
		return 0;
	hid_map_usage_clear(hi, usage, bit, max, EV_KEY, key);
	return 1;
}

static int y720_raw_event(struct hid_device *hdev, struct hid_report *report,
			   u8 *packet, int size)
{
	struct y720_hid *data = hid_get_drvdata(hdev);
	unsigned long flags;
	bool zero;
	unsigned long long now_ms;

	if (report->type != HID_INPUT_REPORT)
		return 0;
	now_ms = ktime_get_mono_fast_ns() / NSEC_PER_MSEC;
	spin_lock_irqsave(&data->lock, flags);
	if (!data->verified)
		goto out;
	if (data->ready && y720_keyboard_modifiers(report->id, packet, size, &data->modifiers))
		goto out;
	zero = y720_zero_radio_packet(report->id, packet, size);
	if (zero)
		atomic64_inc(&data->zero_reports);
	if (y720_radio_pulse(&data->radio, report->id, packet, size,
			data->verified, radio_zero_pulse,
			data->ready && data->radio_input, now_ms)) {
		input_report_key(data->radio_input, KEY_RFKILL, 1);
		input_sync(data->radio_input);
		input_report_key(data->radio_input, KEY_RFKILL, 0);
		input_sync(data->radio_input);
		atomic64_inc(&data->radio_pulses);
	} else if (zero && radio_zero_pulse) {
		atomic64_inc(&data->radio_suppressed);
	}
out:
	spin_unlock_irqrestore(&data->lock, flags);
	return 0; /* Preserve hidraw visibility and normal report processing. */
}

static int y720_event(struct hid_device *hdev, struct hid_field *field,
		      struct hid_usage *usage, __s32 value)
{
	struct y720_hid *data = hid_get_drvdata(hdev);
	unsigned long flags;
	unsigned int key;
	bool was_down;

	if (!data->verified || !map_record || usage->hid != 0x00070071 || !field->hidinput)
		return 0;
	spin_lock_irqsave(&data->lock, flags);
	if (data->ready) {
		was_down = data->record.down;
		key = y720_record_event(&data->record, data->modifiers, value);
		if (key) {
			if (value && !was_down) {
				if (key == KEY_RECORD)
					atomic64_inc(&data->record_plain_presses);
				else
					atomic64_inc(&data->record_modified_presses);
			}
			input_report_key(field->hidinput->input, key, !!value);
		}
	}
	spin_unlock_irqrestore(&data->lock, flags);
	return 1;
}

/* Called under lock, while input devices are still alive. */
static void y720_release_record(struct y720_hid *data)
{
	unsigned int key = y720_record_event(&data->record, 0, 0);

	if (key && data->record_input) {
		input_report_key(data->record_input, key, 0);
		input_sync(data->record_input);
	}
	data->modifiers = 0;
}

static ssize_t radio_zero_reports_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct y720_hid *data = hid_get_drvdata(to_hid_device(dev));

	return sysfs_emit(buf, "%lld\n", atomic64_read(&data->zero_reports));
}
static DEVICE_ATTR_RO(radio_zero_reports);

static ssize_t radio_zero_pulses_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct y720_hid *data = hid_get_drvdata(to_hid_device(dev));

	return sysfs_emit(buf, "%lld\n", atomic64_read(&data->radio_pulses));
}
static DEVICE_ATTR_RO(radio_zero_pulses);

static ssize_t radio_zero_suppressed_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct y720_hid *data = hid_get_drvdata(to_hid_device(dev));

	return sysfs_emit(buf, "%lld\n", atomic64_read(&data->radio_suppressed));
}
static DEVICE_ATTR_RO(radio_zero_suppressed);

static ssize_t record_plain_presses_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct y720_hid *data = hid_get_drvdata(to_hid_device(dev));

	return sysfs_emit(buf, "%lld\n", atomic64_read(&data->record_plain_presses));
}
static DEVICE_ATTR_RO(record_plain_presses);

static ssize_t record_modified_presses_show(struct device *dev,
					   struct device_attribute *attr, char *buf)
{
	struct y720_hid *data = hid_get_drvdata(to_hid_device(dev));

	return sysfs_emit(buf, "%lld\n", atomic64_read(&data->record_modified_presses));
}
static DEVICE_ATTR_RO(record_modified_presses);

static ssize_t verified_mapping_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct y720_hid *data = hid_get_drvdata(to_hid_device(dev));

	return sysfs_emit(buf, "%u\n", data->verified);
}
static DEVICE_ATTR_RO(verified_mapping);

static struct attribute *y720_attrs[] = {
	&dev_attr_radio_zero_reports.attr,
	&dev_attr_radio_zero_pulses.attr,
	&dev_attr_radio_zero_suppressed.attr,
	&dev_attr_record_plain_presses.attr,
	&dev_attr_record_modified_presses.attr,
	&dev_attr_verified_mapping.attr,
	NULL
};
static const struct attribute_group y720_group = { .attrs = y720_attrs };

static int y720_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct y720_hid *data;
	unsigned long flags;
	int ret;

	data = devm_kzalloc(&hdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	atomic64_set(&data->zero_reports, 0);
	atomic64_set(&data->radio_pulses, 0);
	atomic64_set(&data->radio_suppressed, 0);
	atomic64_set(&data->record_plain_presses, 0);
	atomic64_set(&data->record_modified_presses, 0);
	spin_lock_init(&data->lock);
	data->verified = y720_verified(hdev);
	hid_set_drvdata(hdev, data);
	/* Preserve hid-generic's separate keyboard / radio input devices. */
	hdev->quirks |= HID_QUIRK_INPUT_PER_APP;
	ret = hid_parse(hdev);
	if (ret)
		return ret;
	if (!y720_descriptor_matches(hdev->rdesc, hdev->rsize))
		data->verified = false;
	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;
	ret = sysfs_create_group(&hdev->dev.kobj, &y720_group);
	if (ret) {
		hid_hw_stop(hdev);
		return ret;
	}
	spin_lock_irqsave(&data->lock, flags);
	y720_radio_arm(&data->radio, ktime_get_mono_fast_ns() / NSEC_PER_MSEC, 2000);
	data->ready = true;
	spin_unlock_irqrestore(&data->lock, flags);
	hid_info(hdev, "Input-only: verified_mapping=%u record=%u radio_zero_pulse=%u; no firmware writes\n",
		 data->verified, map_record, radio_zero_pulse);
	return 0;
}

static void y720_remove(struct hid_device *hdev)
{
	struct y720_hid *data = hid_get_drvdata(hdev);
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);
	data->ready = false;
	y720_release_record(data);
	spin_unlock_irqrestore(&data->lock, flags);
	sysfs_remove_group(&hdev->dev.kobj, &y720_group);
	hid_hw_stop(hdev);
}

#ifdef CONFIG_PM
static int y720_suspend(struct hid_device *hdev, pm_message_t message)
{
	struct y720_hid *data = hid_get_drvdata(hdev);
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);
	data->ready = false;
	y720_release_record(data);
	spin_unlock_irqrestore(&data->lock, flags);
	return 0;
}

static int y720_resume(struct hid_device *hdev)
{
	struct y720_hid *data = hid_get_drvdata(hdev);
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);
	y720_radio_arm(&data->radio, ktime_get_mono_fast_ns() / NSEC_PER_MSEC, 5000);
	data->ready = true;
	spin_unlock_irqrestore(&data->lock, flags);
	return 0;
}

static int y720_reset_resume(struct hid_device *hdev)
{
	struct y720_hid *data = hid_get_drvdata(hdev);
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);
	data->ready = false;
	y720_release_record(data);
	spin_unlock_irqrestore(&data->lock, flags);
	/* Preserve hid-generic's release of stale normal keys after USB reset. */
	hidinput_reset_resume(hdev);
	return y720_resume(hdev);
}
#endif

static const struct hid_device_id y720_ids[] = {
	{ HID_USB_DEVICE(0x048d, 0xc100) },
	{ }
};
/* Deliberate loading only: no autoload alias in this experimental package. */

static struct hid_driver y720_driver = {
	.name = "y720-hid",
	.id_table = y720_ids,
	.match = y720_match,
	.probe = y720_probe,
	.remove = y720_remove,
	.input_mapping = y720_hid_mapping,
	.raw_event = y720_raw_event,
	.event = y720_event,
#ifdef CONFIG_PM
	.suspend = y720_suspend,
	.resume = y720_resume,
	.reset_resume = y720_reset_resume,
#endif
};
module_hid_driver(y720_driver);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Y720 Linux experimental port");
MODULE_DESCRIPTION("Verified Y720 048d:c100 special-key mappings, no firmware writes");
MODULE_VERSION("0.1");
