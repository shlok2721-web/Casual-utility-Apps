/*
 * battery-monitord - tiny background battery watchdog (GLib only, no GTK).
 *
 *  - sleeps in the GLib main loop; wakes only for the poll timer (60..1200 s,
 *    set in the panel), D-Bus calls and the logind resume signal
 *  - when a ceiling is crossed: loops an alarm sound (ALSA, infinite), sends a
 *    notification and spawns the panel (battery-monitor-panel --auto)
 *  - sleep/shutdown ceilings play the alarm for `grace_seconds` (default 60)
 *    first, then suspend/poweroff. After resume, the check runs again at once
 *    and the alarm restarts if the battery is still low and unplugged.
 */
#include <gio/gio.h>
#include <glib-unix.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "bm_common.h"
#include "bm_audio.h"

#define LOG_RING 100

/* BM_PS_DIR lets you point the daemon at a fake battery directory for testing */
static const char *PS_DIR = "/sys/class/power_supply";

typedef enum { ALARM_NONE = 0, ALARM_HIGH, ALARM_SLEEP, ALARM_SHUTDOWN } AlarmKind;

static const char *alarm_name(AlarmKind k) {
    switch (k) {
    case ALARM_HIGH:     return "high";
    case ALARM_SLEEP:    return "sleep";
    case ALARM_SHUTDOWN: return "shutdown";
    default:             return "none";
    }
}

typedef struct {
    BmConfig cfg;
    GMainLoop *loop;
    GDBusConnection *bus;
    guint poll_id, tick_id, resume_check_id;
    int exit_code;

    /* last reading */
    int percent;
    gboolean plugged;
    char status[24];

    gboolean armed_high, armed_sleep, armed_shutdown;

    /* alarm */
    AlarmKind alarm;
    gboolean dry_run;
    gboolean action_pending;
    int seconds_left;
    BmPlayer *player;
    gboolean high_started_on_ac;
    int tick_count;

    GSubprocess *panel;
    gint64 boot_minus_mono;       /* resume detection fallback */

    char *log_ring[LOG_RING];
    int log_next, log_count;
} Daemon;

static Daemon D;

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

static void emit_signal(const char *name, GVariant *params) {
    if (!D.bus) { g_variant_unref(g_variant_ref_sink(params)); return; }
    g_dbus_connection_emit_signal(D.bus, NULL, BM_BUS_PATH, BM_BUS_IFACE, name, params, NULL);
}

static void log_line(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void log_line(const char *fmt, ...) {
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    g_vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char ts[16];
    strftime(ts, sizeof ts, "%H:%M:%S", &tmv);
    char *line = g_strdup_printf("[%s] %s", ts, msg);

    g_free(D.log_ring[D.log_next]);
    D.log_ring[D.log_next] = line;
    D.log_next = (D.log_next + 1) % LOG_RING;
    if (D.log_count < LOG_RING) D.log_count++;

    g_print("%s\n", line);
    emit_signal("Log", g_variant_new("(s)", line));
}

/* ------------------------------------------------------------------ */
/* battery reading (sysfs)                                             */
/* ------------------------------------------------------------------ */

static gboolean read_sysfs(const char *dir, const char *file, char *out, size_t n) {
    char path[512];
    g_snprintf(path, sizeof path, "%s/%s/%s", PS_DIR, dir, file);
    FILE *f = fopen(path, "r");
    if (!f) return FALSE;
    gboolean ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\r\n")] = 0;
    return ok;
}

static gboolean is_device_scope(const char *name) {
    char buf[32];
    return read_sysfs(name, "scope", buf, sizeof buf) && !g_ascii_strcasecmp(buf, "Device");
}

static gboolean read_battery(void) {
    DIR *dp = opendir(PS_DIR);
    if (!dp) return FALSE;

    char bat[64] = "";
    gboolean have_mains = FALSE, mains_online = FALSE;
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (e->d_name[0] == '.') continue;
        char type[32];
        if (!read_sysfs(e->d_name, "type", type, sizeof type)) continue;
        if (is_device_scope(e->d_name)) continue;          /* mice, headsets, ... */
        if (!g_ascii_strcasecmp(type, "Battery")) {
            char cap[16];
            if (!bat[0] && read_sysfs(e->d_name, "capacity", cap, sizeof cap))
                g_strlcpy(bat, e->d_name, sizeof bat);
        } else {                                             /* Mains / USB / USB_PD ... */
            char online[8];
            if (read_sysfs(e->d_name, "online", online, sizeof online)) {
                have_mains = TRUE;
                if (online[0] == '1') mains_online = TRUE;
            }
        }
    }
    closedir(dp);
    if (!bat[0]) return FALSE;

    char cap[16], st[24] = "Unknown";
    if (!read_sysfs(bat, "capacity", cap, sizeof cap)) return FALSE;
    read_sysfs(bat, "status", st, sizeof st);

    D.percent = bm_clamp(atoi(cap), 0, 100);
    g_strlcpy(D.status, st, sizeof D.status);
    if (have_mains)
        D.plugged = mains_online;
    else
        D.plugged = !g_ascii_strcasecmp(st, "Charging") || !g_ascii_strcasecmp(st, "Full") ||
                    !g_ascii_strcasecmp(st, "Not charging");
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* state broadcast                                                     */
/* ------------------------------------------------------------------ */

static GVariant *build_state(void) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "percent",  g_variant_new_int32(D.percent));
    g_variant_builder_add(&b, "{sv}", "plugged",  g_variant_new_boolean(D.plugged));
    g_variant_builder_add(&b, "{sv}", "status",   g_variant_new_string(D.status));
    g_variant_builder_add(&b, "{sv}", "alarm",    g_variant_new_string(alarm_name(D.alarm)));
    g_variant_builder_add(&b, "{sv}", "sound",    g_variant_new_boolean(D.player != NULL));
    g_variant_builder_add(&b, "{sv}", "action",   g_variant_new_boolean(D.action_pending));
    g_variant_builder_add(&b, "{sv}", "seconds",  g_variant_new_int32(D.action_pending ? D.seconds_left : -1));
    g_variant_builder_add(&b, "{sv}", "dry_run",  g_variant_new_boolean(D.dry_run));
    g_variant_builder_add(&b, "{sv}", "poll",     g_variant_new_int32(D.cfg.poll_seconds));
    return g_variant_builder_end(&b);
}

