/*
 * Second-screen panel host for Linux dual-screen handhelds (ROCKNIX on the
 * Anbernic RG DS / RG DS Plus). The Android shell's job, done with SDL:
 *
 *   Android                               Linux (this file)
 *   ------------------------------------  ------------------------------------
 *   Presentation on the secondary         SDL window, fullscreen on the other
 *   display (SecondScreenManager.java)    output (SDL display != the game's)
 *   SurfaceView + ANativeWindow_lock      SDL_Renderer streaming texture
 *   20 Hz native render thread            20 Hz paint thread (same cadence)
 *   SurfaceView.onTouchEvent -> nativeTap evdev reader on the panel's own
 *                                         touchscreen (or SDL touch/mouse
 *                                         events on the panel window)
 *
 * The compositor (Port_SecondScreen_PaintInto) and tap handler
 * (Port_SecondScreen_OnTap) are the shared ones in port_second_screen.c —
 * nothing about the panel's look or behavior is re-implemented here.
 *
 * Threads:
 *   - main thread: owns every SDL call. Port_SecondScreen_LinuxPump() runs
 *     once per game frame from Port_PumpEvents; it creates the window on
 *     first use, uploads the most recent finished frame and presents it.
 *   - paint thread: reads the published game snapshot and paints into a
 *     private buffer, then swaps it into the "ready" slot. Painting a
 *     1024x768 panel is real CPU work on a Cortex-A55, which is why it does
 *     not happen on the game thread.
 *   - touch thread: blocking read() on the touchscreen's evdev node; a
 *     completed tap goes straight to Port_SecondScreen_OnTap, which already
 *     takes its own lock (it is called from the Java UI thread on Android).
 *
 * Why evdev rather than SDL touch events: on ROCKNIX the bottom panel's
 * touchscreen is calibrated by udev for a side-by-side span of both outputs
 * and its sway helper maps the touch device to the *focused* output, so the
 * coordinates SDL would receive depend on compositor state this game cannot
 * control. Reading the device directly (and grabbing it, so taps on the
 * panel never also reach the game window) gives panel-pixel coordinates
 * no matter how sway is configured. SDL touch/mouse on the panel window is
 * still honored as a fallback and for desktop testing.
 *
 * Environment knobs (all optional; the ROCKNIX launcher sets what it needs):
 *   TMC_PANEL=0|1           0 disables the panel. 1 forces a panel window
 *                           even with a single display (desktop testing).
 *   TMC_PANEL_DISPLAY=x     SDL display for the panel: an index, or a
 *                           substring of the display name. Default: the
 *                           first display that isn't the game window's.
 *   TMC_PANEL_WINDOWED=1    don't make the panel fullscreen.
 *   TMC_PANEL_SCALE=n       paint at 1/n of the panel's pixel size and let
 *                           the GPU scale up (nearest). Default 1.
 *   TMC_PANEL_UPLOAD_ROWS=n rows of the panel texture uploaded per game
 *                           frame (only changed rows are sent at all).
 *                           Default: a third of the panel, so one panel
 *                           frame spreads over the three game frames it
 *                           spans at 20 Hz / 60 Hz.
 *   TMC_PANEL_NICE=n        paint thread niceness. Default 10 (yields to
 *                           the game's PPU workers and audio).
 *   TMC_PANEL_TOUCH=auto|sdl|/dev/input/eventN
 *                           auto (default): find the touchscreen whose
 *                           sysfs path contains TMC_PANEL_TOUCH_MATCH.
 *                           sdl: use SDL events on the panel window only.
 *   TMC_PANEL_TOUCH_MATCH=s sysfs substring for auto. Default
 *                           "fe5e0000.i2c" (the RG DS family's bottom panel).
 *   TMC_PANEL_TOUCH_ROTATE=0|90|180|270, TMC_PANEL_TOUCH_FLIPX=1,
 *   TMC_PANEL_TOUCH_FLIPY=1 coordinate fixes for an oddly mounted panel.
 *   TMC_PANEL_TOUCH_GRAB=0  don't take the touchscreen exclusively.
 */

/* realpath, O_CLOEXEC, clock_gettime under -std=c11. Must precede every include. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "port_second_screen.h"
#include "port_second_screen_platform.h"

#if defined(PORT_SS_LINUX)

#include <SDL3/SDL.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define PANEL_PAINT_HZ 20 /* the compositor's animations assume this cadence */
#define PANEL_LONG_PRESS_MS 350 /* SecondScreenView.LONG_PRESS_MS */
#define PANEL_TITLE "Minish Cap Panel"

