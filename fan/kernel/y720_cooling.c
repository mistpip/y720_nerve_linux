// SPDX-License-Identifier: GPL-2.0-only
/* Legacy Y720 cooling: fixed EC protocol + standard linked hwmon control.
 * Native writes are restricted to LENOVO 80Y4 / Y720-15IKB / 4GCN38WW.
 * No arbitrary EC/register access, mandatory userspace heartbeat or auto-load.
 */
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>
#include <linux/wmi.h>
#include <linux/workqueue.h>
#include "y720-fan-policy.h"

#define Y720_GAMEZONE_GUID "887B54E3-DDDC-4B2C-8B88-68A26A8835D0"
#define FAN_SUPPORT 12
#define FAN_SET 13
#define FAN_STATUS 20
#define MAX_BOOST_SECONDS 300

static bool allow_control;
module_param(allow_control, bool, 0400);
MODULE_PARM_DESC(allow_control, "Enable fixed-protocol hwmon control on verified 80Y4/4GCN38WW only (default false)");

struct y720_data {
	struct wmi_device *wdev;
	struct mutex lock;
	struct delayed_work restore_work;
	struct delayed_work guard_work;
	struct pci_dev *gpu;
	bool native;
	bool faulted;
	int last_error;
	bool suspend_curve;
	bool suspend_entry_pending;
	bool curve_entry_pending;
	int requested_mode;
	int mode;
	unsigned int target;
	unsigned int applied_target;
	unsigned int pwm;
	unsigned long settled_at;
	unsigned long target_writes;
	unsigned long mode_writes;
	unsigned long ramp_low_samples;
	unsigned long target_changed;
	unsigned long desired_since;
	unsigned int bad_rpm_samples[2];
	struct notifier_block reboot_nb;
	bool blocked;
	bool needs_restore;
	unsigned int retries;
	unsigned long deadline;
	int last_command; /* -1 unknown, 0 auto requested, 1 boost requested; NOT fan state */
};

static int y720_native_mode(struct y720_data *data, int mode);

static const struct dmi_system_id y720_dmi[] = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "Y720-15IKB"),
		},
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Y720-15IKB"),
		},
	},
	{}
};

/* Caller holds lock. Evaluate only the explicitly selected, fixed method. */
static int y720_query(struct y720_data *data, u32 method, u32 *value)
{
	/* WMI requires an input descriptor even for a no-argument query. */
	struct acpi_buffer in = { 0, NULL };
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	int ret = -EPROTO;

	if (data->blocked)
		return -EBUSY;
	status = wmidev_evaluate_method(data->wdev, 0, method, &in, &out);
	if (ACPI_FAILURE(status)) {
		dev_err_ratelimited(&data->wdev->dev, "WMI query %u failed: %s\n",
				    method, acpi_format_exception(status));
		ret = -EIO;
		goto free_out;
	}
	obj = out.pointer;
	if (!obj)
		goto free_out;
	if (obj->type == ACPI_TYPE_INTEGER && obj->integer.value <= U32_MAX) {
		*value = obj->integer.value;
		ret = 0;
	} else if (obj->type == ACPI_TYPE_BUFFER && obj->buffer.length == 4) {
		__le32 raw;

		memcpy(&raw, obj->buffer.pointer, sizeof(raw));
		*value = le32_to_cpu(raw);
		ret = 0;
	}
free_out:
	kfree(out.pointer);
	return ret;
}

/* Successful ACPI transport does not prove physical fan response. */
static int y720_set(struct y720_data *data, bool enable)
{
	__le32 setting = cpu_to_le32(enable ? 1 : 0);
	struct acpi_buffer in = { sizeof(setting), &setting };
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status;

	if (data->native)
		return y720_native_mode(data, enable ? 0 : 2);

	status = wmidev_evaluate_method(data->wdev, 0, FAN_SET, &in, &out);
	kfree(out.pointer);
	data->last_command = ACPI_SUCCESS(status) ? enable : -1;
	if (ACPI_FAILURE(status)) {
		dev_err(&data->wdev->dev, "SetFanCooling(%u) transport failed: %s\n",
			enable, acpi_format_exception(status));
		return -EIO;
	}
	return 0;
}

