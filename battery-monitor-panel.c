/*
 * battery-monitor-panel - GTK4 frontend for battery-monitord.
 *
 * Started by the daemon (with --auto) when a ceiling is crossed, or by you at
 * any time (bind it to a key). It holds no battery logic: it shows what the
 * daemon reports over D-Bus, edits ~/.config/battery-monitor/config.conf and
 * tells the daemon to reload. Closing it exits the whole process, so the
 * frontend costs nothing while you aren't looking at it.
 */
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <string.h>

#ifdef USE_LAYER_SHELL
#include <gtk-layer-shell/gtk-layer-shell.h>
#endif

#include "bm_common.h"

#define MAX_LOG_LINES 150
#define AUTOCLOSE_SECONDS 6

enum { S_HIGH = 0, S_SLEEP, S_SHUTDOWN, S_POLL, S_COUNT };

typedef struct {
    GtkApplication *app;
    GtkWidget *win;
    BmConfig cfg;

    GDBusConnection *bus;
    guint watch_id;
    gboolean online;

    /* readout */
    GtkWidget *percent_label, *state_label, *level_bar, *daemon_label;

    /* alarm section */
    GtkWidget *alarm_box, *alarm_title, *alarm_sub, *alarm_btn, *cancel_btn;
    GtkWidget *close_btn;

    /* settings */
    GtkWidget *scale[S_COUNT], *value_label[S_COUNT];
    GtkWidget *hint_label;
    GtkWidget *high_sw, *low_sw;

    GtkWidget *log_view;
    GtkTextBuffer *log_buf;

    /* behaviour */
    gboolean auto_mode;       /* spawned by the daemon */
    gboolean touched;         /* user interacted -> never auto-close */
    gboolean was_alarm, first_state;
    guint autoclose_id;
    guint blink_id;           /* alarm-button glow blink, only while an alarm shows */
} Panel;

static Panel P;

/* ---------------------------------------------------------------- */
/* log view                                                          */
/* ---------------------------------------------------------------- */

static void log_append(const char *line) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(P.log_buf, &end);
    gtk_text_buffer_insert(P.log_buf, &end, line, -1);
    gtk_text_buffer_insert(P.log_buf, &end, "\n", -1);

    int n = gtk_text_buffer_get_line_count(P.log_buf);
    if (n > MAX_LOG_LINES) {
        GtkTextIter s, c;
        gtk_text_buffer_get_start_iter(P.log_buf, &s);
        gtk_text_buffer_get_iter_at_line(P.log_buf, &c, n - MAX_LOG_LINES);
        gtk_text_buffer_delete(P.log_buf, &s, &c);
    }
    GtkTextMark *m = gtk_text_buffer_get_insert(P.log_buf);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(P.log_view), m);
}

/* ---------------------------------------------------------------- */
/* daemon communication                                              */
/* ---------------------------------------------------------------- */

static void daemon_call(const char *method, GVariant *params) {
    if (!P.bus) { if (params) g_variant_unref(g_variant_ref_sink(params)); return; }
    g_dbus_connection_call(P.bus, BM_BUS_NAME, BM_BUS_PATH, BM_BUS_IFACE, method, params, NULL,
                           G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL, NULL);
}

static gboolean autoclose_cb(gpointer ud) {
    (void)ud;
    P.autoclose_id = 0;
    g_application_quit(G_APPLICATION(P.app));
    return G_SOURCE_REMOVE;
}

/* Two static glow states toggled twice a second. (A CSS @keyframes animation
 * redraws the blurred shadow at 60 fps and its render caches grew without
 * bound in testing.) */
static gboolean blink_cb(gpointer ud) {
    (void)ud;
    if (gtk_widget_has_css_class(P.alarm_btn, "glow-hi")) gtk_widget_remove_css_class(P.alarm_btn, "glow-hi");
    else gtk_widget_add_css_class(P.alarm_btn, "glow-hi");
    return G_SOURCE_CONTINUE;
}

static void blink_set(gboolean on) {
    if (on && !P.blink_id) P.blink_id = g_timeout_add(600, blink_cb, NULL);
    else if (!on && P.blink_id) {
        g_source_remove(P.blink_id);
        P.blink_id = 0;
        gtk_widget_remove_css_class(P.alarm_btn, "glow-hi");
    }
}