/* ------------------------------------------------------------------ */
/*  Frame buffers: paint thread -> ready slot -> main-thread upload     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t* px;
    int w, h;
    size_t cap; /* pixels allocated */
} PanelBuf;

static pthread_mutex_t sBufMutex = PTHREAD_MUTEX_INITIALIZER;
static PanelBuf sBack;   /* paint thread only */
static PanelBuf sPrev;   /* paint thread only: copy of the last painted frame, for the diff */
static PanelBuf sReady;  /* shared, under sBufMutex */
static PanelBuf sUpload; /* main thread only */
static int sReadyNew = 0;     /* under sBufMutex */
/* Rows of sReady that differ from the frame the main thread last took,
 * [y0, y1); y0 >= y1 = identical. Accumulated across every frame painted
 * since that hand-off, so skipping frames never skips their changes. */
static int sReadyY0 = 0, sReadyY1 = 0; /* under sBufMutex */
static int sTargetW = 0;      /* under sBufMutex: size the paint thread should paint at */
static int sTargetH = 0;

/* Main thread: the frame currently being moved into the texture. Its dirty
 * band is uploaded a slice per game frame and presented once complete, so
 * no single game frame pays for a whole 1024x768 upload (Mali re-tiles
 * texture uploads on the CPU; a full one per panel frame cost the game its
 * vsync every third frame — 45 FPS). */
static bool sPending = false;
static int sPendY = 0, sPendY1 = 0;
static int sRowsPerPump = 0; /* 0 = auto: a third of the panel */

/* Timing, logged every PANEL_STATS_SECONDS. Paint side is written by the
 * paint thread only, upload side by the main thread only. */
#define PANEL_STATS_SECONDS 10
static atomic_uint_fast64_t sStatPaintUs, sStatPaintN, sStatDirtyRows;
static uint64_t sStatUploadUs, sStatUploadN, sStatPresentUs, sStatPresentN, sStatUploadRows, sStatWindowStart;

/* Tap coordinate space = size of the most recently painted frame (that is
 * what the compositor captured its hit boxes in). */
static atomic_int sPaintedW;
static atomic_int sPaintedH;

static atomic_int sActive; /* a panel window exists and is being fed */

/* Main-thread SDL state. */
static SDL_Window* sPanelWin = NULL;
static SDL_Renderer* sPanelRen = NULL;
static SDL_Texture* sPanelTex = NULL;
static int sTexW = 0, sTexH = 0;
static SDL_WindowID sPanelWinId = 0;
static bool sCreateAttempted = false;
static int sPaintScale = 1;

static bool sPaintThreadStarted = false;
static bool sTouchThreadStarted = false;

static const char* EnvOr(const char* name, const char* fallback) {
    const char* v = getenv(name);
    return (v && v[0]) ? v : fallback;
}

static int EnvInt(const char* name, int fallback) {
    const char* v = getenv(name);
    if (!v || !v[0]) {
        return fallback;
    }
    return atoi(v);
}

static uint64_t NowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool EnsureCapacity(PanelBuf* b, int w, int h) {
    size_t need = (size_t)w * (size_t)h;
    if (need > b->cap) {
        uint32_t* p = (uint32_t*)realloc(b->px, need * sizeof(uint32_t));
        if (!p) {
            return false;
        }
        b->px = p;
        b->cap = need;
    }
    b->w = w;
    b->h = h;
    return true;
}

static uint64_t NowUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* Rows [y0, y1) where `cur` differs from `prev`; copies them into prev.
 * Returns y0 >= y1 when the frames are identical. */
static void DiffRows(const PanelBuf* cur, PanelBuf* prev, int* y0, int* y1) {
    const size_t rowBytes = (size_t)cur->w * sizeof(uint32_t);
    int first = -1, last = -1;
    for (int y = 0; y < cur->h; y++) {
        const uint32_t* a = cur->px + (size_t)y * (size_t)cur->w;
        uint32_t* b = prev->px + (size_t)y * (size_t)cur->w;
        if (memcmp(a, b, rowBytes) != 0) {
            memcpy(b, a, rowBytes);
            if (first < 0) {
                first = y;
            }
            last = y;
        }
    }
    *y0 = first < 0 ? 0 : first;
    *y1 = first < 0 ? 0 : last + 1;
}