static int y720_restore_locked(struct y720_data *data)
{
	int ret;

	if (!data->needs_restore)
		return 0;
	ret = y720_set(data, false);
	if (!ret) {
		data->needs_restore = false;
		data->deadline = 0;
	}
	return ret;
}

#include "y720-native.h"

static void y720_restore_work(struct work_struct *work)
{
	struct y720_data *data = container_of(to_delayed_work(work),
						struct y720_data, restore_work);
	int ret;

	mutex_lock(&data->lock);
	if (data->blocked ||
	    (data->native && data->requested_mode == 1) || !data->needs_restore || !data->deadline)
		goto unlock;
	if (time_before(jiffies, data->deadline)) {
		mod_delayed_work(system_wq, &data->restore_work, data->deadline - jiffies);
		goto unlock;
	}
	ret = y720_restore_locked(data);
	if (ret && data->retries++ < 2)
		mod_delayed_work(system_wq, &data->restore_work, 5 * HZ);
	else if (ret)
		dev_err(&data->wdev->dev, "Could not request firmware auto after three attempts\n");
unlock:
	mutex_unlock(&data->lock);
}

static ssize_t last_command_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct y720_data *data = dev_get_drvdata(dev);
	int value;

	mutex_lock(&data->lock);
	value = data->last_command;
	mutex_unlock(&data->lock);
	return sysfs_emit(buf, "%d\n", value);
}
static DEVICE_ATTR_RO(last_command);

/* Read-only diagnostics; these are not required by any control application. */
static ssize_t target_writes_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct y720_data *data = dev_get_drvdata(dev);
	unsigned long value;

	mutex_lock(&data->lock);
	value = data->target_writes;
	mutex_unlock(&data->lock);
	return sysfs_emit(buf, "%lu\n", value);
}
static DEVICE_ATTR_RO(target_writes);

static ssize_t mode_writes_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct y720_data *data = dev_get_drvdata(dev);
	unsigned long value;

	mutex_lock(&data->lock);
	value = data->mode_writes;
	mutex_unlock(&data->lock);
	return sysfs_emit(buf, "%lu\n", value);
}
static DEVICE_ATTR_RO(mode_writes);

static ssize_t ramp_low_samples_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct y720_data *data = dev_get_drvdata(dev);
	unsigned long value;

	mutex_lock(&data->lock);
	value = data->ramp_low_samples;
	mutex_unlock(&data->lock);
	return sysfs_emit(buf, "%lu\n", value);
}
static DEVICE_ATTR_RO(ramp_low_samples);

static ssize_t last_error_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct y720_data *data = dev_get_drvdata(dev);
	int value;

	mutex_lock(&data->lock);
	value = data->last_error;
	mutex_unlock(&data->lock);
	return sysfs_emit(buf, "%d\n", value);
}
static DEVICE_ATTR_RO(last_error);

static ssize_t safety_fault_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct y720_data *data = dev_get_drvdata(dev);
	bool faulted;

	mutex_lock(&data->lock);
	faulted = data->faulted;
	mutex_unlock(&data->lock);
	return sysfs_emit(buf, "%u\n", faulted);
}
static DEVICE_ATTR_RO(safety_fault);

static ssize_t requested_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct y720_data *data = dev_get_drvdata(dev);
	int mode;

	mutex_lock(&data->lock);
	mode = data->native ? data->requested_mode : -1;
	mutex_unlock(&data->lock);
	return sysfs_emit(buf, "%d\n", mode);
}
static DEVICE_ATTR_RO(requested_mode);

static ssize_t y720_query_show(struct device *dev, u32 method, char *buf)
{
	struct y720_data *data = dev_get_drvdata(dev);
	u32 value;
	int ret;

	mutex_lock(&data->lock);
	ret = y720_query(data, method, &value);
	mutex_unlock(&data->lock);
	return ret ? ret : sysfs_emit(buf, "%u\n", value);
}

