/* SPDX-License-Identifier: GPL-2.0-only */
/* Private implementation for the single verified 80Y4 / 4GCN38WW EC.
 * No raw ports, commands or register addresses are exposed to userspace.
 * All functions below run under data->lock, except init and hwmon callbacks.
 */
#define Y720_DATA_PORT 0x68
#define Y720_STATUS_PORT 0x6c

static int y720_wait(bool output)
{
	u8 status;

	return read_poll_timeout(inb, status,
		output ? (status & 1) : !(status & 2),
		200, 100000, false, Y720_STATUS_PORT);
}

static int y720_transaction(u8 cmd, const u8 *args, unsigned int nargs, u8 *reply)
{
	unsigned int i;
	int ret;

	/* Never drain an unsolicited/stale reply and pretend it was ours. */
	if (inb(Y720_STATUS_PORT) & 1)
		return -EBUSY;
	ret = y720_wait(false);
	if (ret)
		return ret;
	outb(cmd, Y720_STATUS_PORT);
	for (i = 0; i < nargs; i++) {
		ret = y720_wait(false);
		if (ret)
			return ret;
		outb(args[i], Y720_DATA_PORT);
	}
	ret = y720_wait(false);
	if (ret)
		return ret;
	if (reply) {
		ret = y720_wait(true);
		if (!ret)
			*reply = inb(Y720_DATA_PORT);
	} else {
		/* No ACK for these writes; verify fixed state fields separately. */
		usleep_range(1000, 2000);
	}
	return ret;
}

static int y720_reg(unsigned int address, u8 *value)
{
	u8 args[] = { address >> 8, address & 255 };

	/* LRAM(bank, offset): high byte first; command handler buffers in reverse. */
	switch (address) {
	case 0x21e: /* software-control flags */
	case 0x22e: /* linked target fan 1 */
	case 0xc20: /* linked target fan 2 */
	case 0xc15: /* boost flag */
	case 0x406: /* fan 1 tach, cross-check against command 46/81 */
	case 0x4fe: /* fan 2 tach, cross-check against command 46/82 */
		return y720_transaction(0x7e, args, 2, value);
	default:
		return -EINVAL;
	}
}

static int y720_tach(int channel, long *rpm)
{
	u8 arg = channel ? 0x82 : 0x81, value;
	int ret = y720_transaction(0x46, &arg, 1, &value);

	if (!ret) {
		if (value > 70)
			return -ERANGE;
		*rpm = value * 100;
	}
	return ret;
}

static int y720_mode_read(int *mode)
{
	u8 manual, boost;
	int ret;

	ret = y720_reg(0x21e, &manual);
	if (!ret)
		ret = y720_reg(0xc15, &boost);
	if (ret)
		return ret;
	if ((manual & 0xc0) && (manual & 0xc0) != 0xc0)
		return -EPROTO;
	if ((manual & 0xc0) && (boost & 0x10))
		return -EPROTO;
	*mode = (manual & 0xc0) ? 1 : (boost & 0x10) ? 0 : 2;
	return 0;
}

static int y720_mode_command(u8 arg)
{
	return y720_transaction(0x59, &arg, 1, NULL);
}

static int y720_native_mode(struct y720_data *data, int mode)
{
	u8 arg = mode == 0 ? 0x77 : 0x76;
	int actual, ret, attempt;

	if (mode != 0 && mode != 2)
		return -EINVAL;
	/* Even a timed-out command may have reached the EC. */
	data->needs_restore = true;
	ret = -EIO;
	for (attempt = 0; attempt < 3; attempt++) {
		data->mode_writes++;
		ret = y720_mode_command(arg);
		if (!ret)
			ret = y720_mode_read(&actual);
		if (!ret && actual != mode)
			ret = -EIO;
		if (!ret)
			break;
		usleep_range(2000, 4000);
	}
	data->last_command = ret ? -1 : mode == 0;
	if (ret) {
		data->mode = -1;
		dev_err_ratelimited(&data->wdev->dev, "EC mode %d failed including readback: %d\n", mode, ret);
		return ret;
	}
	data->mode = mode;
	data->deadline = 0;
	data->needs_restore = mode != 2;
	if (mode != 2)
		data->settled_at = jiffies + 20 * HZ;
	return 0;
}