static void* PaintThreadMain(void* arg) {
    (void)arg;
    uint32_t tick = 0;
    const uint64_t periodMs = 1000 / PANEL_PAINT_HZ;

    /* Below the game: when a core is contended, the PPU workers and the
     * audio synth win and the panel just paints a little later. */
    int nice = EnvInt("TMC_PANEL_NICE", 10);
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), nice) != 0) {
        fprintf(stderr, "[second_screen] paint thread: setpriority(%d) failed: %s\n", nice, strerror(errno));
    }
    fprintf(stderr, "[second_screen] paint thread started (nice %d)\n", nice);

    for (;;) {
        uint64_t start = NowMs();
        if (atomic_load(&sActive)) {
            int w, h;
            pthread_mutex_lock(&sBufMutex);
            w = sTargetW;
            h = sTargetH;
            pthread_mutex_unlock(&sBufMutex);

            if (w > 0 && h > 0 && EnsureCapacity(&sBack, w, h)) {
                uint64_t t0 = NowUs();
                SecondScreenSnapshot snap;
                Port_SecondScreenState_Read(&snap);
                Port_SecondScreen_PaintInto(sBack.px, w, h, w, &snap, ++tick);
                atomic_store(&sPaintedW, w);
                atomic_store(&sPaintedH, h);

                int y0 = 0, y1 = h;
                if (sPrev.w != w || sPrev.h != h || !sPrev.px) {
                    /* New size: everything is new. Seed the diff copy. */
                    if (EnsureCapacity(&sPrev, w, h)) {
                        memcpy(sPrev.px, sBack.px, (size_t)w * (size_t)h * sizeof(uint32_t));
                    }
                } else {
                    DiffRows(&sBack, &sPrev, &y0, &y1);
                }
                atomic_fetch_add(&sStatPaintUs, NowUs() - t0);
                atomic_fetch_add(&sStatPaintN, 1);
                atomic_fetch_add(&sStatDirtyRows, (uint64_t)(y1 > y0 ? y1 - y0 : 0));

                pthread_mutex_lock(&sBufMutex);
                PanelBuf t = sReady;
                sReady = sBack;
                sBack = t;
                if (sReadyNew) {
                    /* Not taken yet: widen the band to cover both frames. */
                    if (y1 > y0) {
                        if (sReadyY1 <= sReadyY0) {
                            sReadyY0 = y0;
                            sReadyY1 = y1;
                        } else {
                            if (y0 < sReadyY0) sReadyY0 = y0;
                            if (y1 > sReadyY1) sReadyY1 = y1;
                        }
                    }
                } else {
                    sReadyY0 = y0;
                    sReadyY1 = y1;
                }
                sReadyNew = 1;
                pthread_mutex_unlock(&sBufMutex);
            }
        }
        uint64_t spent = NowMs() - start;
        uint64_t sleepMs = spent < periodMs ? periodMs - spent : 1;
        struct timespec ts = { (time_t)(sleepMs / 1000), (long)((sleepMs % 1000) * 1000000) };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Touchscreen (evdev)                                                 */
/* ------------------------------------------------------------------ */

#define TEST_BIT(bits, n) (((bits)[(n) / (8 * sizeof(unsigned long))] >> ((n) % (8 * sizeof(unsigned long)))) & 1UL)

static bool DeviceHasAbsXY(int fd) {
    unsigned long abs[(ABS_MAX + 1) / (8 * sizeof(unsigned long)) + 1];
    memset(abs, 0, sizeof(abs));
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0) {
        return false;
    }
    return (TEST_BIT(abs, ABS_MT_POSITION_X) && TEST_BIT(abs, ABS_MT_POSITION_Y)) ||
           (TEST_BIT(abs, ABS_X) && TEST_BIT(abs, ABS_Y));
}

/* Finds the touchscreen whose sysfs device path contains `match`. */
static int OpenTouchByMatch(const char* match, char* pathOut, size_t pathCap) {
    DIR* d = opendir("/sys/class/input");
    if (!d) {
        return -1;
    }
    int found = -1;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0) {
            continue;
        }
        char link[PATH_MAX], real[PATH_MAX];
        snprintf(link, sizeof(link), "/sys/class/input/%s/device", de->d_name);
        if (!realpath(link, real) || strstr(real, match) == NULL) {
            continue;
        }
        char dev[PATH_MAX];
        snprintf(dev, sizeof(dev), "/dev/input/%s", de->d_name);
        int fd = open(dev, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "[second_screen] touch: cannot open %s: %s\n", dev, strerror(errno));
            continue;
        }
        if (!DeviceHasAbsXY(fd)) {
            close(fd);
            continue;
        }
        snprintf(pathOut, pathCap, "%s", dev);
        found = fd;
        break;
    }
    closedir(d);
    return found;
}

