/*
 * tobii-gaze — minimal Linux client for Tobii IS4/IS5 eye trackers
 * (Eye Tracker 4C, Eye Tracker 5) built on Tobii Stream Engine 3.0.4.
 *
 *   tobii-gaze --info         print device information and exit
 *   tobii-gaze [--head]       stream gaze points (and head pose) to stdout
 *   tobii-gaze --pointer      drive a virtual absolute pointer via /dev/uinput
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <tobii/tobii.h>
#include <tobii/tobii_config.h>
#include <tobii/tobii_streams.h>

#define ABS_MAXVAL 65535

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

struct opts {
    bool info, head, pointer, quiet;
    float smooth;     /* EMA factor: 0 = no smoothing, 0.9 = heavy */
    const char *url;
};

struct ctx {
    struct opts *o;
    int ufd;          /* uinput fd, -1 if unused */
    bool have_prev;
    float sx, sy;     /* smoothed position */
};

/* ---------- uinput virtual absolute pointer (works on Wayland & X11) ---------- */

static int uinput_open(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "cannot open /dev/uinput: %s\n"
                "  (add yourself to a group that owns /dev/uinput, or run with sudo)\n",
                strerror(errno));
        return -1;
    }
    /* Shaped like a QEMU USB tablet: libinput treats it as an absolute pointer. */
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    struct uinput_abs_setup ax = { .code = ABS_X, .absinfo = { .minimum = 0, .maximum = ABS_MAXVAL } };
    struct uinput_abs_setup ay = { .code = ABS_Y, .absinfo = { .minimum = 0, .maximum = ABS_MAXVAL } };
    struct uinput_setup us = { .id = { .bustype = BUS_VIRTUAL, .vendor = 0x2104, .product = 0x0001, .version = 1 } };
    snprintf(us.name, sizeof us.name, "Tobii Gaze Pointer");

    if (ioctl(fd, UI_ABS_SETUP, &ax) < 0 || ioctl(fd, UI_ABS_SETUP, &ay) < 0 ||
        ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "uinput setup failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void uinput_emit(int fd, int type, int code, int value)
{
    struct input_event ev = { .type = type, .code = code, .value = value };
    if (write(fd, &ev, sizeof ev) < 0 && errno != EAGAIN)
        perror("uinput write");
}

static void uinput_move(int fd, float x, float y)
{
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    y = y < 0 ? 0 : y > 1 ? 1 : y;
    uinput_emit(fd, EV_ABS, ABS_X, (int)(x * ABS_MAXVAL + 0.5f));
    uinput_emit(fd, EV_ABS, ABS_Y, (int)(y * ABS_MAXVAL + 0.5f));
    uinput_emit(fd, EV_SYN, SYN_REPORT, 0);
}

/* ---------- Stream Engine callbacks ---------- */

static void gaze_cb(tobii_gaze_point_t const *g, void *user)
{
    struct ctx *c = user;
    if (g->validity != TOBII_VALIDITY_VALID) {
        if (!c->o->quiet && !c->o->pointer)
            printf("%lld gaze invalid\n", (long long)g->timestamp_us);
        return;
    }
    float x = g->position_xy[0], y = g->position_xy[1];
    if (c->have_prev && c->o->smooth > 0) {
        x = c->o->smooth * c->sx + (1 - c->o->smooth) * x;
        y = c->o->smooth * c->sy + (1 - c->o->smooth) * y;
    }
    c->sx = x; c->sy = y; c->have_prev = true;

    if (c->ufd >= 0)
        uinput_move(c->ufd, x, y);
    if (!c->o->quiet)
        printf("%lld gaze %.4f %.4f\n", (long long)g->timestamp_us, x, y);
}

static void head_cb(tobii_head_pose_t const *h, void *user)
{
    struct ctx *c = user;
    if (c->o->quiet) return;
    if (h->position_validity == TOBII_VALIDITY_VALID)
        printf("%lld head pos %.1f %.1f %.1f mm", (long long)h->timestamp_us,
               h->position_xyz[0], h->position_xyz[1], h->position_xyz[2]);
    else
        printf("%lld head pos invalid", (long long)h->timestamp_us);
    printf(" rot");
    for (int i = 0; i < 3; i++) {
        if (h->rotation_validity_xyz[i] == TOBII_VALIDITY_VALID) printf(" %.3f", h->rotation_xyz[i]);
        else printf(" -");
    }
    printf("\n");
}

static void url_cb(char const *url, void *user)
{
    char *buf = user;
    if (buf[0] == '\0') snprintf(buf, 256, "%s", url);
    fprintf(stderr, "found device: %s\n", url);
}

/* ---------- main ---------- */

#define CHECK(expr) do { tobii_error_t e_ = (expr); if (e_ != TOBII_ERROR_NO_ERROR) { \
    fprintf(stderr, "%s failed: %s\n", #expr, tobii_error_message(e_)); goto out; } } while (0)

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  -i, --info         print device information and exit\n"
        "  -H, --head         also stream head pose\n"
        "  -p, --pointer      move a virtual absolute pointer with your gaze (uinput)\n"
        "  -s, --smooth F     exponential smoothing factor 0..0.99 (default 0, pointer: 0.6)\n"
        "  -q, --quiet        do not print samples\n"
        "  -u, --url URL      device URL (default: first enumerated device)\n", argv0);
}

