/* SPDX-License-Identifier: GPL-2.0-only */
/* Linux thermal owns sampling, trip hysteresis and cooling-state selection.
 * The existing guard only verifies/retries the latest requested RPM.
 */
static int y720_thermal_get_temp(struct thermal_zone_device *zone, int *temp)
{
	struct y720_data *data = thermal_zone_device_priv(zone);
	unsigned int unused;
	int ret;

	mutex_lock(&data->lock);
	if (!data->kernel_curve || data->blocked || data->requested_mode != 1) {
		mutex_unlock(&data->lock);
		return -EAGAIN;
	}
	ret = y720_curve_sample(data, &unused);
	if (!ret)
		*temp = data->curve_temperature;
	else
		dev_warn_ratelimited(data->dev,
			"Curve sensor unavailable (%d); retaining target %u RPM\n", ret, data->target);
	mutex_unlock(&data->lock);
	/* thermal treats EAGAIN as retryable and does not permanently disable
	 * this zone after missing samples. The real errno remains in curve_error.
	 */
	return ret ? -EAGAIN : 0;
}

static int y720_thermal_change_mode(struct thermal_zone_device *zone,
				   enum thermal_device_mode mode)
{
	struct y720_data *data = thermal_zone_device_priv(zone);
	int ret = 0;

	mutex_lock(&data->lock);
	if (mode == THERMAL_DEVICE_ENABLED &&
	    (!data->kernel_curve || data->blocked || data->requested_mode != 1)) {
		ret = -EBUSY;
	} else {
		data->thermal_enabled = mode == THERMAL_DEVICE_ENABLED;
		/* A deliberate sysfs disable releases only kernel curve ownership.
		 * Suspend preserves it, with blocked set before disabling the zone. */
		if (mode == THERMAL_DEVICE_DISABLED && !data->blocked)
			data->kernel_curve = false;
	}
	mutex_unlock(&data->lock);
	return ret;
}

static bool y720_thermal_bind(struct thermal_zone_device *zone,
		const struct thermal_trip *trip, struct thermal_cooling_device *cooling,
		struct cooling_spec *spec)
{
	struct y720_data *data = thermal_zone_device_priv(zone);
	unsigned long state = (trip->temperature - 40000) / 1000;

	if (cooling != data->cooling || !state || state > Y720_THERMAL_STATES)
		return false;
	/* Each 1 C threshold has a fixed minimum/maximum cooling state.
	 * step_wise selects the highest reached trip, with 2 C hysteresis. */
	spec->lower = state;
	spec->upper = state;
	spec->weight = THERMAL_WEIGHT_DEFAULT;
	return true;
}

static int y720_cooling_max(struct thermal_cooling_device *cooling, unsigned long *state)
{
	*state = Y720_THERMAL_STATES;
	return 0;
}

static int y720_cooling_get(struct thermal_cooling_device *cooling, unsigned long *state)
{
	struct y720_data *data = cooling->devdata;
	mutex_lock(&data->lock);
	*state = data->thermal_state;
	mutex_unlock(&data->lock);
	return 0;
}

static int y720_cooling_set(struct thermal_cooling_device *cooling, unsigned long state)
{
	struct y720_data *data = cooling->devdata;
	unsigned int rpm;
	int ret = 0;

	if (state > Y720_THERMAL_STATES)
		return -EINVAL;
	mutex_lock(&data->lock);
	if (!data->kernel_curve || data->blocked || data->requested_mode != 1) {
		ret = -EBUSY;
		goto unlock;
	}
	if (data->curve_error) {
		ret = -EAGAIN;
		goto unlock;
	}
	rpm = y720_curve_rpm(data->curve_points, 40000 + state * 1000);
	data->thermal_state = state;
	if (rpm != data->target)
		data->desired_since = jiffies;
	data->target = rpm;
	data->pwm = y720_rpm_to_pwm(rpm);
	ret = y720_target_write(data, rpm);
	if (ret)
		y720_fault(data, ret);
	mod_delayed_work(system_wq, &data->guard_work, HZ);
unlock:
	mutex_unlock(&data->lock);
	return ret;
}