typedef struct {
    int fd;
    bool mt;
    struct input_absinfo ax, ay;
    int rotate; /* 0/90/180/270 */
    bool flipX, flipY;
} TouchDev;

static TouchDev sTouch = { .fd = -1 };

/* Raw device coordinates -> panel paint-buffer pixels. */
static void MapTouch(const TouchDev* t, int rx, int ry, int* outX, int* outY) {
    float spanX = (float)(t->ax.maximum - t->ax.minimum);
    float spanY = (float)(t->ay.maximum - t->ay.minimum);
    float nx = spanX > 0 ? (float)(rx - t->ax.minimum) / spanX : 0.f;
    float ny = spanY > 0 ? (float)(ry - t->ay.minimum) / spanY : 0.f;
    float u = nx, v = ny;
    switch (t->rotate) {
        case 90: u = 1.f - ny; v = nx; break;
        case 180: u = 1.f - nx; v = 1.f - ny; break;
        case 270: u = ny; v = 1.f - nx; break;
        default: break;
    }
    if (t->flipX) u = 1.f - u;
    if (t->flipY) v = 1.f - v;
    if (u < 0.f) u = 0.f;
    if (u > 1.f) u = 1.f;
    if (v < 0.f) v = 0.f;
    if (v > 1.f) v = 1.f;
    int w = atomic_load(&sPaintedW), h = atomic_load(&sPaintedH);
    *outX = (int)(u * (float)(w > 0 ? w - 1 : 0) + 0.5f);
    *outY = (int)(v * (float)(h > 0 ? h - 1 : 0) + 0.5f);
}

/* Per-device gesture state. Fed one evdev event at a time; a completed tap
 * is reported through the callback. Kept free of I/O and globals other than
 * the paint size (via MapTouch) so it can be driven off-device. */
typedef struct {
    int curX, curY, slot;
    bool down, pendDown, pendUp;
    uint64_t downAtMs;
} TouchState;

typedef void (*TouchTapFn)(int x, int y, int longPress);

static uint64_t EventMs(const struct input_event* e) {
    return (uint64_t)e->time.tv_sec * 1000u + (uint64_t)e->time.tv_usec / 1000u;
}

/* Handles both multitouch protocols the RG DS family's Goodix (gt911)
 * driver can speak: type B (ABS_MT_SLOT / ABS_MT_TRACKING_ID) and the
 * BTN_TOUCH + ABS_X/Y single-touch emulation. Only the first contact
 * (slot 0) drives the panel, like a single-pointer SurfaceView gesture. */
static void TouchFeed(const TouchDev* t, TouchState* s, const struct input_event* e, TouchTapFn onTap) {
    if (e->type == EV_ABS) {
        if (e->code == ABS_MT_SLOT) {
            s->slot = e->value;
        } else if (t->mt && s->slot == 0 && e->code == ABS_MT_POSITION_X) {
            s->curX = e->value;
        } else if (t->mt && s->slot == 0 && e->code == ABS_MT_POSITION_Y) {
            s->curY = e->value;
        } else if (!t->mt && e->code == ABS_X) {
            s->curX = e->value;
        } else if (!t->mt && e->code == ABS_Y) {
            s->curY = e->value;
        } else if (t->mt && s->slot == 0 && e->code == ABS_MT_TRACKING_ID) {
            if (e->value >= 0 && !s->down) {
                s->pendDown = true;
            } else if (e->value < 0 && s->down) {
                s->pendUp = true;
            }
        }
    } else if (e->type == EV_KEY && e->code == BTN_TOUCH) {
        if (e->value && !s->down) {
            s->pendDown = true;
        } else if (!e->value && s->down) {
            s->pendUp = true;
        }
    } else if (e->type == EV_SYN && e->code == SYN_DROPPED) {
        /* Kernel buffer overrun: drop the gesture rather than guess. */
        s->down = s->pendDown = s->pendUp = false;
    } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
        if (s->pendDown && !s->down) {
            s->down = true;
            s->downAtMs = EventMs(e);
        }
        if (s->pendUp && s->down) {
            s->down = false;
            int px, py;
            MapTouch(t, s->curX, s->curY, &px, &py);
            onTap(px, py, (EventMs(e) - s->downAtMs) >= PANEL_LONG_PRESS_MS ? 1 : 0);
        }
        s->pendDown = s->pendUp = false;
    }
}

