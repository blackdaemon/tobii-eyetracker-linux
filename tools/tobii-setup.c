/*
 * tobii-setup — screen setup and calibration for Tobii IS4/IS5 eye trackers
 * (Eye Tracker 4C / 5) on Linux, using Tobii Stream Engine 3.0.4 and GTK4.
 *
 *   tobii-setup status                       feature group, display area, mounting
 *   tobii-setup display [-W mm -H mm] [-x mm] [-c connector]
 *                                            set display area (default: from EDID)
 *   tobii-setup calibrate [-n 5|9] [-o file] fullscreen calibration + live gaze check
 *   tobii-setup apply [file]                 re-apply a saved calibration
 *   tobii-setup diag [seconds]               tracking quality per second
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtk/gtk.h>

#include <tobii/tobii.h>
#include <tobii/tobii_config.h>
#include <tobii/tobii_licensing.h>
#include <tobii/tobii_streams.h>

/* ------------------------------------------------------------------ device */

static tobii_api_t *g_api;
static tobii_device_t *g_dev;

static void url_cb(char const *url, void *user)
{
    char *buf = user;
    if (!buf[0]) snprintf(buf, 256, "%s", url);
}

static const char *err(tobii_error_t e) { return tobii_error_message(e); }

static bool device_open(void)
{
    char url[256] = "";
    tobii_error_t e = tobii_api_create(&g_api, NULL, NULL);
    if (e) { fprintf(stderr, "tobii_api_create: %s\n", err(e)); return false; }
    e = tobii_enumerate_local_device_urls(g_api, url_cb, url);
    if (e || !url[0]) {
        fprintf(stderr, "no eye tracker found (is tobiiusb.service running?)\n");
        return false;
    }
    e = tobii_device_create(g_api, url, &g_dev);
    if (e) { fprintf(stderr, "tobii_device_create(%s): %s\n", url, err(e)); return false; }
    return true;
}

static void device_close(void)
{
    if (g_dev) tobii_device_destroy(g_dev);
    if (g_api) tobii_api_destroy(g_api);
    g_dev = NULL; g_api = NULL;
}

static const char *feature_group_name(tobii_feature_group_t g)
{
    switch (g) {
    case TOBII_FEATURE_GROUP_BLOCKED: return "blocked";
    case TOBII_FEATURE_GROUP_CONSUMER: return "consumer";
    case TOBII_FEATURE_GROUP_CONFIG: return "config";
    case TOBII_FEATURE_GROUP_PROFESSIONAL: return "professional";
    case TOBII_FEATURE_GROUP_INTERNAL: return "internal";
    }
    return "?";
}

static void print_display_area(const char *label, tobii_display_area_t const *d)
{
    float w = hypotf(d->top_right_mm_xyz[0] - d->top_left_mm_xyz[0],
                     d->top_right_mm_xyz[1] - d->top_left_mm_xyz[1]);
    float h = hypotf(d->bottom_left_mm_xyz[1] - d->top_left_mm_xyz[1],
                     d->bottom_left_mm_xyz[2] - d->top_left_mm_xyz[2]);
    printf("%s %.0f x %.0f mm  TL(%.1f, %.1f, %.1f) TR(%.1f, %.1f, %.1f) BL(%.1f, %.1f, %.1f)\n", label, w, h,
           d->top_left_mm_xyz[0], d->top_left_mm_xyz[1], d->top_left_mm_xyz[2],
           d->top_right_mm_xyz[0], d->top_right_mm_xyz[1], d->top_right_mm_xyz[2],
           d->bottom_left_mm_xyz[0], d->bottom_left_mm_xyz[1], d->bottom_left_mm_xyz[2]);
}

static float display_area_width(tobii_display_area_t const *d)
{
    return fabsf(d->top_right_mm_xyz[0] - d->top_left_mm_xyz[0]);
}

/* -------------------------------------------------------------------- EDID */