static void apply_state(GVariant *d) {
    int percent = -1, seconds = -1;
    gboolean plugged = FALSE, sound = FALSE, action = FALSE, dry = FALSE;
    const char *status = "", *alarm = "none";
    g_variant_lookup(d, "percent", "i", &percent);
    g_variant_lookup(d, "seconds", "i", &seconds);
    g_variant_lookup(d, "plugged", "b", &plugged);
    g_variant_lookup(d, "sound",   "b", &sound);
    g_variant_lookup(d, "action",  "b", &action);
    g_variant_lookup(d, "dry_run", "b", &dry);
    g_variant_lookup(d, "status",  "&s", &status);
    g_variant_lookup(d, "alarm",   "&s", &alarm);

    /* readout */
    char buf[160];
    if (percent >= 0) g_snprintf(buf, sizeof buf, "%d%%", percent); else g_strlcpy(buf, "--%", sizeof buf);
    gtk_label_set_text(GTK_LABEL(P.percent_label), buf);
    g_snprintf(buf, sizeof buf, "%s%s", status, plugged ? "  ·  AC connected" : "");
    gtk_label_set_text(GTK_LABEL(P.state_label), buf);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(P.level_bar), percent < 0 ? 0 : percent / 100.0);

    const char *lv[] = { "level-ok", "level-warn", "level-crit", "level-high" };
    for (int i = 0; i < 4; i++) gtk_widget_remove_css_class(P.percent_label, lv[i]);
    const char *cls = "level-ok";
    if (percent >= 0) {
        if (percent <= P.cfg.shutdown) cls = "level-crit";
        else if (percent <= P.cfg.sleep_at) cls = "level-warn";
        else if (percent >= P.cfg.high) cls = "level-high";
    }
    gtk_widget_add_css_class(P.percent_label, cls);

    /* alarm section */
    gboolean active = g_strcmp0(alarm, "none") != 0;
    gtk_widget_set_visible(P.alarm_box, active);
    blink_set(active && sound);
    if (active) {
        const char *what = !g_strcmp0(alarm, "high") ? "HIGH BATTERY" :
                           !g_strcmp0(alarm, "sleep") ? "LOW BATTERY" : "CRITICAL BATTERY";
        g_snprintf(buf, sizeof buf, "%s  %d%%%s", what, percent, dry ? "   (TEST)" : "");
        gtk_label_set_text(GTK_LABEL(P.alarm_title), buf);

        if (action) {
            const char *verb = !g_strcmp0(alarm, "shutdown") ? "Shutting down" : "Sleeping";
            g_snprintf(buf, sizeof buf, "%s in %d s%s\nPlug in the charger to cancel automatically.",
                       verb, seconds, dry ? "  (test: nothing will happen)" : "");
        } else {
            g_snprintf(buf, sizeof buf, "Unplug the charger, or turn the alarm off.");
        }
        gtk_label_set_text(GTK_LABEL(P.alarm_sub), buf);

        gtk_widget_set_visible(P.alarm_btn, sound);
        gtk_widget_set_visible(P.cancel_btn, action);
        gtk_button_set_label(GTK_BUTTON(P.cancel_btn),
                             !g_strcmp0(alarm, "shutdown") ? "Cancel shutdown" : "Cancel sleep");
        if (!P.was_alarm) gtk_window_present(GTK_WINDOW(P.win));
    }
    gtk_widget_set_sensitive(P.close_btn, !(active && sound));

    /* auto-close a daemon-spawned panel shortly after the alarm is over */
    if (active) {
        if (P.autoclose_id) { g_source_remove(P.autoclose_id); P.autoclose_id = 0; }
    } else if ((P.was_alarm || P.first_state) && P.auto_mode && !P.touched && !P.autoclose_id) {
        P.autoclose_id = g_timeout_add_seconds(AUTOCLOSE_SECONDS, autoclose_cb, NULL);
    }
    P.was_alarm = active;
    P.first_state = FALSE;
}

static void on_state_signal(GDBusConnection *c, const gchar *s, const gchar *o, const gchar *i,
                            const gchar *n, GVariant *params, gpointer ud) {
    (void)c; (void)s; (void)o; (void)i; (void)n; (void)ud;
    GVariant *d = NULL;
    g_variant_get(params, "(@a{sv})", &d);
    apply_state(d);
    g_variant_unref(d);
}

