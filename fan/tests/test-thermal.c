/* SPDX-License-Identifier: GPL-2.0-only */
/* Callback/locking unit harness; real thermal governor integration is tested
 * separately on the Y720. No kernel, ports, sysfs or hardware writes here. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "../kernel/y720-fan-policy.h"

struct mutex { bool locked; };
static void mutex_lock(struct mutex *m) { assert(!m->locked); m->locked = true; }
static void mutex_unlock(struct mutex *m) { assert(m->locked); m->locked = false; }
enum thermal_device_mode { THERMAL_DEVICE_DISABLED, THERMAL_DEVICE_ENABLED };
struct thermal_zone_device;
struct thermal_cooling_device;
struct thermal_trip { int temperature, hysteresis, type; };
struct cooling_spec { unsigned long upper, lower; unsigned int weight; };
struct thermal_zone_device_ops {
    int (*get_temp)(struct thermal_zone_device *, int *);
    bool (*should_bind)(struct thermal_zone_device *, const struct thermal_trip *, struct thermal_cooling_device *, struct cooling_spec *);
    int (*change_mode)(struct thermal_zone_device *, enum thermal_device_mode);
};
struct thermal_cooling_device_ops {
    int (*get_max_state)(struct thermal_cooling_device *, unsigned long *);
    int (*get_cur_state)(struct thermal_cooling_device *, unsigned long *);
    int (*set_cur_state)(struct thermal_cooling_device *, unsigned long);
};
struct thermal_zone_device { void *devdata; const struct thermal_zone_device_ops *ops; };
struct thermal_cooling_device { void *devdata; const struct thermal_cooling_device_ops *ops; };
struct thermal_zone_params { const char *governor_name; bool no_hwmon; };
struct y720_data {
    void *dev;
    struct mutex lock, thermal_lock;
    bool kernel_curve, blocked, thermal_enabled;
    int requested_mode, curve_error, last_error;
    long curve_temperature;
    unsigned long thermal_state, desired_since;
    unsigned int curve_points[6], target, pwm;
    struct thermal_zone_device *thermal_zone;
    struct thermal_cooling_device *cooling;
    int guard_work;
};
static unsigned int writes, samples, updates;
static int sensor_error, transport_error;
static unsigned long jiffies;
#define HZ 1000
#define system_wq NULL
#define GFP_KERNEL 0
#define THERMAL_WEIGHT_DEFAULT 0
#define THERMAL_TRIP_ACTIVE 1
#define THERMAL_EVENT_UNSPECIFIED 0
#define CONFIG_THERMAL_GOV_STEP_WISE 1
#define IS_ENABLED(v) (v)
#define IS_ERR(p) (!(p))
#define PTR_ERR(p) (-ENOMEM)
#define dev_warn_ratelimited(...) do {} while (0)
#define dev_warn(...) do {} while (0)
#define dev_err(...) do {} while (0)
#define kcalloc(n, size, flags) calloc(n, size)
#define kfree free
static void *thermal_zone_device_priv(struct thermal_zone_device *z) { return z->devdata; }
static int y720_curve_sample(struct y720_data *data, unsigned int *target)
{
    assert(data->lock.locked);
    samples++;
    data->curve_error = sensor_error;
    if (sensor_error) return sensor_error;
    data->curve_temperature = 55000;
    *target = y720_curve_rpm(data->curve_points, 55000);
    return 0;
}
static int y720_target_write(struct y720_data *data, unsigned int rpm)
{
    assert(data->lock.locked && rpm == data->target);
    writes++;
    return transport_error;
}
static void y720_fault(struct y720_data *data, int error) { data->last_error = error; }
static void mod_delayed_work(void *queue, int *work, int delay) { (void)queue; (void)work; (void)delay; }
static int thermal_zone_device_enable(struct thermal_zone_device *z)
{
    return z->ops->change_mode(z, THERMAL_DEVICE_ENABLED);
}
static int thermal_zone_device_disable(struct thermal_zone_device *z)
{
    return z->ops->change_mode(z, THERMAL_DEVICE_DISABLED);
}
static void thermal_zone_device_update(struct thermal_zone_device *z, int event)
{
    int temp;
    (void)event;
    updates++;
    z->ops->get_temp(z, &temp);
}
static struct thermal_cooling_device *thermal_cooling_device_register(const char *name, void *data,
    const struct thermal_cooling_device_ops *ops)
{
    (void)name;
    struct thermal_cooling_device *c = calloc(1, sizeof(*c));
    assert(c); c->devdata = data; c->ops = ops; return c;
}
static struct thermal_zone_device *thermal_zone_device_register_with_trips(const char *name,
    struct thermal_trip *trips, int count, void *priv, const struct thermal_zone_device_ops *ops,
    const struct thermal_zone_params *params, int passive, int polling)
{
    (void)name;
    assert(count == 50 && passive == 1000 && polling == 1000);
    assert(!__builtin_strcmp(params->governor_name, "step_wise") && params->no_hwmon);
    struct thermal_zone_device *z = calloc(1, sizeof(*z));
    assert(z); z->devdata = priv; z->ops = ops;
    struct y720_data *data = priv;
    for (int i = 0; i < count; i++) {
        struct cooling_spec s = {0};
        assert(trips[i].temperature == 41000 + i * 1000 && trips[i].hysteresis == 2000);
        assert(ops->should_bind(z, &trips[i], data->cooling, &s));
        assert(s.lower == (unsigned int)i + 1 && s.upper == s.lower);
    }
    return z;
}
static void thermal_zone_device_unregister(struct thermal_zone_device *z) { free(z); }
static void thermal_cooling_device_unregister(struct thermal_cooling_device *c) { free(c); }
static int devm_add_action_or_reset(void *dev, void (*cleanup)(void *), void *arg)
{
    (void)dev; (void)cleanup; (void)arg; return 0;
}
/* The mocked logger and devm calls do not dereference wdev. */
#define devm_add_action_or_reset(dev, cleanup, arg) devm_add_action_or_reset(NULL, cleanup, arg)
#include "../kernel/y720-thermal.h"