/* Physical size of a connected monitor from its EDID. Returns false if none. */
static bool edid_size(const char *want, float *w_mm, float *h_mm, char *name, size_t nlen)
{
    DIR *dir = opendir("/sys/class/drm");
    if (!dir) return false;
    bool found = false;
    for (struct dirent *de; !found && (de = readdir(dir));) {
        if (!strchr(de->d_name, '-') || strstr(de->d_name, "Writeback")) continue;
        /* connector names look like card1-DP-1; accept "DP-1" or the full name */
        const char *conn = strchr(de->d_name, '-') + 1;
        if (want && strcmp(want, conn) && strcmp(want, de->d_name)) continue;

        char path[512], status[32] = "";
        snprintf(path, sizeof path, "/sys/class/drm/%s/status", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(status, sizeof status, f)) status[0] = 0;
        fclose(f);
        if (strncmp(status, "connected", 9)) continue;

        unsigned char e[128];
        snprintf(path, sizeof path, "/sys/class/drm/%s/edid", de->d_name);
        if (!(f = fopen(path, "rb"))) continue;
        size_t n = fread(e, 1, sizeof e, f);
        fclose(f);
        if (n < 128) continue;

        /* First detailed timing descriptor gives mm; header bytes 21/22 give cm. */
        float w = e[66] | ((e[68] & 0xF0) << 4), h = e[67] | ((e[68] & 0x0F) << 8);
        if (w < 10 || h < 10) { w = e[21] * 10.0f; h = e[22] * 10.0f; }
        if (w < 10 || h < 10) continue;
        *w_mm = w; *h_mm = h;
        snprintf(name, nlen, "%s", conn);
        found = true;
    }
    closedir(dir);
    return found;
}

/* ----------------------------------------------------------------- commands */

static int cmd_status(void)
{
    if (!device_open()) return 1;
    tobii_feature_group_t fg;
    tobii_error_t e = tobii_get_feature_group(g_dev, &fg);
    printf("feature group:     %s\n", e ? err(e) : feature_group_name(fg));

    tobii_display_area_t da;
    if (!(e = tobii_get_display_area(g_dev, &da))) {
        print_display_area("display area:     ", &da);
        if (display_area_width(&da) < 50)
            printf("                   ^ placeholder; run `tobii-setup display`\n");
    } else printf("display area:      %s\n", err(e));

    tobii_geometry_mounting_t gm;
    if (!(e = tobii_get_geometry_mounting(g_dev, &gm)))
        printf("mounting:          guides=%d width=%.1f mm angle=%.1f deg ext.offset(%.1f, %.1f, %.1f) int.offset(%.1f, %.1f, %.1f)\n",
               gm.guides, gm.width_mm, gm.angle_deg,
               gm.external_offset_mm_xyz[0], gm.external_offset_mm_xyz[1], gm.external_offset_mm_xyz[2],
               gm.internal_offset_mm_xyz[0], gm.internal_offset_mm_xyz[1], gm.internal_offset_mm_xyz[2]);
    else printf("mounting:          %s\n", err(e));

    float w, h; char conn[64];
    if (edid_size(NULL, &w, &h, conn, sizeof conn))
        printf("monitor (EDID):    %s %.0f x %.0f mm (%.1f\")\n", conn, w, h, hypotf(w, h) / 25.4f);
    device_close();
    return 0;
}

static int set_display(float w, float h, float offx)
{
    tobii_geometry_mounting_t gm;
    tobii_error_t e = tobii_get_geometry_mounting(g_dev, &gm);
    if (e) { fprintf(stderr, "tobii_get_geometry_mounting: %s\n", err(e)); return 1; }
    tobii_display_area_t da;
    e = tobii_calculate_display_area_basic(g_api, w, h, offx, &gm, &da);
    if (e) { fprintf(stderr, "tobii_calculate_display_area_basic: %s\n", err(e)); return 1; }
    print_display_area("new display area:", &da);
    e = tobii_set_display_area(g_dev, &da);
    if (e) {
        fprintf(stderr, "tobii_set_display_area: %s\n", err(e));
        if (e == TOBII_ERROR_INSUFFICIENT_LICENSE)
            fprintf(stderr, "  this device/runtime requires a config-level license for screen setup\n");
        return 1;
    }
    tobii_get_display_area(g_dev, &da);
    print_display_area("readback:        ", &da);
    return 0;
}