static void on_log_signal(GDBusConnection *c, const gchar *s, const gchar *o, const gchar *i,
                          const gchar *n, GVariant *params, gpointer ud) {
    (void)c; (void)s; (void)o; (void)i; (void)n; (void)ud;
    const char *line = NULL;
    g_variant_get(params, "(&s)", &line);
    if (line) log_append(line);
}

static void on_get_state(GObject *src, GAsyncResult *res, gpointer ud) {
    (void)ud;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (!r) return;
    GVariant *d = NULL;
    g_variant_get(r, "(@a{sv})", &d);
    apply_state(d);
    g_variant_unref(d);
    g_variant_unref(r);
}

static void on_get_log(GObject *src, GAsyncResult *res, gpointer ud) {
    (void)ud;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (!r) return;
    gchar **lines = NULL;
    g_variant_get(r, "(^as)", &lines);
    gtk_text_buffer_set_text(P.log_buf, "", -1);
    for (int i = 0; lines && lines[i]; i++) log_append(lines[i]);
    g_strfreev(lines);
    g_variant_unref(r);
}

static void daemon_appeared(GDBusConnection *c, const gchar *name, const gchar *owner, gpointer ud) {
    (void)c; (void)name; (void)owner; (void)ud;
    P.online = TRUE;
    gtk_label_set_text(GTK_LABEL(P.daemon_label), "daemon: running");
    g_dbus_connection_call(P.bus, BM_BUS_NAME, BM_BUS_PATH, BM_BUS_IFACE, "GetState", NULL,
                           G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, on_get_state, NULL);
    g_dbus_connection_call(P.bus, BM_BUS_NAME, BM_BUS_PATH, BM_BUS_IFACE, "GetLog", NULL,
                           G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, on_get_log, NULL);
}

static void daemon_vanished(GDBusConnection *c, const gchar *name, gpointer ud) {
    (void)c; (void)name; (void)ud;
    P.online = FALSE;
    gtk_label_set_text(GTK_LABEL(P.daemon_label), "daemon: NOT running - start battery-monitord");
    blink_set(FALSE);
    gtk_widget_set_visible(P.alarm_box, FALSE);
    gtk_widget_set_sensitive(P.close_btn, TRUE);
}

/* ---------------------------------------------------------------- */
/* UI callbacks                                                      */
/* ---------------------------------------------------------------- */

static void update_value_label(int id) {
    int v = (int)gtk_range_get_value(GTK_RANGE(P.scale[id]));
    char b[48];
    if (id == S_POLL) {
        if (v % 60 == 0) g_snprintf(b, sizeof b, "%d s  (%d min)", v, v / 60);
        else             g_snprintf(b, sizeof b, "%d s  (%.1f min)", v, v / 60.0);
    } else {
        g_snprintf(b, sizeof b, "%d%%", v);
    }
    gtk_label_set_text(GTK_LABEL(P.value_label[id]), b);
}

static void update_hint(void) {
    gtk_widget_set_visible(P.hint_label, P.cfg.shutdown >= P.cfg.sleep_at);
}

static void on_scale_changed(GtkRange *r, gpointer ud) {
    int id = GPOINTER_TO_INT(ud);
    int v = (int)gtk_range_get_value(r);
    switch (id) {
    case S_HIGH:     P.cfg.high = v; break;
    case S_SLEEP:    P.cfg.sleep_at = v; break;
    case S_SHUTDOWN: P.cfg.shutdown = v; break;
    case S_POLL:     P.cfg.poll_seconds = bm_clamp(v, BM_POLL_MIN, BM_POLL_MAX); break;
    }
    update_value_label(id);
    update_hint();
    P.touched = TRUE;
}

static void on_switch_changed(GObject *sw, GParamSpec *ps, gpointer ud) {
    (void)ps;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    if (GPOINTER_TO_INT(ud) == 0) P.cfg.sound_high = on; else P.cfg.sound_low = on;
    P.touched = TRUE;
}

static void on_save(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    P.touched = TRUE;
    if (bm_config_save(&P.cfg)) {
        log_append("Settings saved.");
        daemon_call("Reload", NULL);
    } else {
        log_append("ERROR: could not write the config file.");
    }
}

