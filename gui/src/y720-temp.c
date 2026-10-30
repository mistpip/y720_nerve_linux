/* SPDX-License-Identifier: MIT */
#include "y720-temp.h"

/* Presentation only: temperature selection for control belongs to the kernel.
 * Never scan sysfs or sample hardware on GTK's main loop. */

gint y720_temp_display_from_status(const Y720FanStatus *status)
{
    if (!status->temperature_valid || status->temperature_mc < 1000 || status->temperature_mc > 120000)
        return -1;
    return (gint)((status->temperature_mc + 999) / 1000);
}