static int y720_control_temperature(long *value)
{
	struct thermal_zone_device *zone;
	int temp, ret;

	/* Read a real kernel sensor, not the GameZone temperature stubs. */
	zone = thermal_zone_get_zone_by_name("x86_pkg_temp");
	if (IS_ERR(zone))
		return PTR_ERR(zone);
	ret = thermal_zone_get_temp(zone, &temp);
	if (ret)
		return ret;
	if (y720_cpu_health(temp) == Y720_TEMP_INVALID)
		return -ENODATA;
	*value = temp;
	return 0;
}

static int y720_health(struct y720_data *data)
{
	long temperature;
	int ret = y720_control_temperature(&temperature);

	if (ret)
		return ret;
	if (temperature >= Y720_CPU_LIMIT_MC) {
		dev_err_ratelimited(&data->wdev->dev, "Control temperature warning: %ld mC; curve policy unchanged\n", temperature);
		return -EOVERFLOW;
	}
	return 0;
}

/* A missing required sensor freezes the last target, never changes ownership
 * or silently substitutes a factory mode. Called only under data->lock. */
static int y720_curve_sample(struct y720_data *data, unsigned int *target)
{
	long temperature;
	int ret = y720_control_temperature(&temperature);
	data->curve_error = ret;
	if (ret)
		return ret;
	data->curve_temperature = temperature;
	*target = y720_curve_rpm(data->curve_points, temperature);
	return 0;
}

static void y720_fault(struct y720_data *data, int reason)
{
	data->last_error = reason;
	/* Diagnostic only in curve mode: no hidden target or policy override. */
	if (data->native && data->requested_mode == 1) {
		data->faulted = true;
		data->deadline = 0;
		cancel_delayed_work(&data->restore_work);
		dev_err_ratelimited(&data->wdev->dev,
			"Curve fault %d: retaining requested curve; no auto/boost or forced-RPM override\n", reason);
		queue_delayed_work(system_wq, &data->guard_work, HZ);
		return;
	}
	data->faulted = true;
	dev_err_ratelimited(&data->wdev->dev,
		"Safety fault %d; requesting firmware auto (reset via pwm1_enable=2)\n", reason);
	data->needs_restore = true;
	if (y720_restore_locked(data)) {
		data->deadline = jiffies;
		data->retries = 0;
		mod_delayed_work(system_wq, &data->restore_work, HZ);
	}
}

static int y720_target_write(struct y720_data *data, unsigned int rpm)
{
	u8 arg = rpm / 100, t1, t2;
	int ret, actual;

	/* Do not reinitialize the EC's closed-loop PWM for an identical target. */
	ret = y720_mode_read(&actual);
	if (ret)
		return ret;
	if (actual == 1 && data->mode == 1 && data->applied_target &&
	    !y720_target_due(rpm, data->applied_target,
		(jiffies - data->target_changed) / HZ,
		rpm == data->target ? (jiffies - data->desired_since) / HZ : 0)) {
		/* Readback detects external writers without reinitializing the fans. */
		arg = data->applied_target / 100;
		ret = y720_reg(0x22e, &t1);
		if (!ret)
			ret = y720_reg(0xc20, &t2);
		if (ret)
			return ret;
		if (t1 == arg && t2 == arg) {
			data->curve_entry_pending = false;
			return 0;
		}
		data->applied_target = 0;
		return -EIO;
	}
	/* Command 46 sets software flags; clear boost first when leaving full speed.
	 * This is an intentional transition into software mode, not a fallback.
	 */
	if (actual == 0) {
		/* Only an explicit default -> curve selection may clear boost. */
		if (!data->curve_entry_pending)
			return -EPROTO;
		ret = y720_native_mode(data, 2);
		if (ret)
			return ret;
	}
	data->needs_restore = true;
	/* A write can reach the EC even when subsequent verification fails. */
	data->applied_target = 0;
	ret = y720_transaction(0x46, &arg, 1, NULL);
	if (!ret)
		ret = y720_mode_read(&actual);
	if (!ret)
		ret = y720_reg(0x22e, &t1);
	if (!ret)
		ret = y720_reg(0xc20, &t2);
	if (!ret && (actual != 1 || t1 != arg || t2 != arg)) {
		dev_err(&data->wdev->dev, "Target readback mismatch: mode=%d fan1=%u fan2=%u wanted=%u\n",
			actual, t1, t2, arg);
		ret = -EIO;
	}
	if (ret)
		return ret;
	/* Changing targets does not reset the tach fault grace interval. */
	if (data->mode != 1)
		data->settled_at = jiffies + 20 * HZ;
	if (data->mode != 1) {
		data->bad_rpm_samples[0] = 0;
		data->bad_rpm_samples[1] = 0;
	}
	data->mode = 1;
	data->curve_entry_pending = false;
	data->applied_target = rpm;
	data->target_changed = jiffies;
	data->target_writes++;
	data->deadline = 0;
	return 0;
}