static void on_check_now(GtkButton *b, gpointer ud) { (void)b; (void)ud; daemon_call("CheckNow", NULL); }
static void on_silence(GtkButton *b, gpointer ud)   { (void)b; (void)ud; daemon_call("Silence", NULL); }
static void on_cancel(GtkButton *b, gpointer ud)    { (void)b; (void)ud; daemon_call("CancelAction", NULL); }
static void on_close(GtkButton *b, gpointer ud)     { (void)b; (void)ud; g_application_quit(G_APPLICATION(P.app)); }

static void on_test(GtkButton *b, gpointer ud) {
    (void)ud;
    P.touched = TRUE;
    const char *kind = g_object_get_data(G_OBJECT(b), "kind");
    if (!P.online) { log_append("Daemon is not running - cannot test."); return; }
    daemon_call("Test", g_variant_new("(s)", kind));
}

/* ---------------------------------------------------------------- */
/* widget builders                                                   */
/* ---------------------------------------------------------------- */

static GtkWidget *section_label(const char *t) {
    GtkWidget *l = gtk_label_new(t);
    gtk_widget_add_css_class(l, "section-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    return l;
}

static GtkWidget *slider_row(int id, const char *name, int min, int max, int step, int value) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *nl = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(nl), 0.0);
    gtk_widget_set_hexpand(nl, TRUE);
    gtk_widget_add_css_class(nl, "slider-name");
    P.value_label[id] = gtk_label_new("");
    gtk_widget_add_css_class(P.value_label[id], "slider-value");
    gtk_box_append(GTK_BOX(top), nl);
    gtk_box_append(GTK_BOX(top), P.value_label[id]);

    GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step);
    gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
    gtk_range_set_value(GTK_RANGE(sc), value);
    gtk_widget_add_css_class(sc, "abyss-slider");
    P.scale[id] = sc;
    update_value_label(id);
    g_signal_connect(sc, "value-changed", G_CALLBACK(on_scale_changed), GINT_TO_POINTER(id));

    gtk_box_append(GTK_BOX(row), top);
    gtk_box_append(GTK_BOX(row), sc);
    return row;
}

static GtkWidget *switch_row(const char *name, gboolean on, int id, GtkWidget **out) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *l = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_widget_add_css_class(l, "switch-name");
    GtkWidget *sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), on);
    gtk_widget_add_css_class(sw, "abyss-switch");
    g_signal_connect(sw, "notify::active", G_CALLBACK(on_switch_changed), GINT_TO_POINTER(id));
    gtk_box_append(GTK_BOX(row), l);
    gtk_box_append(GTK_BOX(row), sw);
    *out = sw;
    return row;
}