static ssize_t fan_cooling_supported_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	return y720_query_show(dev, FAN_SUPPORT, buf);
}
static DEVICE_ATTR_RO(fan_cooling_supported);

static ssize_t firmware_status_raw_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return y720_query_show(dev, FAN_STATUS, buf);
}
static DEVICE_ATTR_RO(firmware_status_raw);

/* These mappings are verified against this machine's 4GCN38WW WMAA method.
 * Raw names deliberately avoid claiming units or physical sensor calibration.
 * CPU/GPU temperature methods 18/19 are constant-zero stubs, so omit them.
 */
#define Y720_QUERY_ATTR(name, method) \
	static ssize_t name##_show(struct device *dev, \
				   struct device_attribute *attr, char *buf) \
	{ \
		return y720_query_show(dev, method, buf); \
	} \
	static DEVICE_ATTR_RO(name)

Y720_QUERY_ATTR(interface_version, 11);
Y720_QUERY_ATTR(fan_count, 7);
Y720_QUERY_ATTR(fan1_speed_raw, 8);
Y720_QUERY_ATTR(fan2_speed_raw, 9);
Y720_QUERY_ATTR(fan_max_speed_raw, 10);
Y720_QUERY_ATTR(temperature_raw, 1);

/* 4GCN38WW reports FANS/FA2S * 100 in RPM. PWM is a normalized linked
 * RPM request, not an electrical duty measurement. Method 10 is not fan*_max.
 */
static umode_t y720_hwmon_visible(const void *unused,
				enum hwmon_sensor_types type, u32 attr, int channel)
{
	const struct y720_data *data = unused;

	if (data->native && channel == 0 &&
	    ((type == hwmon_pwm && (attr == hwmon_pwm_input || attr == hwmon_pwm_enable)) ||
	     (type == hwmon_fan && attr == hwmon_fan_target)))
		return 0644;
	if (type == hwmon_fan && channel >= 0 && channel < 2 &&
	    (attr == hwmon_fan_input || attr == hwmon_fan_label))
		return 0444;
	return 0;
}

static int y720_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long *val)
{
	struct y720_data *data = dev_get_drvdata(dev);
	u32 rpm;
	int ret;

	if (data->native)
		return y720_native_hwmon_read(data, type, attr, channel, val);
	if (type != hwmon_fan || attr != hwmon_fan_input || channel < 0 || channel > 1)
		return -EOPNOTSUPP;
	mutex_lock(&data->lock);
	ret = y720_query(data, 8 + channel, &rpm);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;
	/* Firmware sources are unsigned bytes, multiplied by 100 by AML. */
	if (rpm > 25500)
		return -ERANGE;
	*val = rpm;
	return 0;
}

static int y720_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
				  u32 attr, int channel, const char **str)
{
	if (type != hwmon_fan || attr != hwmon_fan_label || channel < 0 || channel > 1)
		return -EOPNOTSUPP;
	/* Physical CPU/GPU association has not been established. */
	*str = channel ? "Fan 2" : "Fan 1";
	return 0;
}

static const struct hwmon_ops y720_hwmon_ops = {
	.is_visible = y720_hwmon_visible,
	.read = y720_hwmon_read,
	.write = y720_native_hwmon_write,
	.read_string = y720_hwmon_read_string,
};

static const struct hwmon_channel_info * const y720_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_TARGET,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_chip_info y720_hwmon_chip = {
	.ops = &y720_hwmon_ops,
	.info = y720_hwmon_info,
};

static ssize_t boost_seconds_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct y720_data *data = dev_get_drvdata(dev);
	unsigned long remaining = 0;

	mutex_lock(&data->lock);
	if (data->needs_restore && time_before(jiffies, data->deadline))
		remaining = DIV_ROUND_UP(data->deadline - jiffies, HZ);
	mutex_unlock(&data->lock);
	return sysfs_emit(buf, "%lu\n", remaining);
}

