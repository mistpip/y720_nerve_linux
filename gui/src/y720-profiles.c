/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include "y720-profiles.h"

#include <string.h>

struct _Y720ProfileStore {
    GPtrArray *names;
    GPtrArray *profiles; /* Y720BacklightProfile* */
    guint selected;
};

#define OFF_PROFILE_NAME "不开灯"

static gchar *profiles_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "y720-control", "profiles.json", NULL);
}

static Y720BacklightProfile *profile_new(const gchar *effect, const gint colors[4],
                                         const gint brightness[4])
{
    Y720BacklightProfile *profile = g_new0(Y720BacklightProfile, 1);
    profile->effect = g_strdup(effect);
    memcpy(profile->colors, colors, sizeof(profile->colors));
    memcpy(profile->brightness, brightness, sizeof(profile->brightness));
    return profile;
}

static void profile_free(gpointer data)
{
    Y720BacklightProfile *profile = data;
    y720_backlight_profile_clear(profile);
    g_free(profile);
}

static void add_default(Y720ProfileStore *store, const gchar *name,
                        const gchar *effect, const gint colors[4],
                        const gint brightness[4])
{
    g_ptr_array_add(store->names, g_strdup(name));
    g_ptr_array_add(store->profiles, profile_new(effect, colors, brightness));
}

Y720ProfileStore *y720_profiles_load(void)
{
    Y720ProfileStore *store = g_new0(Y720ProfileStore, 1);
    store->names = g_ptr_array_new_with_free_func(g_free);
    store->profiles = g_ptr_array_new_with_free_func(profile_free);

    gchar *path = profiles_path();
    gchar *data = NULL;
    gboolean loaded = FALSE;
    if (g_file_get_contents(path, &data, NULL, NULL)) {
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser, data, -1, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                JsonObject *object = json_node_get_object(root);
                if (json_object_has_member(object, "schema_version") &&
                    json_object_get_int_member(object, "schema_version") == 1 &&
                    json_object_has_member(object, "profiles") &&
                    JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, "profiles"))) {
                    JsonArray *array = json_object_get_array_member(object, "profiles");
                    guint length = json_array_get_length(array);
                    for (guint i = 0; i < length && i < 32; i++) {
                        JsonNode *node = json_array_get_element(array, i);
                        if (!node || !JSON_NODE_HOLDS_OBJECT(node))
                            continue;
                        JsonObject *entry = json_node_get_object(node);
                        if (!json_object_has_member(entry, "name") ||
                            !json_object_has_member(entry, "colors") ||
                            !json_object_has_member(entry, "brightness"))
                            continue;
                        JsonNode *name_node = json_object_get_member(entry, "name");
                        if (!JSON_NODE_HOLDS_VALUE(name_node) ||
                            json_node_get_value_type(name_node) != G_TYPE_STRING)
                            continue;
                        const gchar *name = json_object_get_string_member(entry, "name");
                        const gchar *effect = "static";
                        if (json_object_has_member(entry, "effect")) {
                            JsonNode *effect_node = json_object_get_member(entry, "effect");
                            if (JSON_NODE_HOLDS_VALUE(effect_node) &&
                                json_node_get_value_type(effect_node) == G_TYPE_STRING)
                                effect = json_object_get_string_member(entry, "effect");
                        }
                        if (!json_object_has_member(entry, "colors") ||
                            !JSON_NODE_HOLDS_ARRAY(json_object_get_member(entry, "colors")) ||
                            !json_object_has_member(entry, "brightness") ||
                            !JSON_NODE_HOLDS_ARRAY(json_object_get_member(entry, "brightness")))
                            continue;
                        JsonArray *colors = json_object_get_array_member(entry, "colors");
                        JsonArray *brightness = json_object_get_array_member(entry, "brightness");
                        if (!name || !colors || !brightness ||
                            json_array_get_length(colors) != 4 ||
                            json_array_get_length(brightness) != 4)
                            continue;
                        gint c[4], b[4];
                        gboolean valid = TRUE;
                        for (guint k = 0; k < 4; k++) {
                            JsonNode *cn = json_array_get_element(colors, k);
                            JsonNode *bn = json_array_get_element(brightness, k);
                            if (!JSON_NODE_HOLDS_VALUE(cn) || !JSON_NODE_HOLDS_VALUE(bn)) {
                                valid = FALSE;
                                break;
                            }
                            c[k] = (gint)json_node_get_int(cn);
                            b[k] = (gint)json_node_get_int(bn);
                            if (c[k] < 0 || c[k] > 19 || b[k] < 0 || b[k] > 3)
                                valid = FALSE;
                        }
                        if (!valid)
                            continue;
                        g_ptr_array_add(store->names, g_strdup(name));
                        g_ptr_array_add(store->profiles, profile_new(effect, c, b));
                    }
                    if (store->profiles->len > 0) {
                        gint selected = json_object_has_member(object, "selected")
                                            ? (gint)json_object_get_int_member(object, "selected") : 0;
                        store->selected = CLAMP(selected, 0, (gint)store->profiles->len - 1);
                        loaded = TRUE;
                    }
                }
            }
        }
        g_object_unref(parser);
    }
    g_free(data);
    g_free(path);

    if (loaded && y720_profiles_off_index(store) == store->profiles->len) {
        const gint off[4] = {0, 0, 0, 0};
        const gint off_bright[4] = {0, 0, 0, 0};
        add_default(store, OFF_PROFILE_NAME, "static", off, off_bright);
    }

    if (!loaded) {
        const gint red[4] = {0, 0, 0, 0};
        const gint blue[4] = {6, 6, 6, 6};
        const gint purple[4] = {4, 4, 4, 4};
        const gint bright[4] = {2, 2, 2, 2};
        const gint bright3[4] = {3, 3, 3, 3};
        const gint off[4] = {0, 0, 0, 0};
        const gint off_bright[4] = {0, 0, 0, 0};
        add_default(store, "方案 1", "static", red, bright);
        add_default(store, "方案 2", "breath", blue, bright);
        add_default(store, "方案 3", "wave", purple, bright3);
        add_default(store, OFF_PROFILE_NAME, "static", off, off_bright);
        store->selected = 0;
    }
    return store;
}