static GtkWidget *small_button(const char *label, GCallback cb, const char *kind, const char *css) {
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, css);
    gtk_widget_set_hexpand(b, TRUE);
    if (kind) g_object_set_data(G_OBJECT(b), "kind", (gpointer)kind);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static void load_css(void) {
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p,
"window.abyss-panel {"
"  background-color: rgba(8, 9, 14, 0.88); border-radius: 20px;"
"  border: 1px solid rgba(120, 140, 255, 0.22); box-shadow: 0 0 40px rgba(90, 70, 220, 0.25); }"
"window.abyss-panel label { color: #d9dcff; }"
".panel-title { font-weight: 800; font-size: 11px; letter-spacing: 2px; color: #8d93c9; }"
".close-btn { background-image: none; background-color: rgba(255,255,255,0.06); color: #9aa0d6;"
"  border-radius: 10px; padding: 0 8px; min-height: 22px; }"
".section-label { font-weight: 700; font-size: 11px; letter-spacing: 1.5px; color: #8d93c9; margin-top: 10px; }"
"#percent-label { font-size: 46px; font-weight: 800; }"
"#percent-label.level-ok   { color: #7CF2B0; }"
"#percent-label.level-high { color: #7CC8F2; }"
"#percent-label.level-warn { color: #F2D27C; }"
"#percent-label.level-crit { color: #F27C7C; }"
"#state-label { color: #9aa0d6; font-size: 12px; }"
"#daemon-label { color: #6f76ad; font-size: 10px; }"
"levelbar block.filled { background-color: #7C8CF2; border-radius: 6px; }"
"levelbar block.empty { background-color: rgba(255,255,255,0.08); border-radius: 6px; }"
"levelbar trough { min-height: 10px; border-radius: 6px; }"
".abyss-slider trough { min-height: 6px; border-radius: 6px; background-color: rgba(255,255,255,0.08); }"
".abyss-slider highlight { background-color: #7C8CF2; border-radius: 6px; }"
".abyss-slider slider { min-width: 16px; min-height: 16px; border-radius: 50%; background-color: #c9cfff;"
"  box-shadow: 0 0 10px rgba(124,140,242,0.8); }"
".slider-name, .switch-name { font-size: 12px; color: #c4c8ef; }"
".slider-value { font-size: 12px; color: #7C8CF2; font-weight: 700; }"
".hint { color: #F2D27C; font-size: 10px; }"
"textview.abyss-log, textview.abyss-log text { background-color: rgba(255,255,255,0.03); color: #8a90c9;"
"  font-family: monospace; font-size: 10px; }"
".save-btn { background-image: none; background-color: #3a3f78; color: #d9dcff; border-radius: 10px; padding: 6px 12px; }"
".test-btn { background-image: none; background-color: rgba(255,255,255,0.06); color: #9aa0d6;"
"  border-radius: 10px; padding: 4px 8px; font-size: 11px; }"
/* ---- alarm section: only visible while an alarm is active ---- */
".alarm-box { background-color: #030207; border: 1px solid rgba(255,46,87,0.55); border-radius: 16px; padding: 14px;"
"  margin-bottom: 8px; box-shadow: 0 0 26px rgba(255,46,87,0.25), 0 0 26px rgba(58,91,255,0.20); }"
".alarm-title { color: #ff8fa3; font-weight: 800; font-size: 14px; letter-spacing: 2px; }"
".alarm-sub { color: #cfd5ff; font-size: 12px; }"
"button.alarm-btn { min-height: 72px; border-radius: 16px; border-width: 2px; border-style: solid;"
"  border-color: #ff2e57 #3a5bff #3a5bff #ff2e57;"
"  background-color: #000000;"
"  background-image: linear-gradient(120deg, #000000 0%, #1c0412 38%, #05072b 100%);"
"  color: #ffe3ea; font-size: 22px; font-weight: 900; letter-spacing: 4px;"
"  text-shadow: 0 0 10px rgba(255,46,87,0.9), 0 0 18px rgba(58,91,255,0.7);"
"  box-shadow: -3px 0 12px rgba(255,46,87,0.35), 3px 0 12px rgba(58,91,255,0.35); }"
"button.alarm-btn.glow-hi { box-shadow: -6px 0 28px rgba(255,46,87,0.90), 6px 0 28px rgba(58,91,255,0.90); }"
"button.alarm-btn:hover { background-image: linear-gradient(120deg, #120008 0%, #2a0618 38%, #0a0f45 100%); }"
"button.alarm-btn:active { background-image: linear-gradient(120deg, #000, #000); }"
"button.cancel-btn { background-image: none; background-color: #1b1020; color: #ffc4cf; border-radius: 12px;"
"  border: 1px solid rgba(255,46,87,0.5); padding: 8px 14px; font-weight: 700; }"
);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

/* ---------------------------------------------------------------- */
/* window                                                            */
/* ---------------------------------------------------------------- */

static void build_window(GtkApplication *app) {
    load_css();
    bm_config_load(&P.cfg);
    P.first_state = TRUE;

    GtkWidget *win = gtk_application_window_new(app);
    P.win = win;
    gtk_window_set_title(GTK_WINDOW(win), "Battery Monitor");
    gtk_window_set_default_size(GTK_WINDOW(win), 340, -1);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_widget_add_css_class(win, "abyss-panel");

#ifdef USE_LAYER_SHELL
    if (gtk_layer_is_supported()) {
        gtk_layer_init_for_window(GTK_WINDOW(win));
        gtk_layer_set_namespace(GTK_WINDOW(win), "battery-monitor");
        gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, 24);
        gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, 24);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    }