static ssize_t boost_seconds_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct y720_data *data = dev_get_drvdata(dev);
	unsigned int seconds;
	u32 supported;
	int ret;

	if (!allow_control)
		return -EPERM;
	ret = kstrtouint(buf, 10, &seconds);
	if (ret)
		return ret;
	if (seconds > MAX_BOOST_SECONDS)
		return -EINVAL;
	mutex_lock(&data->lock);
	if (data->blocked) {
		ret = -EBUSY;
		goto unlock;
	}
	/* The legacy timer must not bypass a user-selected software curve. */
	if (data->native && data->requested_mode == 1) {
		ret = -EBUSY;
		goto unlock;
	}
	if (seconds) {
		ret = y720_query(data, FAN_SUPPORT, &supported);
		if (ret)
			goto unlock;
		if (supported != 1) {
			ret = -EOPNOTSUPP;
			goto unlock;
		}
		/* Even a failed transaction may have reached firmware: arrange cleanup. */
		data->needs_restore = true;
		data->retries = 0;
		ret = y720_set(data, true);
		data->deadline = jiffies + (ret ? HZ : seconds * HZ);
		mod_delayed_work(system_wq, &data->restore_work, ret ? HZ : seconds * HZ);
	} else {
		data->needs_restore = true;
		data->retries = 0;
		ret = y720_restore_locked(data);
		if (ret) {
			data->deadline = jiffies;
			mod_delayed_work(system_wq, &data->restore_work, HZ);
		}
	}
unlock:
	mutex_unlock(&data->lock);
	return ret ? ret : count;
}
static struct device_attribute dev_attr_boost_seconds =
	__ATTR(boost_seconds, 0600, boost_seconds_show, boost_seconds_store);

static struct attribute *y720_attrs[] = {
	&dev_attr_target_writes.attr,
	&dev_attr_mode_writes.attr,
	&dev_attr_ramp_low_samples.attr,
	&dev_attr_last_error.attr,
	&dev_attr_safety_fault.attr,
	&dev_attr_requested_mode.attr,
	&dev_attr_last_command.attr,
	&dev_attr_fan_cooling_supported.attr,
	&dev_attr_firmware_status_raw.attr,
	&dev_attr_boost_seconds.attr,
	&dev_attr_interface_version.attr,
	&dev_attr_fan_count.attr,
	&dev_attr_fan1_speed_raw.attr,
	&dev_attr_fan2_speed_raw.attr,
	&dev_attr_fan_max_speed_raw.attr,
	&dev_attr_temperature_raw.attr,
	NULL
};
static const struct attribute_group y720_group = {
	.attrs = y720_attrs,
};

/* Stop new work before cancellation; no firmware write if we never controlled it. */
static int y720_quiesce(struct device *dev, bool save_policy)
{
	struct y720_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->lock);
	if (save_policy) {
		data->suspend_curve = data->native && data->requested_mode == 1;
		data->suspend_entry_pending = data->curve_entry_pending;
	}
	data->blocked = true;
	data->requested_mode = 2;
	data->curve_entry_pending = false;
	ret = y720_restore_locked(data);
	mutex_unlock(&data->lock);
	cancel_delayed_work_sync(&data->restore_work);
	cancel_delayed_work_sync(&data->guard_work);
	return ret;
}

/* Also cover a probe failure after hwmon became visible to userspace. */
static void y720_managed_cleanup(void *arg)
{
	struct y720_data *data = arg;

	if (y720_quiesce(&data->wdev->dev, false))
		dev_err(&data->wdev->dev, "Firmware auto request failed during managed cleanup\n");
}

static int y720_suspend(struct device *dev)
{
	struct y720_data *data = dev_get_drvdata(dev);
	int ret;

	ret = y720_quiesce(dev, true);

	/* If suspend is aborted on this callback's error, permit recovery commands. */
	if (ret) {
		mutex_lock(&data->lock);
		data->blocked = false;
		if (data->suspend_curve) {
			data->requested_mode = 1;
			data->curve_entry_pending = data->suspend_entry_pending;
			y720_curve_resume(data);
			data->suspend_curve = false;
		} else {
			data->deadline = jiffies;
			data->retries = 0;
			mod_delayed_work(system_wq, &data->restore_work, HZ);
		}
		mutex_unlock(&data->lock);
	}
	return ret;
}