static int cmd_display(float w, float h, float offx, const char *conn)
{
    char name[64] = "manual";
    if (w <= 0 || h <= 0) {
        if (!edid_size(conn, &w, &h, name, sizeof name)) {
            fprintf(stderr, "could not read monitor size from EDID; pass -W <mm> -H <mm>\n");
            return 1;
        }
    }
    printf("monitor %s: %.0f x %.0f mm (%.1f\"), tracker offset from centre %.0f mm\n",
           name, w, h, hypotf(w, h) / 25.4f, offx);
    if (w > 700)
        printf("note: ET5 is specified for screens up to ~27\" 16:9 / 30\" 21:9; "
               "accuracy near the side edges of this screen will be reduced\n");
    if (!device_open()) return 1;
    int rc = set_display(w, h, offx);
    device_close();
    return rc;
}

static char *default_calib_path(void)
{
    const char *base = g_get_user_config_dir();
    char *dir = g_build_filename(base, "tobii-eyetracker", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *p = g_build_filename(dir, "calibration.bin", NULL);
    g_free(dir);
    return p;
}

static int cmd_apply(const char *path)
{
    char *def = NULL;
    if (!path) path = def = default_calib_path();
    gchar *data; gsize len; GError *ge = NULL;
    if (!g_file_get_contents(path, &data, &len, &ge)) {
        fprintf(stderr, "%s\n", ge->message);
        g_error_free(ge); g_free(def);
        return 1;
    }
    int rc = 1;
    if (device_open()) {
        tobii_error_t e = tobii_calibration_apply(g_dev, data, len);
        if (e) fprintf(stderr, "tobii_calibration_apply: %s\n", err(e));
        else { printf("applied calibration from %s (%zu bytes)\n", path, (size_t)len); rc = 0; }
    }
    device_close();
    g_free(data); g_free(def);
    return rc;
}

/* --------------------------------------------------------------------- diag */

static struct diag {
    int gaze, gaze_valid, eye, eye_valid, presence;
    float gx0, gx1, gy0, gy1, ez_sum;
} D;

static void diag_gaze(tobii_gaze_point_t const *g, void *u)
{
    (void)u; D.gaze++;
    if (g->validity != TOBII_VALIDITY_VALID) return;
    float x = g->position_xy[0], y = g->position_xy[1];
    if (!D.gaze_valid++) { D.gx0 = D.gx1 = x; D.gy0 = D.gy1 = y; }
    D.gx0 = fminf(D.gx0, x); D.gx1 = fmaxf(D.gx1, x);
    D.gy0 = fminf(D.gy0, y); D.gy1 = fmaxf(D.gy1, y);
}

static void diag_origin(tobii_gaze_origin_t const *o, void *u)
{
    (void)u; D.eye++;
    if (o->left_validity == TOBII_VALIDITY_VALID) { D.eye_valid++; D.ez_sum += o->left_xyz[2]; }
    else if (o->right_validity == TOBII_VALIDITY_VALID) { D.eye_valid++; D.ez_sum += o->right_xyz[2]; }
}

static void diag_presence(tobii_user_presence_status_t s, int64_t t, void *u)
{
    (void)t; (void)u; D.presence = s;
}

static int cmd_diag(int seconds)
{
    if (!device_open()) return 1;
    tobii_gaze_point_subscribe(g_dev, diag_gaze, NULL);
    tobii_gaze_origin_subscribe(g_dev, diag_origin, NULL);
    tobii_user_presence_subscribe(g_dev, diag_presence, NULL);
    printf("Look at the screen and move your eyes around for %d seconds…\n", seconds);
    for (int s = 0; s < seconds; s++) {
        int presence = D.presence;   /* keep last presence state across seconds */
        memset(&D, 0, sizeof D);
        D.presence = presence;
        gint64 end = g_get_monotonic_time() + G_USEC_PER_SEC;
        while (g_get_monotonic_time() < end)
            if (tobii_wait_for_callbacks(1, &g_dev) == TOBII_ERROR_NO_ERROR)
                tobii_device_process_callbacks(g_dev);
        printf("%2ds  presence=%-7s  eyes %3d/%3d valid (dist %4.0f mm)  gaze %3d/%3d valid",
               s + 1, D.presence == 2 ? "present" : D.presence == 1 ? "away" : "unknown",
               D.eye_valid, D.eye, D.eye_valid ? D.ez_sum / D.eye_valid : 0.0f, D.gaze_valid, D.gaze);
        if (D.gaze_valid) printf("  x[%.2f..%.2f] y[%.2f..%.2f]", D.gx0, D.gx1, D.gy0, D.gy1);
        printf("\n");
    }
    tobii_user_presence_unsubscribe(g_dev);
    tobii_gaze_origin_unsubscribe(g_dev);
    tobii_gaze_point_unsubscribe(g_dev);
    device_close();
    return 0;
}

/* -------------------------------------------------------------- calibration */

enum phase { PH_INTRO, PH_POINT, PH_COMPUTE, PH_VALIDATE, PH_ERROR };

static struct {
    GMutex lock;
    GCond cond;
    enum phase phase;
    bool started, stop;
    float tx, ty;          /* current target (normalised) */
    gint64 target_since;   /* monotonic us when target appeared */
    int point, npoints;
    float gx, gy;          /* live gaze */
    gint64 gaze_at;
    char msg[512];
    const char *out_path;
    GtkWidget *area;
} S;

static void set_msg(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void set_msg(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    g_mutex_lock(&S.lock);
    vsnprintf(S.msg, sizeof S.msg, fmt, ap);
    g_mutex_unlock(&S.lock);
    va_end(ap);
    fprintf(stderr, "%s\n", S.msg);
}

static void set_phase(enum phase p)
{
    g_mutex_lock(&S.lock); S.phase = p; g_mutex_unlock(&S.lock);
}

static bool stopped(void)
{
    g_mutex_lock(&S.lock); bool s = S.stop; g_mutex_unlock(&S.lock);
    return s;
}

/* sleep in small steps so Esc stays responsive */
static bool nap_ms(int ms)
{
    for (; ms > 0; ms -= 20) { if (stopped()) return false; g_usleep(20000); }
    return !stopped();
}

static void save_cb(void const *data, size_t size, void *user)
{
    const char *path = user;
    GError *ge = NULL;
    if (g_file_set_contents(path, data, size, &ge))
        fprintf(stderr, "saved calibration (%zu bytes) to %s\n", size, path);
    else { fprintf(stderr, "save failed: %s\n", ge->message); g_error_free(ge); }
}

static void live_gaze_cb(tobii_gaze_point_t const *g, void *user)
{
    (void)user;
    if (g->validity != TOBII_VALIDITY_VALID) return;
    g_mutex_lock(&S.lock);
    S.gx = g->position_xy[0]; S.gy = g->position_xy[1];
    S.gaze_at = g_get_monotonic_time();
    g_mutex_unlock(&S.lock);
}

static gpointer calib_thread(gpointer unused)
{
    (void)unused;
    static const float P9[][2] = { {.5f,.5f}, {.1f,.1f}, {.5f,.1f}, {.9f,.1f}, {.9f,.5f},
                                   {.9f,.9f}, {.5f,.9f}, {.1f,.9f}, {.1f,.5f} };
    static const float P5[][2] = { {.5f,.5f}, {.1f,.1f}, {.9f,.1f}, {.9f,.9f}, {.1f,.9f} };
    const float (*pts)[2] = S.npoints == 5 ? P5 : P9;
    tobii_error_t e;
    bool calibrating = false;

    if (!device_open()) { set_msg("No eye tracker found. Is tobiiusb.service running?"); goto fail; }

    tobii_display_area_t da;
    if (!tobii_get_display_area(g_dev, &da) && display_area_width(&da) < 50) {
        float w, h; char conn[64];
        if (!edid_size(NULL, &w, &h, conn, sizeof conn) || set_display(w, h, 0)) {
            set_msg("Display area is not configured and could not be set.\nRun: tobii-setup display -W <mm> -H <mm>");
            goto fail;
        }
    }

    /* wait for Space */
    g_mutex_lock(&S.lock);
    while (!S.started && !S.stop) g_cond_wait(&S.cond, &S.lock);
    g_mutex_unlock(&S.lock);
    if (stopped()) goto out;

    tobii_calibration_stop(g_dev); /* in case a previous run was left open */
    if ((e = tobii_calibration_start(g_dev, TOBII_ENABLED_EYE_BOTH))) {
        set_msg("tobii_calibration_start: %s%s", err(e),
                e == TOBII_ERROR_INSUFFICIENT_LICENSE ? "\nThis runtime requires a config license for calibration." : "");
        goto fail;
    }
    calibrating = true;

    for (int i = 0; i < S.npoints; i++) {
        g_mutex_lock(&S.lock);
        S.phase = PH_POINT; S.point = i; S.tx = pts[i][0]; S.ty = pts[i][1];
        S.target_since = g_get_monotonic_time();
        g_mutex_unlock(&S.lock);
        if (!nap_ms(1100)) goto out;          /* dot shrinks, eyes settle */
        e = tobii_calibration_collect_data_2d(g_dev, pts[i][0], pts[i][1]);
        if (e == TOBII_ERROR_OPERATION_FAILED) {
            fprintf(stderr, "point %d: retrying\n", i + 1);
            e = tobii_calibration_collect_data_2d(g_dev, pts[i][0], pts[i][1]);
        }
        if (e) fprintf(stderr, "point %d (%.1f, %.1f): %s (continuing)\n", i + 1, pts[i][0], pts[i][1], err(e));
    }

    set_phase(PH_COMPUTE);
    set_msg("Computing calibration…");
    e = tobii_calibration_compute_and_apply(g_dev);
    tobii_calibration_stop(g_dev);
    calibrating = false;
    if (e) { set_msg("tobii_calibration_compute_and_apply: %s\nPress Esc and try again.", err(e)); goto fail; }
    tobii_calibration_retrieve(g_dev, save_cb, (void *)S.out_path);

    set_msg("Calibration applied. The red dot shows your gaze — look around to check accuracy.\nEsc to finish, R to recalibrate.");
    set_phase(PH_VALIDATE);
    if ((e = tobii_gaze_point_subscribe(g_dev, live_gaze_cb, NULL))) {
        set_msg("gaze subscribe: %s", err(e));
        goto fail;
    }
    while (!stopped()) {
        e = tobii_wait_for_callbacks(1, &g_dev);
        if (e == TOBII_ERROR_NO_ERROR) e = tobii_device_process_callbacks(g_dev);
        if (e == TOBII_ERROR_CONNECTION_FAILED) tobii_device_reconnect(g_dev);
    }
    tobii_gaze_point_unsubscribe(g_dev);
    goto out;

fail:
    set_phase(PH_ERROR);
out:
    if (calibrating) tobii_calibration_stop(g_dev);
    device_close();
    return NULL;
}

static void draw(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer u)
{
    (void)a; (void)u;
    g_mutex_lock(&S.lock);
    enum phase ph = S.phase;
    float tx = S.tx * w, ty = S.ty * h, gx = S.gx * w, gy = S.gy * h;
    gint64 since = S.target_since, gat = S.gaze_at, now = g_get_monotonic_time();
    int point = S.point, np = S.npoints;
    char msg[512]; snprintf(msg, sizeof msg, "%s", S.msg);
    g_mutex_unlock(&S.lock);

    cairo_set_source_rgb(cr, 0.12, 0.12, 0.14);
    cairo_paint(cr);

    const char *text = msg;
    if (ph == PH_INTRO)
        text = "Tobii calibration\n\nSit in your normal position and follow each dot with your eyes.\n"
               "Keep your head still.\n\nSpace: start      Esc: cancel";

    if (ph == PH_POINT) {
        double t = (now - since) / 1e6;
        double r = 28 - 20 * fmin(t / 0.9, 1.0);   /* shrink to a 8px dot */
        cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
        cairo_arc(cr, tx, ty, r, 0, 2 * G_PI); cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.9, 0.2, 0.2);
        cairo_arc(cr, tx, ty, 3, 0, 2 * G_PI); cairo_fill(cr);
        char buf[64]; snprintf(buf, sizeof buf, "%d / %d", point + 1, np);
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_set_font_size(cr, 16);
        cairo_move_to(cr, 20, h - 20); cairo_show_text(cr, buf);
        return;
    }

    if (ph == PH_VALIDATE) {
        static const float grid[][2] = { {.1f,.1f}, {.5f,.1f}, {.9f,.1f}, {.1f,.5f}, {.5f,.5f},
                                         {.9f,.5f}, {.1f,.9f}, {.5f,.9f}, {.9f,.9f} };
        cairo_set_source_rgb(cr, 0.4, 0.4, 0.45);
        for (unsigned i = 0; i < G_N_ELEMENTS(grid); i++) {
            cairo_arc(cr, grid[i][0] * w, grid[i][1] * h, 6, 0, 2 * G_PI); cairo_fill(cr);
        }
        if (now - gat < 200000) {
            cairo_set_source_rgba(cr, 0.95, 0.2, 0.2, 0.8);
            cairo_arc(cr, gx, gy, 14, 0, 2 * G_PI); cairo_fill(cr);
        }
    }

    /* centred multi-line message */
    cairo_set_source_rgb(cr, ph == PH_ERROR ? 1.0 : 0.85, ph == PH_ERROR ? 0.5 : 0.85, ph == PH_ERROR ? 0.5 : 0.85);
    cairo_set_font_size(cr, 22);
    char *lines = g_strdup(text), *save = NULL;
    int n = 0;
    for (char *l = lines; *l; l++) if (*l == '\n') n++;
    double y = h / 2.0 - (n + 1) * 15 + (ph == PH_VALIDATE ? h * 0.2 : 0);
    for (char *l = strtok_r(lines, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        cairo_text_extents_t ex; cairo_text_extents(cr, l, &ex);
        cairo_move_to(cr, (w - ex.width) / 2, y); cairo_show_text(cr, l);
        y += 34;
    }
    g_free(lines);
}

static gboolean tick(GtkWidget *w, GdkFrameClock *c, gpointer u)
{
    (void)c; (void)u;
    gtk_widget_queue_draw(w);
    return G_SOURCE_CONTINUE;
}

static GThread *g_worker;
static bool g_restart;

static void request_stop(void)
{
    g_mutex_lock(&S.lock); S.stop = true; g_cond_broadcast(&S.cond); g_mutex_unlock(&S.lock);
}

static gboolean on_key(GtkEventControllerKey *k, guint keyval, guint code, GdkModifierType m, gpointer win)
{
    (void)k; (void)code; (void)m;
    if (keyval == GDK_KEY_Escape || keyval == GDK_KEY_q) {
        request_stop();
        gtk_window_destroy(GTK_WINDOW(win));
    } else if (keyval == GDK_KEY_space) {
        g_mutex_lock(&S.lock); S.started = true; g_cond_broadcast(&S.cond); g_mutex_unlock(&S.lock);
    } else if (keyval == GDK_KEY_r || keyval == GDK_KEY_R) {
        g_mutex_lock(&S.lock); bool can = S.phase == PH_VALIDATE || S.phase == PH_ERROR; g_mutex_unlock(&S.lock);
        if (can) { g_restart = true; request_stop(); gtk_window_destroy(GTK_WINDOW(win)); }
    }
    return TRUE;
}

static int cmd_calibrate(int npoints, const char *out)
{
    char *def = NULL;
    if (!out) out = def = default_calib_path();
    gtk_init();
    do {
        g_restart = false;
        g_mutex_lock(&S.lock);
        S.phase = PH_INTRO; S.started = S.stop = false; S.msg[0] = 0;
        S.npoints = npoints; S.out_path = out; S.gaze_at = 0;
        g_mutex_unlock(&S.lock);

        GtkWidget *win = gtk_window_new();
        gtk_window_set_title(GTK_WINDOW(win), "Tobii calibration");
        GtkWidget *area = gtk_drawing_area_new();
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw, NULL, NULL);
        gtk_widget_add_tick_callback(area, tick, NULL, NULL);
        gtk_window_set_child(GTK_WINDOW(win), area);
        GtkEventController *kc = gtk_event_controller_key_new();
        g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), win);
        gtk_widget_add_controller(win, kc);
        gtk_window_fullscreen(GTK_WINDOW(win));
        gtk_window_present(GTK_WINDOW(win));

        g_worker = g_thread_new("tobii", calib_thread, NULL);
        while (g_list_model_get_n_items(gtk_window_get_toplevels()) > 0)
            g_main_context_iteration(NULL, TRUE);
        request_stop();
        g_thread_join(g_worker);
    } while (g_restart);
    g_free(def);
    return 0;
}

