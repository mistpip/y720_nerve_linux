/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Y720_FAN_POLICY_H
#define Y720_FAN_POLICY_H

/* Conservative first-release limits, not the firmware's nominal 3400 RPM. */
#define Y720_MIN_RPM 1500
#define Y720_MAX_RPM 5100
#define Y720_STEP_RPM 100
#define Y720_CPU_LIMIT_MC 85000
#define Y720_GPU_LIMIT_MC 80000
#define Y720_TARGET_INTERVAL_SECONDS 15
#define Y720_RAMP_SECONDS 5

enum y720_temperature_state { Y720_TEMP_OK, Y720_TEMP_INVALID, Y720_TEMP_HOT };

/* Only actual, verified target changes open this finite observation window. */
static inline int y720_tach_transient(unsigned int rpm, unsigned int elapsed_seconds)
{
	return rpm < 1000 && elapsed_seconds < Y720_RAMP_SECONDS;
}

static inline enum y720_temperature_state y720_cpu_health(int temp_mc)
{
	if (temp_mc < 1000 || temp_mc > 120000)
		return Y720_TEMP_INVALID;
	return temp_mc >= Y720_CPU_LIMIT_MC ? Y720_TEMP_HOT : Y720_TEMP_OK;
}

static inline enum y720_temperature_state y720_gpu_health(int awake,
	unsigned int raw, unsigned int average, unsigned int errors, unsigned int failures)
{
	if (!awake)
		return Y720_TEMP_OK;
	if (!raw || raw > 120 || !average || average > 120 || (errors & 2) || failures)
		return Y720_TEMP_INVALID;
	return raw * 1000 >= Y720_GPU_LIMIT_MC || average * 1000 >= Y720_GPU_LIMIT_MC ?
		Y720_TEMP_HOT : Y720_TEMP_OK;
}

static inline int y720_target_due(unsigned int desired, unsigned int applied,
				unsigned int elapsed_seconds, unsigned int stable_seconds)
{
	unsigned int difference = desired > applied ? desired - applied : applied - desired;

	return difference && elapsed_seconds >= Y720_TARGET_INTERVAL_SECONDS &&
		(difference >= 200 || stable_seconds >= 10);
}

/* A normalized linked RPM selector, NOT an electrical PWM duty measurement.
 * Round upward, so quantization never commands less cooling than requested.
 * pwm=0 intentionally retains the minimum; fan stop is not supported.
 */
static inline unsigned int y720_pwm_to_rpm(unsigned int pwm)
{
	return Y720_MIN_RPM + ((pwm * ((Y720_MAX_RPM - Y720_MIN_RPM) /
		Y720_STEP_RPM) + 254) / 255) * Y720_STEP_RPM;
}

static inline unsigned int y720_rpm_to_pwm(unsigned int rpm)
{
	return ((rpm - Y720_MIN_RPM) * 255) / (Y720_MAX_RPM - Y720_MIN_RPM);
}

static inline unsigned int y720_round_rpm(unsigned int rpm)
{
	return ((rpm + Y720_STEP_RPM - 1) / Y720_STEP_RPM) * Y720_STEP_RPM;
}
#endif