static void y720_curve_resume(struct y720_data *data)
{
	int ret = y720_target_write(data, data->target);

	if (ret)
		y720_fault(data, ret);
	queue_delayed_work(system_wq, &data->guard_work, HZ);
}

static void y720_guard_work(struct work_struct *work)
{
	struct y720_data *data = container_of(to_delayed_work(work), struct y720_data, guard_work);
	long rpm;
	int ret = 0, health = 0, mode, channel;

	mutex_lock(&data->lock);
	if (data->blocked)
		goto out;
	if (!data->needs_restore && data->requested_mode != 1)
		goto out;
	ret = y720_mode_read(&mode);
	if (!ret && data->requested_mode == 1) {
		/* Also recover a failed initial target write, without a policy fallback. */
		ret = y720_target_write(data, data->target);
		if (!ret && !health)
			health = y720_health(data);
	} else if (!ret && mode != data->mode) {
		ret = -EIO;
	}
	for (channel = 0; !ret && channel < 2; channel++) {
		ret = y720_tach(channel, &rpm);
		if (!ret && data->mode == 1 &&
		    y720_tach_transient(rpm, (jiffies - data->target_changed) / HZ)) {
			data->ramp_low_samples++;
			continue;
		}
		/* Allow closed-loop startup but never tolerate a stalled settled fan. */
		if (!ret && time_after_eq(jiffies, data->settled_at) && rpm < 1000)
			ret = -ENXIO;
		if (!ret && data->mode == 1 && time_after_eq(jiffies, data->settled_at)) {
			if (abs(rpm - data->applied_target) > 1200)
				data->bad_rpm_samples[channel]++;
			else
				data->bad_rpm_samples[channel] = 0;
			if (data->bad_rpm_samples[channel] >= 20)
				ret = -EREMOTEIO;
		}
	}
	if (ret || health)
		y720_fault(data, ret ? ret : health);
	else {
		data->faulted = false;
		data->last_error = 0;
		mod_delayed_work(system_wq, &data->guard_work, HZ);
	}
out:
	mutex_unlock(&data->lock);
}

static int y720_native_hwmon_read(struct y720_data *data, enum hwmon_sensor_types type,
				u32 attr, int channel, long *val)
{
	int ret = 0;

	mutex_lock(&data->lock);
	if (data->blocked) {
		ret = -EBUSY;
		goto out;
	}
	if (type == hwmon_temp && attr == hwmon_temp_input && channel == 0) {
		ret = y720_control_temperature(val);
	} else if (type == hwmon_fan && attr == hwmon_fan_input && channel < 2) {
		ret = y720_tach(channel, val);
		if (ret && data->needs_restore)
			y720_fault(data, ret);
	} else if (!channel && type == hwmon_fan && attr == hwmon_fan_target) {
		*val = data->target;
	} else if (!channel && type == hwmon_pwm && attr == hwmon_pwm_input) {
		*val = data->pwm;
	} else if (!channel && type == hwmon_pwm && attr == hwmon_pwm_enable) {
		int actual;

		ret = y720_mode_read(&actual);
		if (!ret)
			*val = actual == 1 && data->kernel_curve && data->thermal_enabled ? 3 : actual;
		else if (data->needs_restore)
			y720_fault(data, ret);
	} else {
		ret = -EOPNOTSUPP;
	}
out:
	mutex_unlock(&data->lock);
	return ret;
}

