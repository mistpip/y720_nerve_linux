/* SPDX-License-Identifier: MIT */
#ifndef Y720_CLI_H
#define Y720_CLI_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct {
    gboolean ok;
    gboolean timed_out;
    gboolean cancelled;
    gint exit_code;
    gint term_sig;

    gchar *program;
    gchar *version;
    gchar *stdout_text;
    gchar *stderr_text;

    JsonObject *data;
    gchar *error_code;
    gchar *error_message;
    JsonObject *error_details;
    gchar *transport_error;
    gchar *expected_program;
} Y720Result;

typedef void (*Y720CliCallback)(Y720Result *result, gpointer user_data);

gchar *y720_cli_locate(const gchar *name);

void y720_cli_run(const gchar *binary,
                  const gchar *const *args,
                  const gchar *input,
                  guint timeout_seconds,
                  GCancellable *cancellable,
                  GObject *owner,
                  Y720CliCallback callback,
                  gpointer user_data);

void y720_cli_run_elevated(const gchar *binary,
                            const gchar *const *args,
                            const gchar *input,
                            guint timeout_seconds,
                            GCancellable *cancellable,
                            GObject *owner,
                            Y720CliCallback callback,
                            gpointer user_data);
gboolean y720_cli_can_elevate(const gchar *binary);
void y720_cli_cancel_all(void);
gboolean y720_cli_has_pending(void);

void y720_result_free(Y720Result *result);
const gchar *y720_cli_error_hint(const gchar *code);

G_END_DECLS

#endif