int main(int argc, char **argv)
{
    struct opts o = { .smooth = -1 };
    static const struct option lo[] = {
        { "info", no_argument, 0, 'i' }, { "head", no_argument, 0, 'H' },
        { "pointer", no_argument, 0, 'p' }, { "smooth", required_argument, 0, 's' },
        { "quiet", no_argument, 0, 'q' }, { "url", required_argument, 0, 'u' },
        { "help", no_argument, 0, 'h' }, { 0 } };
    for (int ch; (ch = getopt_long(argc, argv, "iHps:qu:h", lo, NULL)) != -1;) {
        switch (ch) {
        case 'i': o.info = true; break;
        case 'H': o.head = true; break;
        case 'p': o.pointer = true; break;
        case 's': o.smooth = strtof(optarg, NULL); break;
        case 'q': o.quiet = true; break;
        case 'u': o.url = optarg; break;
        default: usage(argv[0]); return ch == 'h' ? 0 : 2;
        }
    }
    if (o.smooth < 0) o.smooth = o.pointer ? 0.6f : 0.0f;
    if (o.smooth > 0.99f) o.smooth = 0.99f;
    setvbuf(stdout, NULL, _IOLBF, 0);

    int rc = 1;
    tobii_api_t *api = NULL;
    tobii_device_t *dev = NULL;
    struct ctx c = { .o = &o, .ufd = -1 };
    char url[256] = "";

    tobii_version_t ver;
    if (tobii_get_api_version(&ver) == TOBII_ERROR_NO_ERROR)
        fprintf(stderr, "Stream Engine %d.%d.%d.%d\n", ver.major, ver.minor, ver.revision, ver.build);

    CHECK(tobii_api_create(&api, NULL, NULL));
    if (o.url) {
        snprintf(url, sizeof url, "%s", o.url);
    } else {
        CHECK(tobii_enumerate_local_device_urls(api, url_cb, url));
        if (!url[0]) {
            fprintf(stderr, "no eye tracker found — is tobiiusb.service running? "
                            "(systemctl status tobiiusb)\n");
            goto out;
        }
    }
    CHECK(tobii_device_create(api, url, &dev));

    tobii_device_info_t di;
    if (tobii_get_device_info(dev, &di) == TOBII_ERROR_NO_ERROR) {
        fprintf(stderr, "model %s | generation %s | serial %s | firmware %s | integration %s\n",
                di.model, di.generation, di.serial_number, di.firmware_version, di.integration_type);
        if (o.info) {
            printf("url:                  %s\nmodel:                %s\ngeneration:           %s\n"
                   "serial:               %s\nfirmware:             %s\nintegration id:       %s\n"
                   "integration type:     %s\nhw calibration:       %s (%s)\nlot id:               %s\n"
                   "runtime build:        %s\n",
                   url, di.model, di.generation, di.serial_number, di.firmware_version, di.integration_id,
                   di.integration_type, di.hw_calibration_version, di.hw_calibration_date, di.lot_id,
                   di.runtime_build_version);
            float freq;
            if (tobii_get_output_frequency(dev, &freq) == TOBII_ERROR_NO_ERROR)
                printf("output frequency:     %.0f Hz\n", freq);
            tobii_display_area_t da;
            if (tobii_get_display_area(dev, &da) == TOBII_ERROR_NO_ERROR)
                printf("display area (mm):    TL(%.0f,%.0f,%.0f) TR(%.0f,%.0f,%.0f) BL(%.0f,%.0f,%.0f)\n",
                       da.top_left_mm_xyz[0], da.top_left_mm_xyz[1], da.top_left_mm_xyz[2],
                       da.top_right_mm_xyz[0], da.top_right_mm_xyz[1], da.top_right_mm_xyz[2],
                       da.bottom_left_mm_xyz[0], da.bottom_left_mm_xyz[1], da.bottom_left_mm_xyz[2]);
            rc = 0;
            goto out;
        }
    }

    if (o.pointer && (c.ufd = uinput_open()) < 0)
        goto out;

    CHECK(tobii_gaze_point_subscribe(dev, gaze_cb, &c));
    if (o.head) {
        tobii_error_t e = tobii_head_pose_subscribe(dev, head_cb, &c);
        if (e != TOBII_ERROR_NO_ERROR)
            fprintf(stderr, "head pose not available: %s\n", tobii_error_message(e));
    }

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    fprintf(stderr, "streaming%s — Ctrl+C to stop\n", o.pointer ? " (gaze pointer active)" : "");

    while (!g_stop) {
        tobii_error_t e = tobii_wait_for_callbacks(1, &dev);
        if (e == TOBII_ERROR_TIMED_OUT) continue;
        if (e == TOBII_ERROR_NO_ERROR) e = tobii_device_process_callbacks(dev);
        if (e == TOBII_ERROR_CONNECTION_FAILED) {
            fprintf(stderr, "connection lost, reconnecting...\n");
            while (!g_stop && tobii_device_reconnect(dev) != TOBII_ERROR_NO_ERROR)
                sleep(1);
            continue;
        }
        if (e != TOBII_ERROR_NO_ERROR && !g_stop) {
            fprintf(stderr, "stream error: %s\n", tobii_error_message(e));
            goto out;
        }
    }
    rc = 0;

out:
    if (dev) {
        if (o.head) tobii_head_pose_unsubscribe(dev);
        tobii_gaze_point_unsubscribe(dev);
        tobii_device_destroy(dev);
    }
    if (api) tobii_api_destroy(api);
    if (c.ufd >= 0) { ioctl(c.ufd, UI_DEV_DESTROY); close(c.ufd); }
    return rc;
}