static void emit_state(void) {
    emit_signal("StateChanged", g_variant_new("(@a{sv})", build_state()));
}

/* ------------------------------------------------------------------ */
/* notification + panel                                                */
/* ------------------------------------------------------------------ */

static void notify(const char *summary, const char *body, gboolean urgent) {
    if (!D.bus) return;
    GVariantBuilder hb;
    g_variant_builder_init(&hb, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hb, "{sv}", "urgency", g_variant_new_byte(urgent ? 2 : 1));
    GVariant *params = g_variant_new("(susss@as@a{sv}i)", "Battery Monitor", 0u,
                                     urgent ? "battery-caution" : "battery-full-charged",
                                     summary, body,
                                     g_variant_new_strv(NULL, 0),
                                     g_variant_builder_end(&hb), -1);
    g_dbus_connection_call(D.bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                           "org.freedesktop.Notifications", "Notify", params, NULL,
                           G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
}

static void panel_exited(GObject *src, GAsyncResult *res, gpointer ud) {
    (void)ud;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, NULL);
    g_clear_object(&D.panel);
}

static void spawn_panel(void) {
    if (D.panel) return;                    /* already running; it follows state signals */
    char *self = g_file_read_link("/proc/self/exe", NULL);
    char *dir = self ? g_path_get_dirname(self) : NULL;
    char *cand = dir ? g_build_filename(dir, "battery-monitor-panel", NULL) : NULL;
    const char *exe = (cand && g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE)) ? cand : "battery-monitor-panel";

    GError *err = NULL;
    D.panel = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &err, exe, "--auto", NULL);
    if (D.panel)
        g_subprocess_wait_async(D.panel, NULL, panel_exited, NULL);
    else {
        log_line("Could not start panel (%s): %s", exe, err->message);
        g_error_free(err);
    }
    g_free(cand); g_free(dir); g_free(self);
}

/* ------------------------------------------------------------------ */
/* alarm                                                               */
/* ------------------------------------------------------------------ */

static void stop_sound(void) {
    if (D.player) {
        bm_player_stop(D.player);
        D.player = NULL;
#ifdef __GLIBC__
        malloc_trim(0);           /* give audio buffers back to the OS */
#endif
    }
}

static void end_alarm(void) {
    stop_sound();
    if (D.tick_id) { g_source_remove(D.tick_id); D.tick_id = 0; }
    D.alarm = ALARM_NONE;
    D.dry_run = FALSE;
    D.action_pending = FALSE;
    D.seconds_left = -1;
}

static void run_action(AlarmKind kind) {
    const char *verb = (kind == ALARM_SHUTDOWN) ? "poweroff" : "suspend";
    const char *argv[] = { "systemctl", verb, NULL };
    GError *err = NULL;
    if (!g_spawn_async(NULL, (char **)argv, NULL,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                       NULL, NULL, NULL, &err)) {
        log_line("systemctl %s failed: %s", verb, err->message);
        g_error_free(err);
    }
}

