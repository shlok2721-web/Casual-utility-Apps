/*
 * bm_common.h - shared by the daemon (battery-monitord) and the panel
 * (battery-monitor-panel): config file handling, limits, D-Bus interface.
 */
#ifndef BM_COMMON_H
#define BM_COMMON_H

#include <glib.h>
#include <glib/gstdio.h>

/* ---- limits ---- */
#define BM_POLL_MIN        60
#define BM_POLL_MAX      1200
#define BM_POLL_DEFAULT   300
#define BM_GRACE_MIN       10
#define BM_GRACE_MAX      600
#define BM_GRACE_DEFAULT   60      /* seconds of alarm before sleep/poweroff */
#define BM_HYSTERESIS       3      /* % recovery needed to re-arm a ceiling  */

#define BM_DEFAULT_HIGH      90
#define BM_DEFAULT_SLEEP     15
#define BM_DEFAULT_SHUTDOWN   5

/* ---- D-Bus (session bus) ---- */
#define BM_BUS_NAME  "com.local.BatteryMonitor"
#define BM_BUS_PATH  "/com/local/BatteryMonitor"
#define BM_BUS_IFACE "com.local.BatteryMonitor"

#define BM_INTROSPECTION_XML \
"<node>" \
"  <interface name='" BM_BUS_IFACE "'>" \
"    <method name='GetState'><arg type='a{sv}' direction='out'/></method>" \
"    <method name='GetLog'><arg type='as' direction='out'/></method>" \
"    <method name='Silence'/>" \
"    <method name='CancelAction'/>" \
"    <method name='Reload'/>" \
"    <method name='CheckNow'/>" \
"    <method name='Test'><arg type='s' direction='in'/></method>" \
"    <signal name='StateChanged'><arg type='a{sv}'/></signal>" \
"    <signal name='Log'><arg type='s'/></signal>" \
"  </interface>" \
"</node>"

/* ---- config ---- */
typedef struct {
    int      high;            /* 0..100  alarm when battery >= high            */
    int      sleep_at;        /* 0..100  suspend when battery <= sleep_at      */
    int      shutdown;        /* 0..100  power off when battery <= shutdown    */
    int      poll_seconds;    /* 60..1200                                      */
    int      grace_seconds;   /* alarm time before sleep/poweroff              */
    gboolean sound_high;
    gboolean sound_low;
    gchar   *sound_high_path;
    gchar   *sound_low_path;
} BmConfig;

static inline int bm_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline gchar *bm_config_path(void) {
    gchar *dir = g_build_filename(g_get_user_config_dir(), "battery-monitor", NULL);
    g_mkdir_with_parents(dir, 0700);
    gchar *path = g_build_filename(dir, "config.conf", NULL);
    g_free(dir);
    return path;
}

static inline void bm_config_clear(BmConfig *c) {
    g_clear_pointer(&c->sound_high_path, g_free);
    g_clear_pointer(&c->sound_low_path, g_free);
}

static inline void bm_config_defaults(BmConfig *c) {
    bm_config_clear(c);
    c->high = BM_DEFAULT_HIGH;
    c->sleep_at = BM_DEFAULT_SLEEP;
    c->shutdown = BM_DEFAULT_SHUTDOWN;
    c->poll_seconds = BM_POLL_DEFAULT;
    c->grace_seconds = BM_GRACE_DEFAULT;
    c->sound_high = TRUE;
    c->sound_low = TRUE;
    c->sound_high_path = g_build_filename(g_get_home_dir(), "Music",
                                          "snorcon-high-battery-charge-421821.mp3", NULL);
    c->sound_low_path  = g_build_filename(g_get_home_dir(), "Music",
                                          "snorcon-low-battery-charge-421814.mp3", NULL);
}

static inline void bm_kf_int(GKeyFile *kf, const char *g, const char *k, int *out, int lo, int hi) {
    GError *e = NULL;
    int v = g_key_file_get_integer(kf, g, k, &e);
    if (e) g_error_free(e); else *out = bm_clamp(v, lo, hi);
}
static inline void bm_kf_bool(GKeyFile *kf, const char *g, const char *k, gboolean *out) {
    GError *e = NULL;
    gboolean v = g_key_file_get_boolean(kf, g, k, &e);
    if (e) g_error_free(e); else *out = v;
}
static inline void bm_kf_str(GKeyFile *kf, const char *g, const char *k, gchar **out) {
    gchar *v = g_key_file_get_string(kf, g, k, NULL);
    if (v && *v) { g_free(*out); *out = v; } else g_free(v);
}

/* Loads config (missing keys keep defaults). Always leaves *c valid. */
static inline void bm_config_load(BmConfig *c) {
    bm_config_defaults(c);
    gchar *path = bm_config_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        bm_kf_int(kf, "thresholds", "high",     &c->high,         0, 100);
        bm_kf_int(kf, "thresholds", "sleep",    &c->sleep_at,     0, 100);
        bm_kf_int(kf, "thresholds", "shutdown", &c->shutdown,     0, 100);
        bm_kf_int(kf, "monitor", "poll_seconds",  &c->poll_seconds,  BM_POLL_MIN,  BM_POLL_MAX);
        bm_kf_int(kf, "monitor", "grace_seconds", &c->grace_seconds, BM_GRACE_MIN, BM_GRACE_MAX);
        bm_kf_bool(kf, "sound", "high_enabled", &c->sound_high);
        bm_kf_bool(kf, "sound", "low_enabled",  &c->sound_low);
        bm_kf_str(kf, "sound", "high_file", &c->sound_high_path);
        bm_kf_str(kf, "sound", "low_file",  &c->sound_low_path);
    }
    g_key_file_free(kf);
    g_free(path);
}

static inline gboolean bm_config_save(const BmConfig *c) {
    gchar *path = bm_config_path();
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_integer(kf, "thresholds", "high",     c->high);
    g_key_file_set_integer(kf, "thresholds", "sleep",    c->sleep_at);
    g_key_file_set_integer(kf, "thresholds", "shutdown", c->shutdown);
    g_key_file_set_integer(kf, "monitor", "poll_seconds",  c->poll_seconds);
    g_key_file_set_integer(kf, "monitor", "grace_seconds", c->grace_seconds);
    g_key_file_set_boolean(kf, "sound", "high_enabled", c->sound_high);
    g_key_file_set_boolean(kf, "sound", "low_enabled",  c->sound_low);
    g_key_file_set_string(kf, "sound", "high_file", c->sound_high_path ? c->sound_high_path : "");
    g_key_file_set_string(kf, "sound", "low_file",  c->sound_low_path ? c->sound_low_path : "");
    GError *err = NULL;
    gboolean ok = g_key_file_save_to_file(kf, path, &err);
    if (!ok) { g_warning("Could not save config: %s", err->message); g_error_free(err); }
    g_key_file_free(kf);
    g_free(path);
    return ok;
}

#endif
