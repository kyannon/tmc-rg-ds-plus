/*
 * Off-device check for port/port_second_screen_linux.c (the RG DS / RG DS
 * Plus panel host). Builds that file against stub compositor entry points
 * and exercises it on a desktop with SDL's offscreen video driver:
 *
 *   1. window + paint thread + main-thread upload/present
 *   2. SDL mouse/finger taps on the panel window -> Port_SecondScreen_OnTap
 *      in paint-buffer coordinates, right click / hold = long press
 *   3. events for other windows are left alone
 *   4. evdev gesture parsing (TouchFeed) for the gt911's type-B multitouch
 *      stream and single-touch ABS_X/Y, with kernel-timestamp long press,
 *      second-finger isolation, SYN_DROPPED and rotation mapping
 *   5. close request tears the panel down
 *
 * Build (host):
 *   cc -std=c11 -DPORT_DUAL_SCREEN_LINUX -Iport -I<sdl3>/include \
 *      tools/second_screen_linux_harness.c -L<sdl3>/lib -lSDL3 -lpthread
 * Run: SDL_VIDEODRIVER=offscreen ./a.out
 */
#include "../port/port_second_screen_linux.c"

static int gPaints = 0, gTaps = 0, gLastX = -1, gLastY = -1, gLastLong = -1;
static int gFails = 0;

#define CHECK(cond, ...)                                                                                             \
    do {                                                                                                             \
        if (cond) {                                                                                                  \
            printf("  ok   ");                                                                                       \
        } else {                                                                                                     \
            printf("  FAIL ");                                                                                       \
            gFails++;                                                                                                \
        }                                                                                                            \
        printf(__VA_ARGS__);                                                                                         \
        printf("\n");                                                                                                \
    } while (0)

/* ---- stubs for the shared compositor / snapshot ---- */
void Port_SecondScreen_PaintInto(uint32_t* pixels, int width, int height, int stride, const SecondScreenSnapshot* snap,
                                 uint32_t tick) {
    (void)snap;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            pixels[y * stride + x] = 0xFF000000u | (uint32_t)((x ^ y ^ (int)tick) & 0xFF);
        }
    }
    __atomic_add_fetch(&gPaints, 1, __ATOMIC_SEQ_CST);
}
void Port_SecondScreen_OnTap(int x, int y, int longPress) {
    gLastX = x;
    gLastY = y;
    gLastLong = longPress;
    __atomic_add_fetch(&gTaps, 1, __ATOMIC_SEQ_CST);
}
void Port_SecondScreenState_Read(SecondScreenSnapshot* out) {
    memset(out, 0, sizeof(*out));
}

static void PumpFor(int ms) {
    uint64_t end = NowMs() + (uint64_t)ms;
    while (NowMs() < end) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            Port_SecondScreen_LinuxHandleEvent(&e);
        }
        Port_SecondScreen_LinuxPump();
        SDL_Delay(16);
    }
}

static void Ev(struct input_event* e, uint16_t type, uint16_t code, int32_t value, int ms) {
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->code = code;
    e->value = value;
    e->time.tv_sec = ms / 1000;
    e->time.tv_usec = (ms % 1000) * 1000;
}

static void Feed(TouchDev* d, TouchState* s, const struct input_event* evs, int n) {
    for (int i = 0; i < n; i++) {
        TouchFeed(d, s, &evs[i], Port_SecondScreen_OnTap);
    }
}