static gboolean tick_cb(gpointer ud) {
    (void)ud;
    if (D.alarm == ALARM_NONE) { D.tick_id = 0; return G_SOURCE_REMOVE; }
    D.tick_count++;

    /* audio thread died on its own (bad file, decode error): reap it, report truth */
    if (D.player && !bm_player_alive(D.player)) {
        log_line("Alarm sound stopped unexpectedly (see terminal output for 'audio:' lines).");
        stop_sound();
    }

    /* every 5 s, re-read the battery so a plugged charger resolves the alarm */
    if (D.tick_count % 5 == 0 || (D.action_pending && D.seconds_left <= 1)) {
        if (read_battery() && !D.dry_run) {
            if ((D.alarm == ALARM_SLEEP || D.alarm == ALARM_SHUTDOWN) && D.plugged) {
                log_line("Charger connected - %s cancelled.",
                         D.alarm == ALARM_SHUTDOWN ? "shutdown" : "sleep");
                end_alarm();
                emit_state();
                return G_SOURCE_REMOVE;
            }
            if (D.alarm == ALARM_HIGH && D.high_started_on_ac && !D.plugged) {
                log_line("Charger unplugged - high alarm cleared.");
                end_alarm();
                emit_state();
                return G_SOURCE_REMOVE;
            }
        }
    }

    if (D.action_pending && --D.seconds_left <= 0) {
        AlarmKind kind = D.alarm;
        gboolean dry = D.dry_run;
        end_alarm();
        if (dry) log_line("[test] countdown finished - would now %s.",
                          kind == ALARM_SHUTDOWN ? "power off" : "suspend");
        else {
            log_line("Grace period over - %s now.", kind == ALARM_SHUTDOWN ? "powering off" : "suspending");
            run_action(kind);
        }
        emit_state();
        return G_SOURCE_REMOVE;
    }
    emit_state();
    return G_SOURCE_CONTINUE;
}

