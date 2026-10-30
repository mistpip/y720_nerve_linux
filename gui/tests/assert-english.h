/* SPDX-License-Identifier: MIT */
/* Verify rendered labels rather than only the existence of catalog entries. */
static void assert_english_text(const gchar *text)
{
    if (!text || !g_strcmp0(text, "中文")) return; /* Language autonym. */
    for (const gchar *p = text; *p; p = g_utf8_next_char(p)) {
        if (g_unichar_get_script(g_utf8_get_char(p)) == G_UNICODE_SCRIPT_HAN)
            g_error("Untranslated UI text: %s", text);
    }
}

static void assert_english_widget(GtkWidget *widget)
{
    if (g_object_get_data(G_OBJECT(widget), "y720-i18n-user-text")) return;
    if (GTK_IS_LABEL(widget)) assert_english_text(gtk_label_get_text(GTK_LABEL(widget)));
    assert_english_text(gtk_widget_get_tooltip_text(widget));
    if (GTK_IS_WINDOW(widget)) assert_english_text(gtk_window_get_title(GTK_WINDOW(widget)));
    if (ADW_IS_DIALOG(widget)) assert_english_text(adw_dialog_get_title(ADW_DIALOG(widget)));
    if (ADW_IS_PREFERENCES_GROUP(widget)) {
        assert_english_text(adw_preferences_group_get_title(ADW_PREFERENCES_GROUP(widget)));
        assert_english_text(adw_preferences_group_get_description(ADW_PREFERENCES_GROUP(widget)));
    }
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
         child = gtk_widget_get_next_sibling(child)) assert_english_widget(child);
}