int main(void)
{
    struct y720_data data = {.requested_mode=2, .target=1800,
        .curve_points={1500,1800,2400,3200,4200,5100}};
    assert(!y720_thermal_init(&data));
    assert(!data.kernel_curve && !data.thermal_enabled && !writes && !samples);
    assert(thermal_zone_device_enable(data.thermal_zone) == -EBUSY);
    data.kernel_curve = true; data.requested_mode = 1;
    assert(!y720_thermal_sync(&data) && data.thermal_enabled);
    int temperature = 0;
    assert(!y720_thermal_get_temp(data.thermal_zone, &temperature) && temperature == 55000);
    unsigned long state;
    assert(!y720_cooling_max(data.cooling, &state) && state == 50);
    for (state = 0; state <= 50; state++) {
        assert(!y720_cooling_set(data.cooling, state));
        assert(data.target == y720_curve_rpm(data.curve_points, 40000 + state * 1000));
    }
    unsigned int before = writes, target = data.target;
    assert(y720_cooling_set(data.cooling, 51) == -EINVAL && writes == before);
    sensor_error = -ENODATA;
    assert(y720_thermal_get_temp(data.thermal_zone, &temperature) == -EAGAIN);
    assert(data.curve_error == -ENODATA && data.kernel_curve && data.requested_mode == 1);
    assert(y720_cooling_set(data.cooling, 0) == -EAGAIN && writes == before && data.target == target);
    sensor_error = 0;
    assert(!y720_thermal_get_temp(data.thermal_zone, &temperature));
    transport_error = -EIO;
    assert(y720_cooling_set(data.cooling, 10) == -EIO && data.target == 1800);
    assert(data.kernel_curve && data.requested_mode == 1 && data.last_error == -EIO);
    transport_error = 0;
    assert(!y720_cooling_set(data.cooling, 10));
    data.blocked = true;
    assert(!y720_thermal_sync(&data) && !data.thermal_enabled && data.kernel_curve);
    before = writes;
    assert(y720_cooling_set(data.cooling, 50) == -EBUSY && writes == before);
    data.blocked = false;
    assert(!y720_thermal_sync(&data) && data.thermal_enabled);
    /* An external hwmon writer releases ownership before any late callback. */
    data.kernel_curve = false;
    assert(y720_cooling_set(data.cooling, 50) == -EBUSY && writes == before);
    assert(!y720_thermal_sync(&data) && !data.thermal_enabled);
    assert(data.requested_mode == 1 && data.target == 1800);
    y720_thermal_cleanup(&data);
    assert(data.blocked && !data.thermal_zone && !data.cooling);
    puts("PASS thermal callbacks: bindings, ownership, sensor/transport faults, suspend, cleanup and lock order");
    return 0;
}