/* returns TRUE if an alarm was (re)started */
static gboolean start_alarm(AlarmKind kind, gboolean dry_run) {
    if (D.alarm != ALARM_NONE && kind <= D.alarm) return FALSE;   /* same/lower severity: keep current */
    end_alarm();

    D.alarm = kind;
    D.dry_run = dry_run;
    D.action_pending = (kind != ALARM_HIGH);
    D.seconds_left = D.action_pending ? D.cfg.grace_seconds : -1;
    D.high_started_on_ac = D.plugged;
    D.tick_count = 0;

    gboolean snd = (kind == ALARM_HIGH) ? D.cfg.sound_high : D.cfg.sound_low;
    const char *path = (kind == ALARM_HIGH) ? D.cfg.sound_high_path : D.cfg.sound_low_path;
    if (snd) {
        D.player = bm_player_start(path);
        if (!D.player) log_line("Alarm sound could not start (%s).", path ? path : "no file set");
    }

    char body[160];
    const char *title;
    switch (kind) {
    case ALARM_HIGH:
        title = "Battery high";
        g_snprintf(body, sizeof body, "Battery reached %d%% (ceiling %d%%). You may want to unplug.",
                   D.percent, D.cfg.high);
        break;
    case ALARM_SLEEP:
        title = "Battery low";
        g_snprintf(body, sizeof body, "Battery at %d%%. Sleeping in %d s - plug in the charger to cancel.",
                   D.percent, D.cfg.grace_seconds);
        break;
    default:
        title = "Battery critical";
        g_snprintf(body, sizeof body, "Battery at %d%%. Powering off in %d s - plug in the charger to cancel.",
                   D.percent, D.cfg.grace_seconds);
    }
    log_line("%s%s: %s", dry_run ? "[test] " : "", title, body);
    notify(title, body, kind != ALARM_HIGH);
    spawn_panel();

    D.tick_id = g_timeout_add_seconds(1, tick_cb, NULL);
    emit_state();
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* threshold evaluation                                                */
/* ------------------------------------------------------------------ */

static void evaluate(void) {
    int p = D.percent;

    /* HIGH: info alarm, no system action */
    if (p >= D.cfg.high) {
        if (D.armed_high && start_alarm(ALARM_HIGH, FALSE)) D.armed_high = FALSE;
    } else if (p <= D.cfg.high - BM_HYSTERESIS) {
        D.armed_high = TRUE;
    }

    if (D.plugged) {                         /* charging: re-arm both low ceilings */
        D.armed_sleep = D.armed_shutdown = TRUE;
        return;
    }

    /* the more severe ceiling wins if the user overlapped them */
    if (p <= D.cfg.shutdown) {
        if (D.armed_shutdown && start_alarm(ALARM_SHUTDOWN, FALSE)) {
            D.armed_shutdown = FALSE;
            D.armed_sleep = FALSE;
        }
    } else if (p >= D.cfg.shutdown + BM_HYSTERESIS) {
        D.armed_shutdown = TRUE;
    }

    if (p > D.cfg.shutdown && p <= D.cfg.sleep_at) {
        if (D.armed_sleep && start_alarm(ALARM_SLEEP, FALSE)) D.armed_sleep = FALSE;
    } else if (p >= D.cfg.sleep_at + BM_HYSTERESIS) {
        D.armed_sleep = TRUE;
    }
}

static gint64 boot_minus_mono(void) {
    struct timespec a, b;
    clock_gettime(CLOCK_BOOTTIME, &a);
    clock_gettime(CLOCK_MONOTONIC, &b);
    return ((gint64)a.tv_sec - b.tv_sec);
}

static void rearm_all(void) { D.armed_high = D.armed_sleep = D.armed_shutdown = TRUE; }

static void check_now(void) {
    /* fallback resume detection: BOOTTIME advances during suspend, MONOTONIC doesn't */
    gint64 bm = boot_minus_mono();
    if (bm - D.boot_minus_mono > 20) {
        log_line("Resume detected - re-arming ceilings.");
        rearm_all();
    }
    D.boot_minus_mono = bm;

    if (!read_battery()) {
        log_line("Could not read battery from %s.", PS_DIR);
        return;
    }
    log_line("Check: %d%% (%s%s)", D.percent, D.status, D.plugged ? ", AC" : "");
    evaluate();
    emit_state();
}

static gboolean poll_cb(gpointer ud) { (void)ud; check_now(); return G_SOURCE_CONTINUE; }

static void restart_poll_timer(void) {
    if (D.poll_id) g_source_remove(D.poll_id);
    D.poll_id = g_timeout_add_seconds((guint)D.cfg.poll_seconds, poll_cb, NULL);
}

static void reload_config(void) {
    bm_config_load(&D.cfg);
    rearm_all();                      /* new thresholds are tested immediately */
    restart_poll_timer();
    log_line("Config reloaded: high=%d sleep=%d shutdown=%d poll=%ds",
             D.cfg.high, D.cfg.sleep_at, D.cfg.shutdown, D.cfg.poll_seconds);
    check_now();
}

/* ------------------------------------------------------------------ */
/* suspend / resume (logind)                                           */
/* ------------------------------------------------------------------ */

static gboolean resume_check_cb(gpointer ud) {
    (void)ud;
    D.resume_check_id = 0;
    check_now();
    return G_SOURCE_REMOVE;
}

static void on_prepare_for_sleep(GDBusConnection *c, const gchar *s, const gchar *o, const gchar *i,
                                 const gchar *sig, GVariant *params, gpointer ud) {
    (void)c; (void)s; (void)o; (void)i; (void)sig; (void)ud;
    gboolean starting = FALSE;
    g_variant_get(params, "(b)", &starting);
    if (starting) {
        if (D.alarm != ALARM_NONE) { end_alarm(); emit_state(); }   /* never carry an alarm into suspend */
        return;
    }
    /* resumed: start fresh. Alarm restarts if still low & unplugged. */
    log_line("System resumed - re-checking battery.");
    end_alarm();
    rearm_all();
    restart_poll_timer();
    if (D.resume_check_id) g_source_remove(D.resume_check_id);
    D.resume_check_id = g_timeout_add_seconds(3, resume_check_cb, NULL);  /* let sysfs/audio settle */
}

static void on_system_bus(GObject *src, GAsyncResult *res, gpointer ud) {
    (void)src; (void)ud;
    GDBusConnection *sys = g_bus_get_finish(res, NULL);
    if (!sys) { log_line("System bus unavailable; resume detection uses clock drift only."); return; }
    g_dbus_connection_signal_subscribe(sys, "org.freedesktop.login1", "org.freedesktop.login1.Manager",
                                       "PrepareForSleep", "/org/freedesktop/login1", NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_prepare_for_sleep, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* D-Bus service                                                       */
/* ------------------------------------------------------------------ */

static void do_silence(void) {
    if (D.alarm == ALARM_NONE) return;
    stop_sound();
    if (D.alarm == ALARM_HIGH) {
        log_line("High alarm turned off.");
        end_alarm();
    } else {
        log_line("Alarm sound turned off (countdown continues).");
    }
    emit_state();
}

static void do_cancel_action(void) {
    if (D.alarm == ALARM_NONE) return;
    log_line("%s cancelled by user.", D.alarm == ALARM_HIGH ? "High alarm" :
             (D.alarm == ALARM_SHUTDOWN ? "Shutdown" : "Sleep"));
    end_alarm();
    emit_state();
}

static void handle_method(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                          const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer ud) {
    (void)c; (void)sender; (void)path; (void)iface; (void)ud;
    if (!g_strcmp0(method, "GetState")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sv})", build_state()));
    } else if (!g_strcmp0(method, "GetLog")) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        int start = (D.log_next - D.log_count + LOG_RING) % LOG_RING;
        for (int i = 0; i < D.log_count; i++)
            g_variant_builder_add(&b, "s", D.log_ring[(start + i) % LOG_RING]);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(as)", &b));
    } else if (!g_strcmp0(method, "Silence")) {
        do_silence();
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!g_strcmp0(method, "CancelAction")) {
        do_cancel_action();
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!g_strcmp0(method, "Reload")) {
        reload_config();
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!g_strcmp0(method, "CheckNow")) {
        check_now();
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!g_strcmp0(method, "Test")) {
        const char *kind = NULL;
        g_variant_get(params, "(&s)", &kind);
        AlarmKind k = !g_strcmp0(kind, "high") ? ALARM_HIGH :
                      !g_strcmp0(kind, "sleep") ? ALARM_SLEEP :
                      !g_strcmp0(kind, "shutdown") ? ALARM_SHUTDOWN : ALARM_NONE;
        if (k == ALARM_NONE)
            g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "unknown test kind");
        else {
            read_battery();
            if (D.alarm != ALARM_NONE) log_line("Test ignored: an alarm is already active.");
            else start_alarm(k, TRUE);
            g_dbus_method_invocation_return_value(inv, NULL);
        }
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method");
    }
}

