/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "../kernel/y720-fan-policy.h"

int main(void)
{

	unsigned int pwm, rpm, previous = 0;
	for (pwm = 0; pwm <= 255; pwm++) {
		rpm = y720_pwm_to_rpm(pwm);
		assert(rpm >= Y720_MIN_RPM && rpm <= Y720_MAX_RPM);
		assert(rpm % Y720_STEP_RPM == 0);
		assert(rpm >= previous);
		assert(y720_pwm_to_rpm(y720_rpm_to_pwm(rpm)) == rpm);
		previous = rpm;
	}
	for (rpm = Y720_MIN_RPM; rpm <= Y720_MAX_RPM; rpm++) {
		unsigned int rounded = y720_round_rpm(rpm);
		assert(rounded >= rpm && rounded - rpm < Y720_STEP_RPM);
		assert(rounded <= Y720_MAX_RPM);
	}
	assert(y720_pwm_to_rpm(0) == Y720_MIN_RPM);
	assert(y720_pwm_to_rpm(255) == Y720_MAX_RPM);
	assert(!y720_target_due(3000, 3000, 1000, 1000));
	assert(y720_pwm_to_rpm(0) == 1500);
	assert(!y720_target_due(4000, 3000, 14, 20));
	assert(y720_target_due(4000, 3000, 15, 0));
	assert(y720_target_due(3000, 4000, 15, 0));
	assert(!y720_target_due(3200, 3300, 100, 9));
	assert(y720_target_due(3200, 3300, 100, 10));
	assert(y720_target_due(3300, 3200, 100, 10));
	assert(y720_cpu_health(0) == Y720_TEMP_INVALID);
	assert(y720_cpu_health(-1000) == Y720_TEMP_INVALID);
	assert(y720_cpu_health(121000) == Y720_TEMP_INVALID);
	assert(y720_cpu_health(84999) == Y720_TEMP_OK);
	assert(y720_cpu_health(85000) == Y720_TEMP_HOT);

	assert(y720_tach_transient(0, 0));
	assert(y720_tach_transient(999, 4));
	assert(!y720_tach_transient(1000, 0));
	assert(!y720_tach_transient(0, 5));
	assert(!y720_tach_transient(0, 100));
	assert(Y720_RAMP_SECONDS < Y720_TARGET_INTERVAL_SECONDS);
	unsigned int points[Y720_CURVE_POINTS] = {1500, 1800, 2400, 3200, 4200, 5100};
	assert(y720_curve_valid(points));
	for (int temp = -1000; temp <= 121000; temp++) {
		unsigned int state = y720_thermal_state(temp);
		assert(state <= Y720_THERMAL_STATES);
		rpm = y720_curve_rpm(points, temp);
		assert(rpm >= 1500 && rpm <= 5100 && !(rpm % 100));
		if (temp > -1000) assert(rpm >= previous);
		previous = rpm;
	}
	assert(y720_curve_rpm(points, 55000) == 2100);
	assert(y720_curve_rpm(points, 70000) == 3200);
	points[0] = 5100;
	assert(!y720_curve_valid(points)); /* More cooling state may never mean less RPM. */
	for (unsigned int i = 0; i < Y720_CURVE_POINTS; i++) points[i] = 5100;
	assert(y720_curve_valid(points));
	assert(y720_curve_rpm(points, 10000) == 5100); /* Full speed is not temperature-gated. */
	for (unsigned int i = 0; i < Y720_CURVE_POINTS; i++) points[i] = 1500;
	assert(y720_curve_valid(points) && y720_curve_rpm(points, 120000) == 1500);
	points[2] = 1501;
	assert(!y720_curve_valid(points));
	puts("PASS: all 256 PWM values, RPM rounding and endpoint round trips");
	return 0;
}
