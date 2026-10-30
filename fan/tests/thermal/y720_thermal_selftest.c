// SPDX-License-Identifier: GPL-2.0-only
/* Opt-in integration harness for the unmodified Linux thermal core.
 * Reuses production callbacks but replaces sensors and the RPM transport.
 * Contains NO ACPI, WMI, PCI, EC or real fan-control calls. Never packaged.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/thermal.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include "y720-fan-policy.h"

static int test_temperature = 40000;
static int test_sensor_error;
static unsigned int test_flat_rpm;
module_param_named(temperature_millidegrees, test_temperature, int, 0600);
module_param_named(sensor_error, test_sensor_error, int, 0600);
module_param_named(flat_rpm, test_flat_rpm, uint, 0400);

struct y720_data {
	struct device *dev;
	struct mutex lock, thermal_lock;
	struct thermal_zone_device *thermal_zone;
	struct thermal_cooling_device *cooling;
	struct delayed_work guard_work;
	bool kernel_curve, blocked, thermal_enabled;
	int requested_mode, curve_error;
	long curve_temperature;
	unsigned long thermal_state, desired_since;
	unsigned int curve_points[6], target, pwm;
};

static int y720_curve_sample(struct y720_data *data, unsigned int *rpm)
{
	int temp = READ_ONCE(test_temperature);
	int ret = READ_ONCE(test_sensor_error);
	if (!ret && y720_cpu_health(temp) == Y720_TEMP_INVALID)
		ret = -ENODATA;
	data->curve_error = ret;
	if (ret)
		return ret;
	data->curve_temperature = temp;
	*rpm = y720_curve_rpm(data->curve_points, temp);
	return 0;
}

static int y720_target_write(struct y720_data *data, unsigned int rpm)
{
	/* Requested RPM only. Deliberately no hardware transport. */
	return rpm == data->target ? 0 : -EINVAL;
}

static void y720_fault(struct y720_data *data, int error)
{
	dev_err(data->dev, "Selftest callback failed: %d\n", error);
}

#include "y720-thermal.h"

static void noop_guard(struct work_struct *work) {}

static ssize_t activate_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct y720_data *data = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%u\n", READ_ONCE(data->kernel_curve));
}

static ssize_t activate_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct y720_data *data = dev_get_drvdata(dev);
	bool enabled;
	int ret = kstrtobool(buf, &enabled);
	if (ret)
		return ret;
	mutex_lock(&data->lock);
	data->kernel_curve = enabled;
	data->requested_mode = 1;
	mutex_unlock(&data->lock);
	ret = y720_thermal_sync(data);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(activate);

static ssize_t target_rpm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct y720_data *data = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%u\n", READ_ONCE(data->target));
}
static DEVICE_ATTR_RO(target_rpm);

static ssize_t curve_error_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct y720_data *data = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", READ_ONCE(data->curve_error));
}
static DEVICE_ATTR_RO(curve_error);

static struct attribute *selftest_attrs[] = {
	&dev_attr_activate.attr, &dev_attr_target_rpm.attr, &dev_attr_curve_error.attr, NULL
};
static const struct attribute_group selftest_group = {.attrs = selftest_attrs};

static int selftest_probe(struct platform_device *pdev)
{
	const unsigned int defaults[6] = {1500,1800,2400,3200,4200,5100};
	struct y720_data *data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	unsigned int i;
	int ret;
	if (!data)
		return -ENOMEM;
	data->dev = &pdev->dev;
	mutex_init(&data->lock);
	mutex_init(&data->thermal_lock);
	INIT_DELAYED_WORK(&data->guard_work, noop_guard);
	for (i = 0; i < 6; i++)
		data->curve_points[i] = test_flat_rpm ? test_flat_rpm : defaults[i];
	if (!y720_curve_valid(data->curve_points))
		return -EINVAL;
	data->target = data->curve_points[0];
	platform_set_drvdata(pdev, data);
	ret = y720_thermal_init(data);
	if (ret)
		return ret;
	if (!data->thermal_zone)
		return -ENODEV;
	return devm_device_add_group(&pdev->dev, &selftest_group);
}

static void selftest_remove(struct platform_device *pdev)
{
	struct y720_data *data = platform_get_drvdata(pdev);
	mutex_lock(&data->lock);
	data->blocked = true;
	data->kernel_curve = false;
	mutex_unlock(&data->lock);
	y720_thermal_sync(data);
	cancel_delayed_work_sync(&data->guard_work);
}

static struct platform_driver selftest_driver = {
	.probe = selftest_probe,
	.remove = selftest_remove,
	.driver.name = "y720-thermal-selftest",
};
static struct platform_device *selftest_device;

static int __init selftest_init(void)
{
	int ret = platform_driver_register(&selftest_driver);
	if (ret)
		return ret;
	selftest_device = platform_device_register_simple("y720-thermal-selftest", -1, NULL, 0);
	if (IS_ERR(selftest_device)) {
		ret = PTR_ERR(selftest_device);
		platform_driver_unregister(&selftest_driver);
		return ret;
	}
	return 0;
}
static void __exit selftest_exit(void)
{
	platform_device_unregister(selftest_device);
	platform_driver_unregister(&selftest_driver);
}
module_init(selftest_init);
module_exit(selftest_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Y720 thermal integration selftest: synthetic sensors, NO hardware writes");