static int y720_resume(struct device *dev)
{
	struct y720_data *data = dev_get_drvdata(dev);

	mutex_lock(&data->lock);
	data->blocked = false;
	data->last_command = -1;
	if (data->suspend_curve) {
		data->requested_mode = 1;
		data->curve_entry_pending = data->suspend_entry_pending;
		y720_curve_resume(data);
		data->suspend_curve = false;
	}
	mutex_unlock(&data->lock);
	return 0; /* Restore the requested curve, never firmware boost. */
}
static DEFINE_SIMPLE_DEV_PM_OPS(y720_pm, y720_suspend, y720_resume);

/* wmi_driver has no shutdown member in Linux 6.12. A blocking reboot notifier
 * provides shutdown cleanup on both 6.12 and newer WMI driver APIs.
 */
static int y720_reboot(struct notifier_block *nb, unsigned long event, void *unused)
{
	struct y720_data *data = container_of(nb, struct y720_data, reboot_nb);

	if (y720_quiesce(&data->wdev->dev, false))
		dev_err(&data->wdev->dev, "Firmware auto request failed before reboot/shutdown\n");
	return NOTIFY_DONE;
}

static int y720_probe(struct wmi_device *wdev, const void *context)
{
	struct y720_data *data;
	struct device *hwmon;
	int ret;

	if (!dmi_check_system(y720_dmi) || wmidev_instance_count(wdev) != 1)
		return -ENODEV;
	data = devm_kzalloc(&wdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->wdev = wdev;
	data->last_command = -1;
	mutex_init(&data->lock);
	INIT_DELAYED_WORK(&data->restore_work, y720_restore_work);
	INIT_DELAYED_WORK(&data->guard_work, y720_guard_work);
	data->reboot_nb.notifier_call = y720_reboot;
	dev_set_drvdata(&wdev->dev, data);
	if (allow_control) {
		ret = y720_native_init(data);
		if (ret)
			return ret;
	}
	ret = devm_add_action_or_reset(&wdev->dev, y720_managed_cleanup, data);
	if (ret)
		return ret;
	ret = devm_register_reboot_notifier(&wdev->dev, &data->reboot_nb);
	if (ret)
		return ret;
	/* Publish the verified RPM ABI only for the BIOS inspected and tested. */
	if (dmi_match(DMI_BIOS_VERSION, "4GCN38WW")) {
		hwmon = devm_hwmon_device_register_with_info(&wdev->dev, "y720_cooling",
						     data, &y720_hwmon_chip, NULL);
		if (IS_ERR(hwmon))
			return PTR_ERR(hwmon);
	}
	/* Native probe only reads known state; never takes control on loading. */
	dev_info(&wdev->dev, "Experimental Y720 interface; control %s\n",
		 allow_control ? "enabled" : "disabled");
	return devm_device_add_group(&wdev->dev, &y720_group);
}

static void y720_remove(struct wmi_device *wdev)
{
	if (y720_quiesce(&wdev->dev, false))
		dev_err(&wdev->dev, "Firmware auto request failed during removal/shutdown\n");
}

/* No MODULE_DEVICE_TABLE alias: this prototype must be loaded deliberately. */
static const struct wmi_device_id y720_ids[] = {
	{ Y720_GAMEZONE_GUID, NULL },
	{}
};
static struct wmi_driver y720_driver = {
	.driver = {
		.name = "y720_cooling",
		.pm = pm_sleep_ptr(&y720_pm),
	},
	.id_table = y720_ids,
	.probe = y720_probe,
	.remove = y720_remove,
};
module_wmi_driver(y720_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Experimental Y720 legacy GameZone cooling, read-only by default");
MODULE_VERSION("0.1");