static const struct thermal_cooling_device_ops y720_cooling_ops = {
	.get_max_state = y720_cooling_max,
	.get_cur_state = y720_cooling_get,
	.set_cur_state = y720_cooling_set,
};

static const struct thermal_zone_device_ops y720_thermal_ops = {
	.get_temp = y720_thermal_get_temp,
	.should_bind = y720_thermal_bind,
	.change_mode = y720_thermal_change_mode,
};

/* Called with neither data->lock nor any thermal core lock held. Concurrent
 * manual writers invalidate kernel_curve before waiting here, so a queued
 * thermal callback cannot overwrite the new owner's RPM request. */
static int y720_thermal_sync(struct y720_data *data)
{
	bool active, enabled;
	int ret = 0;

	mutex_lock(&data->thermal_lock);
	if (!data->thermal_zone)
		goto unlock;
	mutex_lock(&data->lock);
	active = data->kernel_curve && !data->blocked && data->requested_mode == 1;
	enabled = data->thermal_enabled;
	mutex_unlock(&data->lock);
	if (active && !enabled)
		ret = thermal_zone_device_enable(data->thermal_zone);
	else if (!active && enabled)
		ret = thermal_zone_device_disable(data->thermal_zone);
	else if (active)
		thermal_zone_device_update(data->thermal_zone, THERMAL_EVENT_UNSPECIFIED);
	if (ret) {
		mutex_lock(&data->lock);
		data->curve_error = ret;
		mutex_unlock(&data->lock);
		dev_err(data->dev, "Unable to change thermal curve state: %d\n", ret);
	}
unlock:
	mutex_unlock(&data->thermal_lock);
	return ret;
}

static void y720_thermal_cleanup(void *arg)
{
	struct y720_data *data = arg;

	mutex_lock(&data->thermal_lock);
	mutex_lock(&data->lock);
	data->blocked = true;
	data->kernel_curve = false;
	mutex_unlock(&data->lock);
	thermal_zone_device_unregister(data->thermal_zone);
	thermal_cooling_device_unregister(data->cooling);
	data->thermal_zone = NULL;
	data->cooling = NULL;
	mutex_unlock(&data->thermal_lock);
}

static int y720_thermal_init(struct y720_data *data)
{
	static const struct thermal_zone_params params = {
		.governor_name = "step_wise",
		.no_hwmon = true, /* real sensor channels already belong to y720_cooling */
	};
	struct thermal_trip *trips;
	unsigned int i;
	int ret;

	if (!IS_ENABLED(CONFIG_THERMAL_GOV_STEP_WISE)) {
		dev_warn(data->dev, "Kernel curve unavailable: CONFIG_THERMAL_GOV_STEP_WISE required\n");
		return 0;
	}
	trips = kcalloc(Y720_THERMAL_STATES, sizeof(*trips), GFP_KERNEL);
	if (!trips)
		return -ENOMEM;
	data->cooling = thermal_cooling_device_register("y720-linked-fans", data, &y720_cooling_ops);
	if (IS_ERR(data->cooling)) {
		ret = PTR_ERR(data->cooling);
		data->cooling = NULL;
		kfree(trips);
		return ret;
	}
	for (i = 0; i < Y720_THERMAL_STATES; i++) {
		trips[i].temperature = 41000 + i * 1000;
		trips[i].hysteresis = Y720_THERMAL_HYST_MC;
		trips[i].type = THERMAL_TRIP_ACTIVE;
	}
	data->thermal_zone = thermal_zone_device_register_with_trips("y720-curve",
		trips, Y720_THERMAL_STATES, data, &y720_thermal_ops, &params, 1000, 1000);
	kfree(trips); /* thermal core owns a copy */
	if (IS_ERR(data->thermal_zone)) {
		ret = PTR_ERR(data->thermal_zone);
		data->thermal_zone = NULL;
		thermal_cooling_device_unregister(data->cooling);
		data->cooling = NULL;
		return ret;
	}
	/* Registration starts disabled. No temperature polling or fan takeover
	 * until the user explicitly commits a valid curve. */
	return devm_add_action_or_reset(data->dev, y720_thermal_cleanup, data);
}
