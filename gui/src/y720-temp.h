/* SPDX-License-Identifier: MIT */
#ifndef Y720_TEMP_H
#define Y720_TEMP_H

#include <glib.h>

G_BEGIN_DECLS

/* Maximum CPU/GPU temperature in Celsius; -1 when no sensor is readable. */
gint y720_temp_read_max(void);

G_END_DECLS

#endif