/* -------------------------------------------------------------------- main */

static void usage(void)
{
    fprintf(stderr,
        "usage: tobii-setup <command> [options]\n\n"
        "  status                         show license level, display area, mounting, monitor size\n"
        "  display [-W mm -H mm] [-x mm] [-c connector]\n"
        "                                 set screen geometry (default: size from EDID)\n"
        "                                 -x: tracker offset from screen centre, + = right\n"
        "  calibrate [-n 5|9] [-o file]   fullscreen calibration, then live gaze check\n"
        "                                 (saves to ~/.config/tobii-eyetracker/calibration.bin)\n"
        "  apply [file]                   re-apply a saved calibration (e.g. after replug)\n"
        "  diag [seconds]                 per-second tracking quality (eyes found, gaze validity/range)\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];
    argc--; argv++;

    float w = 0, h = 0, offx = 0;
    int npoints = 9;
    const char *conn = NULL, *out = NULL;
    for (int ch; (ch = getopt(argc, argv, "W:H:x:c:n:o:h")) != -1;) {
        switch (ch) {
        case 'W': w = strtof(optarg, NULL); break;
        case 'H': h = strtof(optarg, NULL); break;
        case 'x': offx = strtof(optarg, NULL); break;
        case 'c': conn = optarg; break;
        case 'n': npoints = atoi(optarg) == 5 ? 5 : 9; break;
        case 'o': out = optarg; break;
        default: usage(); return 2;
        }
    }

    if (!strcmp(cmd, "status")) return cmd_status();
    if (!strcmp(cmd, "display")) return cmd_display(w, h, offx, conn);
    if (!strcmp(cmd, "calibrate")) return cmd_calibrate(npoints, out);
    if (!strcmp(cmd, "diag")) return cmd_diag(optind < argc ? atoi(argv[optind]) : 10);
    if (!strcmp(cmd, "apply")) return cmd_apply(optind < argc ? argv[optind] : NULL);
    usage();
    return 2;
}