int main(void) {
    setenv("TMC_PANEL", "1", 1);
    setenv("TMC_PANEL_TOUCH", "sdl", 1);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    SDL_Window* game = SDL_CreateWindow("game", 480, 320, 0);
    CHECK(game != NULL, "game window created");

    printf("[1] window, paint thread, upload\n");
    Port_SecondScreen_Init();
    PumpFor(600);
    CHECK(sPanelWin != NULL, "panel window exists");
    CHECK(Port_SecondScreen_HasSurface() == 1, "HasSurface reports the panel");
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(sPanelWin, &pw, &ph);
    CHECK(gPaints >= 8, "paint thread ran (%d paints in 0.6 s, ~20 Hz expected)", gPaints);
    CHECK(atomic_load(&sPaintedW) == pw && atomic_load(&sPaintedH) == ph, "painted at panel pixel size %dx%d",
          atomic_load(&sPaintedW), atomic_load(&sPaintedH));
    CHECK(sPanelTex != NULL && sTexW == pw && sTexH == ph, "streaming texture matches (%dx%d)", sTexW, sTexH);
    CHECK(SDL_GetRendererName(sPanelRen) != NULL, "renderer: %s", SDL_GetRendererName(sPanelRen));

    printf("[2] SDL taps on the panel window\n");
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.windowID = sPanelWinId;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.x = (float)pw / 2;
    e.button.y = (float)ph / 4;
    CHECK(Port_SecondScreen_LinuxHandleEvent(&e) == 1, "mouse down consumed");
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    CHECK(Port_SecondScreen_LinuxHandleEvent(&e) == 1, "mouse up consumed");
    CHECK(gTaps == 1 && gLastX == pw / 2 && gLastY == ph / 4 && gLastLong == 0, "left click -> tap (%d,%d) long=%d",
          gLastX, gLastY, gLastLong);
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.button = SDL_BUTTON_RIGHT;
    Port_SecondScreen_LinuxHandleEvent(&e);
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    Port_SecondScreen_LinuxHandleEvent(&e);
    CHECK(gTaps == 2 && gLastLong == 1, "right click -> long press");

    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_FINGER_DOWN;
    e.tfinger.windowID = sPanelWinId;
    e.tfinger.x = 0.25f;
    e.tfinger.y = 0.75f;
    Port_SecondScreen_LinuxHandleEvent(&e);
    e.type = SDL_EVENT_FINGER_UP;
    Port_SecondScreen_LinuxHandleEvent(&e);
    CHECK(gTaps == 3 && gLastX == (int)(0.25f * pw) && gLastY == (int)(0.75f * ph) && gLastLong == 0,
          "finger tap -> (%d,%d)", gLastX, gLastY);

    printf("[3] other windows' input is not taken\n");
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = SDL_GetWindowID(game);
    CHECK(Port_SecondScreen_LinuxHandleEvent(&e) == 0, "game-window click passes through");
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = sPanelWinId;
    CHECK(Port_SecondScreen_LinuxHandleEvent(&e) == 0, "keys pass through even when aimed at the panel");
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    CHECK(Port_SecondScreen_LinuxHandleEvent(&e) == 0, "gamepad passes through");

    printf("[4] evdev gestures (gt911-style)\n");
    atomic_store(&sPaintedW, 1024);
    atomic_store(&sPaintedH, 768);
    TouchDev d;
    memset(&d, 0, sizeof(d));
    d.mt = true;
    d.ax.minimum = 0;
    d.ax.maximum = 1023;
    d.ay.minimum = 0;
    d.ay.maximum = 767;
    TouchState st;
    memset(&st, 0, sizeof(st));
    struct input_event ev[16];
    int n = 0, before = gTaps;
    /* finger down at (512,384), moves to (600,400), lifts 100 ms later */
    Ev(&ev[n++], EV_ABS, ABS_MT_SLOT, 0, 1000);
    Ev(&ev[n++], EV_ABS, ABS_MT_TRACKING_ID, 7, 1000);
    Ev(&ev[n++], EV_ABS, ABS_MT_POSITION_X, 512, 1000);
    Ev(&ev[n++], EV_ABS, ABS_MT_POSITION_Y, 384, 1000);
    Ev(&ev[n++], EV_KEY, BTN_TOUCH, 1, 1000);
    Ev(&ev[n++], EV_SYN, SYN_REPORT, 0, 1000);
    Ev(&ev[n++], EV_ABS, ABS_MT_POSITION_X, 600, 1050);
    Ev(&ev[n++], EV_ABS, ABS_MT_POSITION_Y, 400, 1050);
    Ev(&ev[n++], EV_SYN, SYN_REPORT, 0, 1050);
    Feed(&d, &st, ev, n);
    CHECK(gTaps == before, "no tap while the finger is down");
    n = 0;
    Ev(&ev[n++], EV_ABS, ABS_MT_TRACKING_ID, -1, 1100);
    Ev(&ev[n++], EV_KEY, BTN_TOUCH, 0, 1100);
    Ev(&ev[n++], EV_SYN, SYN_REPORT, 0, 1100);
    Feed(&d, &st, ev, n);
    CHECK(gTaps == before + 1 && gLastX == 600 && gLastY == 400 && gLastLong == 0,
          "short tap lands where the finger lifted (%d,%d) long=%d", gLastX, gLastY, gLastLong);

    /* press-and-hold 500 ms */
    n = 0;
    Ev(&ev[n++], EV_ABS, ABS_MT_TRACKING_ID, 8, 2000);
    Ev(&ev[n++], EV_ABS, ABS_MT_POSITION_X, 10, 2000);
    Ev(&ev[n++], EV_ABS, ABS_MT_POSITION_Y, 20, 2000);
    Ev(&ev[n++], EV_KEY, BTN_TOUCH, 1, 2000);
    Ev(&ev[n++], EV_SYN, SYN_REPORT, 0, 2000);
    Ev(&ev[n++], EV_ABS, ABS_MT_TRACKING_ID, -1, 2500);
    Ev(&ev[n++], EV_KEY, BTN_TOUCH, 0, 2500);
    Ev(&ev[n++], EV_SYN, SYN_REPORT, 0, 2500);
    Feed(&d, &st, ev, n);
    CHECK(gTaps == before + 2 && gLastLong == 1 && gLastX == 10 && gLastY == 20, "500 ms hold -> long press");

    /* a second finger on slot 1 neither moves nor ends the first gesture */
    n = 0;
    Ev(&ev[n++], EV_ABS, ABS_MT_SLOT, 0, 3000);
    Ev(&ev[n++], EV_ABS, ABS_MT_TRACKING_ID, 9, 3000);
    Ev(&ev[n++], EV_ABS, ABS_MT_POSITION_X, 100, 3000);
    Ev(&ev[n++], EV_ABS, ABS_MT_POSITION_Y, 100, 3000);
    Ev(&ev[n++], EV_KEY, BTN_TOUCH, 1, 3000);
    Ev(&ev[n++], EV_SYN, SYN_REPORT, 0, 3000);
    Ev(&ev[n++], EV_ABS, ABS_MT_SLOT, 1, 3050);
    Ev(&ev[n++], EV_ABS, ABS_MT_TRACKING_ID, 10, 3050);
    Ev(&ev[n++], EV_ABS, ABS_MT_POSITION_X, 900, 3050);
    Ev(&ev[n++], EV_ABS, ABS_MT_POSITION_Y, 700, 3050);
    Ev(&ev[n++], EV_SYN, SYN_REPORT, 0, 3050);
    Ev(&ev[n++], EV_ABS, ABS_MT_TRACKING_ID, -1, 3060);
    Ev(&ev[n++], EV_SYN, SYN_REPORT, 0, 3060);
    Feed(&d, &st, ev, n);
    CHECK(gTaps == before + 2, "second finger lifting doesn't fire a tap");
    n = 0;
    Ev(&ev[n++], EV_ABS, ABS_MT_SLOT, 0, 3100);
    Ev(&ev[n++], EV_ABS, ABS_MT_TRACKING_ID, -1, 3100);
    Ev(&ev[n++], EV_KEY, BTN_TOUCH, 0, 3100);
    Ev(&ev[n++], EV_SYN, SYN_REPORT, 0, 3100);
    Feed(&d, &st, ev, n);
    CHECK(gTaps == before + 3 && gLastX == 100 && gLastY == 100, "first finger's tap keeps its own position");

    /* SYN_DROPPED cancels the gesture */
    n = 0;
    Ev(&ev[n++], EV_ABS, ABS_MT_TRACKING_ID, 11, 4000);
    Ev(&ev[n++], EV_KEY, BTN_TOUCH, 1, 4000);
    Ev(&ev[n++], EV_SYN, SYN_REPORT, 0, 4000);
    Ev(&ev[n++], EV_SYN, SYN_DROPPED, 0, 4010);
    Ev(&ev[n++], EV_ABS, ABS_MT_TRACKING_ID, -1, 4020);
    Ev(&ev[n++], EV_KEY, BTN_TOUCH, 0, 4020);
    Ev(&ev[n++], EV_SYN, SYN_REPORT, 0, 4020);
    Feed(&d, &st, ev, n);
    CHECK(gTaps == before + 3, "SYN_DROPPED drops the gesture");

    /* single-touch device (ABS_X/ABS_Y + BTN_TOUCH), scaled range */
    TouchDev st1;
    memset(&st1, 0, sizeof(st1));
    st1.mt = false;
    st1.ax.maximum = 4095;
    st1.ay.maximum = 4095;
    TouchState s1;
    memset(&s1, 0, sizeof(s1));
    n = 0;
    Ev(&ev[n++], EV_ABS, ABS_X, 4095, 5000);
    Ev(&ev[n++], EV_ABS, ABS_Y, 0, 5000);
    Ev(&ev[n++], EV_KEY, BTN_TOUCH, 1, 5000);
    Ev(&ev[n++], EV_SYN, SYN_REPORT, 0, 5000);
    Ev(&ev[n++], EV_KEY, BTN_TOUCH, 0, 5080);
    Ev(&ev[n++], EV_SYN, SYN_REPORT, 0, 5080);
    Feed(&st1, &s1, ev, n);
    CHECK(gTaps == before + 4 && gLastX == 1023 && gLastY == 0, "single-touch top-right corner -> (%d,%d)", gLastX,
          gLastY);

    /* rotation: a panel mounted 90 degrees off */
    int rx, ry;
    d.rotate = 90;
    MapTouch(&d, 0, 0, &rx, &ry);
    CHECK(rx == 1023 && ry == 0, "rotate 90: raw origin -> (%d,%d)", rx, ry);
    d.rotate = 180;
    MapTouch(&d, 0, 0, &rx, &ry);
    CHECK(rx == 1023 && ry == 767, "rotate 180: raw origin -> (%d,%d)", rx, ry);
    d.rotate = 0;
    d.flipX = true;
    MapTouch(&d, 0, 0, &rx, &ry);
    CHECK(rx == 1023 && ry == 0, "flipX: raw origin -> (%d,%d)", rx, ry);

    printf("[5] close request\n");
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    e.window.windowID = sPanelWinId;
    CHECK(Port_SecondScreen_LinuxHandleEvent(&e) == 1, "close consumed");
    CHECK(sPanelWin == NULL && Port_SecondScreen_HasSurface() == 0, "panel torn down");
    int paints = gPaints;
    PumpFor(300);
    CHECK(gPaints == paints, "paint thread idles once the panel is gone");

    SDL_DestroyWindow(game);
    SDL_Quit();
    printf(gFails ? "\n%d FAILED\n" : "\nall checks passed\n", gFails);
    return gFails ? 1 : 0;
}
