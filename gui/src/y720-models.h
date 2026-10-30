/* SPDX-License-Identifier: MIT */
#ifndef Y720_MODELS_H
#define Y720_MODELS_H

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define Y720_FAN_DIAGNOSTIC_COUNT 6

extern const gchar *const y720_fan_diagnostic_names[Y720_FAN_DIAGNOSTIC_COUNT];

typedef struct {
    gboolean controls_available;
    gchar *device;
    gchar *driver_version;
    gchar *mode;
    gint64 mode_raw;
    gint64 target_rpm;
    gint64 pwm;
    gint64 fan_rpm[2];
    gboolean diagnostic_present[Y720_FAN_DIAGNOSTIC_COUNT];
    gint64 diagnostic[Y720_FAN_DIAGNOSTIC_COUNT];
    gboolean temperature_valid;
    gint64 temperature_mc;
    gchar *temperature_source;
    gboolean kernel_curve_supported;
    gboolean kernel_curve_active;
    gint64 kernel_curve_error;
    gint64 kernel_curve_temp;
    gint kernel_curve_points[6];
} Y720FanStatus;

gboolean y720_fan_status_parse(JsonObject *data, Y720FanStatus *status, GError **error);
void y720_fan_status_clear(Y720FanStatus *status);

typedef struct {
    gchar *effect;
    gint colors[4];
    gint brightness[4];
} Y720BacklightProfile;

typedef struct {
    gchar *device;
    gchar *hid_device;
    gchar *driver;
    gchar *descriptor_sha256;
    gint report_bytes;
    gboolean state_known;
    gboolean has_profile;
    Y720BacklightProfile profile;
} Y720BacklightInfo;

gboolean y720_backlight_info_parse(JsonObject *data, Y720BacklightInfo *info, GError **error);
void y720_backlight_info_clear(Y720BacklightInfo *info);
void y720_backlight_profile_clear(Y720BacklightProfile *profile);
void y720_backlight_profile_copy(Y720BacklightProfile *dst, const Y720BacklightProfile *src);
gchar *y720_backlight_profile_to_json(const Y720BacklightProfile *profile);

const gchar *y720_color_name(gint index);
const gchar *y720_color_label(gint index);
const gchar *y720_effect_name(gint index);
const gchar *y720_effect_label(gint index);
gboolean y720_color_from_name(const gchar *name, gint *index);
gint y720_effect_index(const gchar *name);

G_END_DECLS

#endif
