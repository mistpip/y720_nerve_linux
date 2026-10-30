/* SPDX-License-Identifier: MIT */
#ifndef Y720_TEMP_H
#define Y720_TEMP_H

#include <glib.h>
#include "y720-models.h"

G_BEGIN_DECLS

/* Display the driver's single control temperature, or -1 when unavailable. */
gint y720_temp_display_from_status(const Y720FanStatus *status);

G_END_DECLS

#endif