static int y720_native_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
				 u32 attr, int channel, long val)
{
	struct y720_data *data = dev_get_drvdata(dev);
	unsigned int rpm, pwm;
	bool sync = false;
	int ret = 0;

	if (!data->native || !allow_control)
		return -EOPNOTSUPP;
	if (channel || val < 0)
		return -EINVAL;
	mutex_lock(&data->lock);
	if (data->blocked) {
		ret = -EBUSY;
		goto out;
	}
	if (type == hwmon_pwm && attr == hwmon_pwm_enable) {
		if (val > 3) {
			ret = -EINVAL;
			goto out;
		}
		if (val == 2 || val == 0) {
			ret = y720_native_mode(data, val);
			if (!ret) {
				data->kernel_curve = false;
				sync = true;
				/* Explicit user selection, unlike an automatic fault fallback. */
				data->requested_mode = val;
				data->curve_entry_pending = false;
				data->faulted = false;
				data->last_error = 0;
			}
			if (!ret && val == 0) {
				data->target = Y720_MAX_RPM;
				data->desired_since = jiffies;
				data->pwm = 255;
			}
			goto handled;
		}
		rpm = data->target;
		if (val == 3) {
			if (!data->thermal_zone) {
				ret = -EOPNOTSUPP;
				goto out;
			}
			ret = y720_curve_sample(data, &rpm);
			if (ret)
				goto out;
		}
		data->kernel_curve = val == 3;
		sync = true;
		if (val == 3)
			data->thermal_state = y720_thermal_state(data->curve_temperature);
		pwm = val == 3 ? y720_rpm_to_pwm(rpm) : data->pwm;
		data->requested_mode = 1;
		data->curve_entry_pending = true;
	} else if (type == hwmon_pwm && attr == hwmon_pwm_input) {
		if (val > 255) {
			ret = -EINVAL;
			goto out;
		}
		pwm = val;
		rpm = y720_pwm_to_rpm(pwm);
		data->kernel_curve = false;
		sync = true;
	} else if (type == hwmon_fan && attr == hwmon_fan_target) {
		if (val < Y720_MIN_RPM || val > Y720_MAX_RPM) {
			ret = -EINVAL;
			goto out;
		}
		rpm = y720_round_rpm(val);
		pwm = y720_rpm_to_pwm(rpm);
		data->kernel_curve = false;
		sync = true;
	} else {
		ret = -EOPNOTSUPP;
		goto out;
	}
	/* Values can be staged in auto/full speed without taking control. */
	/* Preserve the latest request even if transport fails; never invent a target. */
	if (data->target != rpm)
		data->desired_since = jiffies;
	data->target = rpm;
	data->pwm = pwm;
	if (data->requested_mode == 1)
		ret = y720_target_write(data, rpm);
handled:
	if (ret)
		y720_fault(data, ret);
	else if (data->needs_restore)
		queue_delayed_work(system_wq, &data->guard_work, HZ);
out:
	mutex_unlock(&data->lock);
	if (sync) {
		int sync_ret = y720_thermal_sync(data);
		if (!ret)
			ret = sync_ret;
	}
	return ret;
}

static int y720_native_init(struct y720_data *data)
{
	long rpm;
	u8 raw;
	int ret;

	if (!dmi_match(DMI_SYS_VENDOR, "LENOVO") ||
	    !dmi_match(DMI_PRODUCT_NAME, "80Y4") ||
	    !dmi_match(DMI_PRODUCT_VERSION, "Y720-15IKB") ||
	    !dmi_match(DMI_BIOS_VERSION, "4GCN38WW"))
		return -ENODEV;
	if (!devm_request_region(&data->wdev->dev, Y720_DATA_PORT, 1, "y720_cooling") ||
	    !devm_request_region(&data->wdev->dev, Y720_STATUS_PORT, 1, "y720_cooling"))
		return -EBUSY;
	ret = y720_mode_read(&data->mode);
	if (!ret)
		ret = y720_tach(0, &rpm);
	if (!ret)
		ret = y720_reg(0x406, &raw);
	if (!ret && abs(rpm - raw * 100) > 200)
		ret = -EPROTO;
	if (!ret)
		ret = y720_tach(1, &rpm);
	if (!ret)
		ret = y720_reg(0x4fe, &raw);
	if (!ret && abs(rpm - raw * 100) > 200)
		ret = -EPROTO;
	if (ret)
		return dev_err_probe(&data->wdev->dev, ret, "Native read-only transport check failed\n");
	/* Do not silently adopt a pre-existing software controller. */
	if (data->mode == 1)
		return -EBUSY;
	data->target = Y720_MIN_RPM;
	data->requested_mode = data->mode;
	data->desired_since = jiffies;
	data->pwm = 0;
	data->native = true;
	return 0;
}