static const GDBusInterfaceVTable vtable = { handle_method, NULL, NULL, { 0 } };

static void on_bus_acquired(GDBusConnection *conn, const gchar *name, gpointer ud) {
    (void)name; (void)ud;
    D.bus = conn;
    GError *err = NULL;
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(BM_INTROSPECTION_XML, &err);
    if (!info || !g_dbus_connection_register_object(conn, BM_BUS_PATH, info->interfaces[0],
                                                    &vtable, NULL, NULL, &err)) {
        g_printerr("D-Bus registration failed: %s\n", err ? err->message : "?");
        D.exit_code = 1;
        g_main_loop_quit(D.loop);
    }
    if (info) g_dbus_node_info_unref(info);
}

static void on_name_acquired(GDBusConnection *conn, const gchar *name, gpointer ud) {
    (void)conn; (void)name; (void)ud;
    log_line("battery-monitord started (poll every %d s).", D.cfg.poll_seconds);
    D.boot_minus_mono = boot_minus_mono();
    rearm_all();
    restart_poll_timer();
    check_now();
    g_bus_get(G_BUS_TYPE_SYSTEM, NULL, on_system_bus, NULL);
}

static void on_name_lost(GDBusConnection *conn, const gchar *name, gpointer ud) {
    (void)name; (void)ud;
    g_printerr(conn ? "battery-monitord is already running.\n" : "Cannot connect to the session D-Bus.\n");
    D.exit_code = 1;
    g_main_loop_quit(D.loop);
}

static gboolean on_sighup(gpointer ud)  { (void)ud; reload_config(); return G_SOURCE_CONTINUE; }
static gboolean on_sigterm(gpointer ud) { (void)ud; g_main_loop_quit(D.loop); return G_SOURCE_REMOVE; }

int main(void) {
    if (g_getenv("BM_PS_DIR")) PS_DIR = g_getenv("BM_PS_DIR");
    bm_config_load(&D.cfg);
    D.percent = -1;
    g_strlcpy(D.status, "Unknown", sizeof D.status);
    D.seconds_left = -1;

    D.loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGHUP, on_sighup, NULL);
    g_unix_signal_add(SIGTERM, on_sigterm, NULL);
    g_unix_signal_add(SIGINT, on_sigterm, NULL);

    guint owner = g_bus_own_name(G_BUS_TYPE_SESSION, BM_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                 on_bus_acquired, on_name_acquired, on_name_lost, NULL, NULL);
    g_main_loop_run(D.loop);

    end_alarm();
    g_bus_unown_name(owner);
    bm_config_clear(&D.cfg);
    return D.exit_code;
}
