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
	assert(y720_gpu_health(0, 0, 0, 255, 255) == Y720_TEMP_OK);
	assert(y720_gpu_health(1, 0, 0, 0, 0) == Y720_TEMP_INVALID);
	assert(y720_gpu_health(1, 40, 40, 2, 0) == Y720_TEMP_INVALID);
	assert(y720_gpu_health(1, 40, 40, 0, 1) == Y720_TEMP_INVALID);
	assert(y720_gpu_health(1, 255, 40, 0, 0) == Y720_TEMP_INVALID);
	assert(y720_gpu_health(1, 79, 79, 0, 0) == Y720_TEMP_OK);
	assert(y720_gpu_health(1, 80, 79, 0, 0) == Y720_TEMP_HOT);
	assert(y720_gpu_health(1, 79, 80, 0, 0) == Y720_TEMP_HOT);
	assert(y720_tach_transient(0, 0));
	assert(y720_tach_transient(999, 4));
	assert(!y720_tach_transient(1000, 0));
	assert(!y720_tach_transient(0, 5));
	assert(!y720_tach_transient(0, 100));
	assert(Y720_RAMP_SECONDS < Y720_TARGET_INTERVAL_SECONDS);
	puts("PASS: all 256 PWM values, RPM rounding and endpoint round trips");
	return 0;
}