static void DeliverTap(int x, int y, int longPress) {
    if (atomic_load(&sActive)) {
        Port_SecondScreen_OnTap(x, y, longPress);
    }
}

static void* TouchThreadMain(void* arg) {
    (void)arg;
    TouchDev* t = &sTouch;
    TouchState st;
    memset(&st, 0, sizeof(st));
    struct input_event evs[64];

    fprintf(stderr, "[second_screen] touch thread started\n");
    for (;;) {
        ssize_t n = read(t->fd, evs, sizeof(evs));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "[second_screen] touch: read failed (%s), touch disabled\n", strerror(errno));
            return NULL;
        }
        size_t count = (size_t)n / sizeof(struct input_event);
        for (size_t i = 0; i < count; i++) {
            TouchFeed(t, &st, &evs[i], DeliverTap);
        }
    }
    return NULL;
}

static void StartTouch(void) {
    const char* spec = EnvOr("TMC_PANEL_TOUCH", "auto");
    if (strcmp(spec, "sdl") == 0 || strcmp(spec, "none") == 0 || strcmp(spec, "0") == 0) {
        fprintf(stderr, "[second_screen] touch: evdev disabled, using SDL events on the panel window\n");
        return;
    }

    char path[PATH_MAX] = { 0 };
    int fd;
    if (strncmp(spec, "/dev/", 5) == 0) {
        snprintf(path, sizeof(path), "%s", spec);
        fd = open(path, O_RDONLY | O_CLOEXEC);
    } else {
        const char* match = EnvOr("TMC_PANEL_TOUCH_MATCH", "fe5e0000.i2c");
        fd = OpenTouchByMatch(match, path, sizeof(path));
        if (fd < 0) {
            fprintf(stderr,
                    "[second_screen] touch: no touchscreen matching '%s'; using SDL events on the panel window\n",
                    match);
            return;
        }
    }
    if (fd < 0) {
        fprintf(stderr, "[second_screen] touch: cannot open %s: %s\n", path, strerror(errno));
        return;
    }

    TouchDev* t = &sTouch;
    t->fd = fd;
    unsigned long abs[(ABS_MAX + 1) / (8 * sizeof(unsigned long)) + 1];
    memset(abs, 0, sizeof(abs));
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
    t->mt = TEST_BIT(abs, ABS_MT_POSITION_X) && TEST_BIT(abs, ABS_MT_POSITION_Y);
    ioctl(fd, EVIOCGABS(t->mt ? ABS_MT_POSITION_X : ABS_X), &t->ax);
    ioctl(fd, EVIOCGABS(t->mt ? ABS_MT_POSITION_Y : ABS_Y), &t->ay);
    t->rotate = EnvInt("TMC_PANEL_TOUCH_ROTATE", 0);
    t->flipX = EnvInt("TMC_PANEL_TOUCH_FLIPX", 0) != 0;
    t->flipY = EnvInt("TMC_PANEL_TOUCH_FLIPY", 0) != 0;

    char name[128] = "?";
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    if (EnvInt("TMC_PANEL_TOUCH_GRAB", 1) && ioctl(fd, EVIOCGRAB, 1) != 0) {
        fprintf(stderr, "[second_screen] touch: EVIOCGRAB failed (%s); taps may also reach the compositor\n",
                strerror(errno));
    }
    fprintf(stderr, "[second_screen] touch: %s '%s' %s x[%d..%d] y[%d..%d] rotate=%d\n", path, name,
            t->mt ? "multitouch" : "single-touch", t->ax.minimum, t->ax.maximum, t->ay.minimum, t->ay.maximum,
            t->rotate);

    pthread_t th;
    if (pthread_create(&th, NULL, TouchThreadMain, NULL) == 0) {
        pthread_detach(th);
        sTouchThreadStarted = true;
    }
}

/* ------------------------------------------------------------------ */
/*  Window (main thread)                                                */
/* ------------------------------------------------------------------ */

static SDL_Window* FindGameWindow(void) {
    int n = 0;
    SDL_Window** wins = SDL_GetWindows(&n);
    SDL_Window* game = NULL;
    for (int i = 0; wins && i < n; i++) {
        if (wins[i] != sPanelWin) {
            game = wins[i];
            break;
        }
    }
    SDL_free(wins);
    return game;
}

