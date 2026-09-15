// Video for the Scratch Everywhere! PAPP.
//
// The renderer (papp_render.cpp) draws the stage into a 480x360 RGB565 frame.
// The loader scales each frame to the canvas with the P4's PPA
// (display_write_frame_custom). A presenter task on core 1 shows frame N
// while the app draws frame N+1 into the other buffer.
//
// The canvas is one of CANVAS_SIZES, picked from what the loader offers (the
// store's Screen setting, else the listing's recommended 800x600):
//   800x600  480x360 x5/3: the panel's full height, 4:3 like the stage
//   640x480  480x360 x4/3
//   480x360  480x360 x1
#include "papp_port.h"

#include <string.h>

// Largest first; the same sizes as "canvas" in apps/psram_scratch/papp.json.
static const struct {
    int w, h;
} CANVAS_SIZES[] = {{800, 600}, {640, 480}, {480, 360}};

// The frame's place on the canvas: 480x360 at 5/3 on the 800x480 canvas would
// not fit, so a loader without the canvas services gets x4/3 (640x480 centred
// on 800x480).
static int s_canvas_w = 800, s_canvas_h = 480;
static float s_scale = 4.0f / 3.0f;

static uint16_t *s_frames[2] = {nullptr, nullptr};
static int s_back = 0;                // the buffer the app draws into
static volatile int s_show = -1;      // the buffer the presenter shows next (-1: none)
static volatile bool s_quit = false;
static volatile bool s_done = true;
static void *s_task = nullptr;

static const size_t FRAME_BYTES = PAPP_FRAME_W * PAPP_FRAME_H * sizeof(uint16_t);

static void show(const uint16_t *frame)
{
    papp_svc->display_write_frame_custom(frame, PAPP_FRAME_W, PAPP_FRAME_H, s_scale, false);
}

// Switch to the largest of CANVAS_SIZES that fits in the loader's offer (the
// smallest when none does). A loader without display_set_canvas, or one that
// refuses, keeps 800x480 with the frame at x4/3.
static void choose_canvas()
{
    if (papp_svc->display_get_size == nullptr || papp_svc->display_set_canvas == nullptr) {
        return;
    }
    int offer_w = 0, offer_h = 0;
    papp_svc->display_get_size(&offer_w, &offer_h);
    const int count = (int)(sizeof(CANVAS_SIZES) / sizeof(CANVAS_SIZES[0]));
    int pick = count - 1;
    for (int i = 0; i < count; i++) {
        if (CANVAS_SIZES[i].w <= offer_w && CANVAS_SIZES[i].h <= offer_h) {
            pick = i;
            break;
        }
    }
    if (papp_svc->display_set_canvas(CANVAS_SIZES[pick].w, CANVAS_SIZES[pick].h) == 0) {
        s_canvas_w = CANVAS_SIZES[pick].w;
        s_canvas_h = CANVAS_SIZES[pick].h;
        s_scale = (float)s_canvas_h / PAPP_FRAME_H;
        papp_svc->log_printf("SE: offered a %dx%d canvas, using %dx%d\n", offer_w, offer_h, s_canvas_w, s_canvas_h);
    } else {
        papp_svc->log_printf("SE: %dx%d canvas refused, keeping 800x480\n", CANVAS_SIZES[pick].w, CANVAS_SIZES[pick].h);
    }
}

extern "C" int papp_video_canvas_to_frame(int cx, int cy, int *fx, int *fy)
{
    const float out_w = PAPP_FRAME_W * s_scale;
    const float out_h = PAPP_FRAME_H * s_scale;
    const float x0 = (s_canvas_w - out_w) * 0.5f;
    const float y0 = (s_canvas_h - out_h) * 0.5f;
    const int x = (int)((cx - x0) / s_scale);
    const int y = (int)((cy - y0) / s_scale);
    const int inside = cx >= x0 && cy >= y0 && x < PAPP_FRAME_W && y < PAPP_FRAME_H;
    *fx = x < 0 ? 0 : (x >= PAPP_FRAME_W ? PAPP_FRAME_W - 1 : x);
    *fy = y < 0 ? 0 : (y >= PAPP_FRAME_H ? PAPP_FRAME_H - 1 : y);
    return inside;
}

// Every 60 s: frames shown per second and the average time a flush takes.
static void log_rate(long long flush_us)
{
    static long long window = 0, total = 0;
    static int shown = 0;
    const long long now = papp_time_us();
    if (window == 0) {
        window = now;
    }
    shown++;
    total += flush_us;
    if (now - window >= 60000000) {
        papp_svc->log_printf("SE: %d fps shown, flush %lld us\n", (int)(shown * 1000000LL / (now - window)),
                             total / shown);
        window = now;
        shown = 0;
        total = 0;
    }
}

static void presenter_task(void *)
{
    while (!s_quit) {
        const int index = s_show;
        if (index < 0) {
            // delay_ms() below one tick (10 ms) is only a yield, which would
            // keep ESPHome's loop on this core from running: sleep a tick.
            papp_svc->delay_ms(10);
            continue;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const long long start = papp_time_us();
        show(s_frames[index]);
        log_rate(papp_time_us() - start);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        s_show = -1;
    }
    s_done = true;
    for (;;) {
        papp_svc->delay_ms(1000);  // deleted by papp_video_shutdown
    }
}

extern "C" int papp_video_init(void)
{
    // On the app task, before the presenter task exists.
    choose_canvas();
    for (int i = 0; i < 2; i++) {
        s_frames[i] = static_cast<uint16_t *>(papp_alloc_raw(FRAME_BYTES));
        if (s_frames[i] == nullptr) {
            papp_svc->log_printf("SE: no memory for the frame buffers\n");
            return -1;
        }
        memset(s_frames[i], 0xFF, FRAME_BYTES);
    }
    papp_svc->display_clear(0x0000);
    s_back = 0;
    s_show = -1;
    s_quit = false;
    s_done = false;
    // Core 1, priority 3: above ESPHome's loop, below the audio task.
    if (papp_svc->task_create(presenter_task, "se_present", 8 * 1024, nullptr, 3, &s_task, 1) != 0) {
        s_task = nullptr;
        s_done = true;
        papp_svc->log_printf("SE: no presenter task, presenting on the app task\n");
    }
    return 0;
}

extern "C" uint16_t *papp_video_back(void)
{
    return s_frames[s_back];
}

extern "C" void papp_video_present(void)
{
    if (s_task == nullptr) {
        show(s_frames[s_back]);
        return;
    }
    // Wait for the presenter to finish the other buffer (the previous frame):
    // the app draws into it next.
    while (s_show >= 0) {
        papp_svc->delay_ms(1);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s_show = s_back;
    s_back ^= 1;
}

extern "C" void papp_video_shutdown(void)
{
    if (s_task != nullptr) {
        s_quit = true;
        for (int i = 0; i < 200 && !s_done; i++) {
            papp_svc->delay_ms(5);
        }
        papp_svc->task_delete(s_task);
        s_task = nullptr;
    }
    for (int i = 0; i < 2; i++) {
        if (s_frames[i] != nullptr) {
            papp_svc->mem_free(s_frames[i]);
        }
        s_frames[i] = nullptr;
    }
}