void y720_profiles_free(Y720ProfileStore *store)
{
    if (!store)
        return;
    g_ptr_array_free(store->names, TRUE);
    g_ptr_array_free(store->profiles, TRUE);
    g_free(store);
}

void y720_profiles_save(Y720ProfileStore *store)
{
    if (!store)
        return;
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "schema_version");
    json_builder_add_int_value(builder, 1);
    json_builder_set_member_name(builder, "selected");
    json_builder_add_int_value(builder, store->selected);
    json_builder_set_member_name(builder, "profiles");
    json_builder_begin_array(builder);
    for (guint i = 0; i < store->profiles->len; i++) {
        Y720BacklightProfile *profile = g_ptr_array_index(store->profiles, i);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, g_ptr_array_index(store->names, i));
        json_builder_set_member_name(builder, "effect");
        json_builder_add_string_value(builder, profile->effect ? profile->effect : "static");
        json_builder_set_member_name(builder, "colors");
        json_builder_begin_array(builder);
        for (guint k = 0; k < 4; k++)
            json_builder_add_int_value(builder, profile->colors[k]);
        json_builder_end_array(builder);
        json_builder_set_member_name(builder, "brightness");
        json_builder_begin_array(builder);
        for (guint k = 0; k < 4; k++)
            json_builder_add_int_value(builder, profile->brightness[k]);
        json_builder_end_array(builder);
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);
    json_builder_end_object(builder);

    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gchar *data = json_generator_to_data(generator, NULL);
    gchar *path = profiles_path();
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    gchar *tmp = g_strconcat(path, ".tmp", NULL);
    if (g_file_set_contents(tmp, data, -1, NULL))
        rename(tmp, path);
    g_free(tmp);
    g_free(dir);
    g_free(path);
    g_free(data);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
}

guint y720_profiles_count(Y720ProfileStore *store)
{
    return store->profiles->len;
}

guint y720_profiles_selected(Y720ProfileStore *store)
{
    return store->selected;
}

const gchar *y720_profiles_name(Y720ProfileStore *store, guint index)
{
    if (index >= store->names->len)
        return NULL;
    return g_ptr_array_index(store->names, index);
}

void y720_profiles_get(Y720ProfileStore *store, guint index, Y720BacklightProfile *profile)
{
    if (index >= store->profiles->len) {
        memset(profile, 0, sizeof(*profile));
        return;
    }
    y720_backlight_profile_copy(profile, g_ptr_array_index(store->profiles, index));
}

void y720_profiles_set(Y720ProfileStore *store, guint index, const Y720BacklightProfile *profile)
{
    if (index >= store->profiles->len)
        return;
    y720_backlight_profile_copy(g_ptr_array_index(store->profiles, index), profile);
}

guint y720_profiles_add(Y720ProfileStore *store, const Y720BacklightProfile *profile)
{
    gint number = (gint)store->profiles->len + 1;
    gchar *name = g_strdup_printf("方案 %d", number);
    g_ptr_array_add(store->names, name);
    Y720BacklightProfile *copy = g_new0(Y720BacklightProfile, 1);
    y720_backlight_profile_copy(copy, profile);
    g_ptr_array_add(store->profiles, copy);
    return store->profiles->len - 1;
}

gboolean y720_profiles_remove(Y720ProfileStore *store, guint index)
{
    if (store->profiles->len <= 1 || index >= store->profiles->len ||
        y720_profiles_is_off(store, index))
        return FALSE;
    g_ptr_array_remove_index(store->names, index);
    g_ptr_array_remove_index(store->profiles, index);
    if (store->selected >= store->profiles->len)
        store->selected = store->profiles->len - 1;
    return TRUE;
}

void y720_profiles_select(Y720ProfileStore *store, guint index)
{
    if (index < store->profiles->len)
        store->selected = index;
}

guint y720_profiles_next(Y720ProfileStore *store)
{
    store->selected = (store->selected + 1) % store->profiles->len;
    return store->selected;
}

gboolean y720_profiles_is_off(Y720ProfileStore *store, guint index)
{
    const gchar *name = y720_profiles_name(store, index);
    return g_strcmp0(name, OFF_PROFILE_NAME) == 0;
}

guint y720_profiles_off_index(Y720ProfileStore *store)
{
    for (guint i = 0; i < store->profiles->len; i++)
        if (y720_profiles_is_off(store, i))
            return i;
    return store->profiles->len;
}