static SDL_DisplayID PickPanelDisplay(SDL_Window* game) {
    int n = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&n);
    if (!ids) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "[second_screen] display %d: id=%u '%s'\n", i, (unsigned)ids[i],
                SDL_GetDisplayName(ids[i]) ? SDL_GetDisplayName(ids[i]) : "?");
    }

    SDL_DisplayID pick = 0;
    const char* want = getenv("TMC_PANEL_DISPLAY");
    if (want && want[0]) {
        char* end = NULL;
        long idx = strtol(want, &end, 10);
        if (end && *end == '\0' && idx >= 0 && idx < n) {
            pick = ids[idx];
        } else {
            for (int i = 0; i < n && !pick; i++) {
                const char* name = SDL_GetDisplayName(ids[i]);
                if (name && strstr(name, want)) {
                    pick = ids[i];
                }
            }
        }
        if (!pick) {
            fprintf(stderr, "[second_screen] TMC_PANEL_DISPLAY='%s' matched no display\n", want);
        }
    }
    if (!pick) {
        SDL_DisplayID gameDisp = game ? SDL_GetDisplayForWindow(game) : SDL_GetPrimaryDisplay();
        for (int i = 0; i < n; i++) {
            if (ids[i] != gameDisp) {
                pick = ids[i];
                break;
            }
        }
    }
    SDL_free(ids);
    return pick;
}

static bool CreatePanelWindow(void) {
    const char* enable = getenv("TMC_PANEL");
    if (enable && strcmp(enable, "0") == 0) {
        fprintf(stderr, "[second_screen] disabled by TMC_PANEL=0\n");
        return false;
    }
    bool force = enable && strcmp(enable, "1") == 0;

    SDL_Window* game = FindGameWindow();
    SDL_DisplayID disp = PickPanelDisplay(game);
    bool windowed = EnvInt("TMC_PANEL_WINDOWED", 0) != 0;
    if (!disp) {
        if (!force) {
            fprintf(stderr, "[second_screen] only one display; panel off (TMC_PANEL=1 forces a window)\n");
            return false;
        }
        windowed = true;
    }

    int w = 640, h = 480;
    if (disp) {
        SDL_Rect r;
        if (SDL_GetDisplayBounds(disp, &r) && r.w > 0 && r.h > 0) {
            w = r.w;
            h = r.h;
        }
    }
    if (windowed && w > 1024) {
        w /= 2;
        h /= 2;
    }

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, PANEL_TITLE);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
    if (disp) {
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(disp));
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(disp));
    }
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, !windowed);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, windowed);
    sPanelWin = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!sPanelWin) {
        fprintf(stderr, "[second_screen] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    sPanelWinId = SDL_GetWindowID(sPanelWin);

    /* GLES2 first (the game's own renderer is too on these devices), then
     * whatever SDL prefers, then the CPU fallback. */
    const char* drivers[] = { "opengles2", NULL, "software" };
    for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]) && !sPanelRen; i++) {
        sPanelRen = SDL_CreateRenderer(sPanelWin, drivers[i]);
    }
    if (!sPanelRen) {
        fprintf(stderr, "[second_screen] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(sPanelWin);
        sPanelWin = NULL;
        sPanelWinId = 0;
        return false;
    }
    /* Never let the panel's swap wait on its own vblank: the game loop is
     * paced by the top screen, and this present happens inside it. */
    SDL_SetRenderVSync(sPanelRen, 0);
    SDL_HideCursor();

    if (game) {
        SDL_RaiseWindow(game); /* keep focus on the game */
    }
    fprintf(stderr, "[second_screen] panel window %dx%d on display %u (%s), %s, renderer %s\n", w, h, (unsigned)disp,
            disp && SDL_GetDisplayName(disp) ? SDL_GetDisplayName(disp) : "-", windowed ? "windowed" : "fullscreen",
            SDL_GetRendererName(sPanelRen));
    return true;
}

static void DestroyPanelWindow(void) {
    atomic_store(&sActive, 0);
    if (sPanelTex) {
        SDL_DestroyTexture(sPanelTex);
        sPanelTex = NULL;
    }
    if (sPanelRen) {
        SDL_DestroyRenderer(sPanelRen);
        sPanelRen = NULL;
    }
    if (sPanelWin) {
        SDL_DestroyWindow(sPanelWin);
        sPanelWin = NULL;
    }
    sPanelWinId = 0;
    sTexW = sTexH = 0;
    sPending = false;
    fprintf(stderr, "[second_screen] panel window closed\n");
}

