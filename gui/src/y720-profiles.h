/* SPDX-License-Identifier: MIT */
#ifndef Y720_PROFILES_H
#define Y720_PROFILES_H

#include "y720-models.h"

G_BEGIN_DECLS

typedef struct _Y720ProfileStore Y720ProfileStore;

Y720ProfileStore *y720_profiles_load(void);
void y720_profiles_free(Y720ProfileStore *store);
void y720_profiles_save(Y720ProfileStore *store);

guint y720_profiles_count(Y720ProfileStore *store);
guint y720_profiles_selected(Y720ProfileStore *store);
const gchar *y720_profiles_name(Y720ProfileStore *store, guint index);
void y720_profiles_get(Y720ProfileStore *store, guint index, Y720BacklightProfile *profile);
void y720_profiles_set(Y720ProfileStore *store, guint index, const Y720BacklightProfile *profile);
guint y720_profiles_add(Y720ProfileStore *store, const Y720BacklightProfile *profile);
gboolean y720_profiles_remove(Y720ProfileStore *store, guint index);
void y720_profiles_select(Y720ProfileStore *store, guint index);
guint y720_profiles_next(Y720ProfileStore *store);
gboolean y720_profiles_is_off(Y720ProfileStore *store, guint index);
guint y720_profiles_off_index(Y720ProfileStore *store);

G_END_DECLS

#endif