#endif

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(root, 16);
    gtk_widget_set_margin_bottom(root, 14);
    gtk_widget_set_margin_start(root, 20);
    gtk_widget_set_margin_end(root, 20);

    /* header */
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *title = gtk_label_new("BATTERY MONITOR");
    gtk_widget_add_css_class(title, "panel-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_widget_set_hexpand(title, TRUE);
    P.close_btn = gtk_button_new_with_label("✕");
    gtk_widget_add_css_class(P.close_btn, "close-btn");
    g_signal_connect(P.close_btn, "clicked", G_CALLBACK(on_close), NULL);
    gtk_box_append(GTK_BOX(head), title);
    gtk_box_append(GTK_BOX(head), P.close_btn);
    gtk_box_append(GTK_BOX(root), head);

    /* alarm box (hidden until the daemon reports an active alarm) */
    P.alarm_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(P.alarm_box, "alarm-box");
    gtk_widget_set_margin_top(P.alarm_box, 8);
    P.alarm_title = gtk_label_new("");
    gtk_widget_add_css_class(P.alarm_title, "alarm-title");
    P.alarm_sub = gtk_label_new("");
    gtk_widget_add_css_class(P.alarm_sub, "alarm-sub");
    gtk_label_set_justify(GTK_LABEL(P.alarm_sub), GTK_JUSTIFY_CENTER);
    P.alarm_btn = gtk_button_new_with_label("TURN OFF ALARM");
    gtk_widget_add_css_class(P.alarm_btn, "alarm-btn");
    g_signal_connect(P.alarm_btn, "clicked", G_CALLBACK(on_silence), NULL);
    P.cancel_btn = gtk_button_new_with_label("Cancel sleep");
    gtk_widget_add_css_class(P.cancel_btn, "cancel-btn");
    g_signal_connect(P.cancel_btn, "clicked", G_CALLBACK(on_cancel), NULL);
    gtk_box_append(GTK_BOX(P.alarm_box), P.alarm_title);
    gtk_box_append(GTK_BOX(P.alarm_box), P.alarm_sub);
    gtk_box_append(GTK_BOX(P.alarm_box), P.alarm_btn);
    gtk_box_append(GTK_BOX(P.alarm_box), P.cancel_btn);
    gtk_widget_set_visible(P.alarm_box, FALSE);
    gtk_box_append(GTK_BOX(root), P.alarm_box);

    /* readout */
    P.percent_label = gtk_label_new("--%");
    gtk_widget_set_name(P.percent_label, "percent-label");
    gtk_widget_add_css_class(P.percent_label, "level-ok");
    P.state_label = gtk_label_new("-");
    gtk_widget_set_name(P.state_label, "state-label");
    P.level_bar = gtk_level_bar_new_for_interval(0.0, 1.0);
    gtk_level_bar_set_mode(GTK_LEVEL_BAR(P.level_bar), GTK_LEVEL_BAR_MODE_CONTINUOUS);
    gtk_widget_set_margin_top(P.level_bar, 6);
    P.daemon_label = gtk_label_new("daemon: connecting...");
    gtk_widget_set_name(P.daemon_label, "daemon-label");
    gtk_widget_set_margin_top(P.daemon_label, 4);
    gtk_box_append(GTK_BOX(root), P.percent_label);
    gtk_box_append(GTK_BOX(root), P.state_label);
    gtk_box_append(GTK_BOX(root), P.level_bar);
    gtk_box_append(GTK_BOX(root), P.daemon_label);

    /* ceilings */
    gtk_box_append(GTK_BOX(root), section_label("CEILINGS  (0 - 100 %)"));
    gtk_box_append(GTK_BOX(root), slider_row(S_HIGH, "High - alarm only", 0, 100, 1, P.cfg.high));
    gtk_box_append(GTK_BOX(root), slider_row(S_SLEEP, "Sleep (suspend)", 0, 100, 1, P.cfg.sleep_at));
    gtk_box_append(GTK_BOX(root), slider_row(S_SHUTDOWN, "Shutdown", 0, 100, 1, P.cfg.shutdown));
    P.hint_label = gtk_label_new("Shutdown ≥ Sleep: sleep will never trigger (shutdown wins).");
    gtk_widget_add_css_class(P.hint_label, "hint");
    gtk_label_set_xalign(GTK_LABEL(P.hint_label), 0.0);
    gtk_label_set_wrap(GTK_LABEL(P.hint_label), TRUE);
    gtk_box_append(GTK_BOX(root), P.hint_label);
    update_hint();

    /* poll */
    gtk_box_append(GTK_BOX(root), section_label("CHECK INTERVAL"));
    gtk_box_append(GTK_BOX(root), slider_row(S_POLL, "Battery check every", BM_POLL_MIN, BM_POLL_MAX, 10,
                                             P.cfg.poll_seconds));

    /* sounds */
    gtk_box_append(GTK_BOX(root), section_label("ALARM SOUND"));
    gtk_box_append(GTK_BOX(root), switch_row("High-battery alarm sound", P.cfg.sound_high, 0, &P.high_sw));
    gtk_box_append(GTK_BOX(root), switch_row("Low-battery alarm sound", P.cfg.sound_low, 1, &P.low_sw));

    /* tests */
    gtk_box_append(GTK_BOX(root), section_label("TEST (no real sleep / poweroff)"));
    GtkWidget *tr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(tr), small_button("High", G_CALLBACK(on_test), "high", "test-btn"));
    gtk_box_append(GTK_BOX(tr), small_button("Sleep", G_CALLBACK(on_test), "sleep", "test-btn"));
    gtk_box_append(GTK_BOX(tr), small_button("Shutdown", G_CALLBACK(on_test), "shutdown", "test-btn"));
    gtk_box_append(GTK_BOX(root), tr);

    /* save / check */
    GtkWidget *sr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(sr, 10);
    GtkWidget *save = small_button("Save & apply", G_CALLBACK(on_save), NULL, "save-btn");
    GtkWidget *chk = small_button("Check now", G_CALLBACK(on_check_now), NULL, "test-btn");
    gtk_box_append(GTK_BOX(sr), save);
    gtk_box_append(GTK_BOX(sr), chk);
    gtk_box_append(GTK_BOX(root), sr);

    /* activity log (filled from the daemon) */
    gtk_box_append(GTK_BOX(root), section_label("ACTIVITY"));
    GtkWidget *sc = gtk_scrolled_window_new();
    gtk_widget_set_size_request(sc, -1, 110);
    P.log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(P.log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(P.log_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(P.log_view), GTK_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(P.log_view, "abyss-log");
    P.log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(P.log_view));
    gtk_text_buffer_set_enable_undo(P.log_buf, FALSE);   /* undo history would grow without bound */
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), P.log_view);
    gtk_box_append(GTK_BOX(root), sc);

    gtk_window_set_child(GTK_WINDOW(win), root);

    /* daemon link */
    GError *err = NULL;
    P.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (P.bus) {
        g_dbus_connection_signal_subscribe(P.bus, BM_BUS_NAME, BM_BUS_IFACE, "StateChanged", BM_BUS_PATH,
                                           NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_state_signal, NULL, NULL);
        g_dbus_connection_signal_subscribe(P.bus, BM_BUS_NAME, BM_BUS_IFACE, "Log", BM_BUS_PATH,
                                           NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_log_signal, NULL, NULL);
        P.watch_id = g_bus_watch_name_on_connection(P.bus, BM_BUS_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                    daemon_appeared, daemon_vanished, NULL, NULL);
    } else {
        gtk_label_set_text(GTK_LABEL(P.daemon_label), "no session D-Bus");
        g_clear_error(&err);
    }
}

static int on_command_line(GApplication *app, GApplicationCommandLine *cl, gpointer ud) {
    (void)ud;
    GVariantDict *opts = g_application_command_line_get_options_dict(cl);
    gboolean from_daemon = g_variant_dict_contains(opts, "auto");
    if (!P.win) {
        P.auto_mode = from_daemon;
        P.app = GTK_APPLICATION(app);
        build_window(GTK_APPLICATION(app));
    } else if (!from_daemon) {
        P.touched = TRUE;                 /* you opened it on purpose: keep it open */
    }
    gtk_window_present(GTK_WINDOW(P.win));
    return 0;
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.local.batterymonitor.panel",
                                              G_APPLICATION_HANDLES_COMMAND_LINE);
    g_application_add_main_option(G_APPLICATION(app), "auto", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                  "Started by battery-monitord (closes itself after the alarm)", NULL);
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    if (P.watch_id) g_bus_unwatch_name(P.watch_id);
    g_object_unref(app);
    bm_config_clear(&P.cfg);
    return status;
}