void Port_SecondScreen_Init(void) {
    sRowsPerPump = EnvInt("TMC_PANEL_UPLOAD_ROWS", 0);
    if (sRowsPerPump < 0) {
        sRowsPerPump = 0;
    }
    sPaintScale = EnvInt("TMC_PANEL_SCALE", 1);
    if (sPaintScale < 1) {
        sPaintScale = 1;
    }
    fprintf(stderr, "[second_screen] init (Linux dual-screen); panel opens with the first game frame\n");
}

/* One line every PANEL_STATS_SECONDS so log.txt shows what the panel
 * costs on the device: paint (paint thread), upload + present (game thread). */
static void PanelStatsMaybeLog(void) {
    uint64_t now = NowUs();
    if (sStatWindowStart == 0) {
        sStatWindowStart = now;
        return;
    }
    if (now - sStatWindowStart < (uint64_t)PANEL_STATS_SECONDS * 1000000u) {
        return;
    }
    uint64_t pn = atomic_exchange(&sStatPaintN, 0);
    uint64_t pus = atomic_exchange(&sStatPaintUs, 0);
    uint64_t dirty = atomic_exchange(&sStatDirtyRows, 0);
    fprintf(stderr,
            "[second_screen] %ds: %llu paints avg %.1f ms (%.0f dirty rows avg) | %llu uploads avg %.2f ms "
            "(%.0f rows avg) | %llu presents avg %.2f ms\n",
            PANEL_STATS_SECONDS, (unsigned long long)pn, pn ? pus / 1000.0 / pn : 0.0,
            pn ? (double)dirty / pn : 0.0, (unsigned long long)sStatUploadN,
            sStatUploadN ? sStatUploadUs / 1000.0 / sStatUploadN : 0.0,
            sStatUploadN ? (double)sStatUploadRows / sStatUploadN : 0.0, (unsigned long long)sStatPresentN,
            sStatPresentN ? sStatPresentUs / 1000.0 / sStatPresentN : 0.0);
    sStatUploadUs = sStatUploadN = sStatPresentUs = sStatPresentN = sStatUploadRows = 0;
    sStatWindowStart = now;
}

void Port_SecondScreen_LinuxPump(void) {
    if (!sCreateAttempted) {
        sCreateAttempted = true;
        if (!CreatePanelWindow()) {
            return;
        }
        if (!sPaintThreadStarted) {
            pthread_t th;
            if (pthread_create(&th, NULL, PaintThreadMain, NULL) == 0) {
                pthread_detach(th);
                sPaintThreadStarted = true;
            }
        }
        if (!sTouchThreadStarted) {
            StartTouch();
        }
    }
    if (!sPanelWin) {
        return;
    }

    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(sPanelWin, &pw, &ph);
    if (pw <= 0 || ph <= 0) {
        return;
    }
    int tw = pw / sPaintScale, th = ph / sPaintScale;

    pthread_mutex_lock(&sBufMutex);
    sTargetW = tw;
    sTargetH = th;
    /* Take a new frame only between uploads: the one in flight is read
     * straight out of sUpload until its last slice is in the texture. */
    if (!sPending && sReadyNew) {
        PanelBuf t = sUpload;
        sUpload = sReady;
        sReady = t;
        sReadyNew = 0;
        sPending = true;
        sPendY = sReadyY0;
        sPendY1 = sReadyY1;
        sReadyY0 = sReadyY1 = 0;
    }
    pthread_mutex_unlock(&sBufMutex);
    atomic_store(&sActive, 1);

    PanelStatsMaybeLog();

    if (!sPending || !sUpload.px) {
        return;
    }

    /* The game's renderer may own a GL context on this thread; leave it
     * current again afterwards whatever SDL's panel renderer does. */
    SDL_Window* prevWin = SDL_GL_GetCurrentWindow();
    SDL_GLContext prevCtx = SDL_GL_GetCurrentContext();

    if (!sPanelTex || sTexW != sUpload.w || sTexH != sUpload.h) {
        if (sPanelTex) {
            SDL_DestroyTexture(sPanelTex);
        }
        /* Panel pixels are A<<24|B<<16|G<<8|R little-endian u32s. */
        sPanelTex = SDL_CreateTexture(sPanelRen, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, sUpload.w,
                                      sUpload.h);
        if (sPanelTex) {
            SDL_SetTextureScaleMode(sPanelTex, SDL_SCALEMODE_NEAREST);
            sTexW = sUpload.w;
            sTexH = sUpload.h;
        }
        /* A fresh texture holds nothing: upload the whole frame. */
        sPendY = 0;
        sPendY1 = sUpload.h;
    }
    if (!sPanelTex) {
        sPending = false;
        goto restore;
    }

    if (sPendY < sPendY1) {
        int rows = sRowsPerPump > 0 ? sRowsPerPump : (sUpload.h + 2) / 3;
        int y1 = sPendY + rows;
        if (y1 > sPendY1) {
            y1 = sPendY1;
        }
        uint64_t t0 = NowUs();
        SDL_Rect r = { 0, sPendY, sUpload.w, y1 - sPendY };
        SDL_UpdateTexture(sPanelTex, &r, sUpload.px + (size_t)sPendY * (size_t)sUpload.w,
                          sUpload.w * (int)sizeof(uint32_t));
        sStatUploadUs += NowUs() - t0;
        sStatUploadN++;
        sStatUploadRows += (uint64_t)(y1 - sPendY);
        sPendY = y1;
        if (sPendY < sPendY1) {
            goto restore; /* more slices next game frame */
        }
    } else {
        /* Identical to what is on screen: nothing to upload or present. */
        sPending = false;
        goto restore;
    }

    {
        uint64_t t0 = NowUs();
        SDL_SetRenderDrawColor(sPanelRen, 0, 0, 0, 255);
        SDL_RenderClear(sPanelRen);
        SDL_RenderTexture(sPanelRen, sPanelTex, NULL, NULL);
        SDL_RenderPresent(sPanelRen);
        sStatPresentUs += NowUs() - t0;
        sStatPresentN++;
    }
    sPending = false;

restore:
    if (prevCtx && SDL_GL_GetCurrentContext() != prevCtx) {
        SDL_GL_MakeCurrent(prevWin, prevCtx);
    }
}

/* SDL input aimed at the panel window: taps by touch or mouse (the
 * fallback when no evdev touchscreen was found, and how the panel is
 * driven on a desktop), plus its window lifecycle. Keyboard and gamepad
 * events are never consumed — they belong to the game whichever window
 * the compositor thinks is focused. */
int Port_SecondScreen_LinuxHandleEvent(const union SDL_Event* ev) {
    const SDL_Event* e = (const SDL_Event*)ev;
    if (!sPanelWin || sPanelWinId == 0) {
        return 0;
    }
    static uint64_t sDownAt = 0;

    switch (e->type) {
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_CANCELED:
            if (e->tfinger.windowID != sPanelWinId) {
                return 0;
            }
            if (e->type == SDL_EVENT_FINGER_DOWN) {
                sDownAt = NowMs();
            } else if (e->type == SDL_EVENT_FINGER_UP) {
                int w = atomic_load(&sPaintedW), h = atomic_load(&sPaintedH);
                Port_SecondScreen_OnTap((int)(e->tfinger.x * (float)w), (int)(e->tfinger.y * (float)h),
                                        (NowMs() - sDownAt) >= PANEL_LONG_PRESS_MS);
            }
            return 1;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_WHEEL:
            if (e->button.windowID != sPanelWinId) {
                return 0;
            }
            if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.which != SDL_TOUCH_MOUSEID) {
                sDownAt = NowMs();
            } else if (e->type == SDL_EVENT_MOUSE_BUTTON_UP && e->button.which != SDL_TOUCH_MOUSEID) {
                int winW = 1, winH = 1;
                SDL_GetWindowSize(sPanelWin, &winW, &winH);
                int w = atomic_load(&sPaintedW), h = atomic_load(&sPaintedH);
                int x = winW > 0 ? (int)(e->button.x * (float)w / (float)winW) : 0;
                int y = winH > 0 ? (int)(e->button.y * (float)h / (float)winH) : 0;
                /* Right click stands in for press-and-hold. */
                bool longPress = e->button.button == SDL_BUTTON_RIGHT || (NowMs() - sDownAt) >= PANEL_LONG_PRESS_MS;
                Port_SecondScreen_OnTap(x, y, longPress);
            }
            return 1;

        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (e->window.windowID != sPanelWinId) {
                return 0;
            }
            DestroyPanelWindow();
            return 1;

        default:
            if (e->type >= SDL_EVENT_WINDOW_FIRST && e->type <= SDL_EVENT_WINDOW_LAST &&
                e->window.windowID == sPanelWinId) {
                return 1;
            }
            return 0;
    }
}

void Port_SecondScreen_OnSurfaceReady(void* window, int width, int height) {
    (void)window;
    (void)width;
    (void)height;
}

void Port_SecondScreen_OnSurfaceLost(void) {}

int Port_SecondScreen_HasSurface(void) {
    return atomic_load(&sActive) ? 1 : 0;
}

#endif /* PORT_SS_LINUX */
