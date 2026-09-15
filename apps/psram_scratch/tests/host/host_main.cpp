// Host test for the Scratch Everywhere! PAPP: the app's own code (ports/
// scratch, minus the newlib glue and app_entry) and the pinned upstream
// runtime, built for Linux with a fake loader service table, run on
// papp_test.sb3 (apps/psram_scratch/tests) with scripted input.
//
// Time is virtual: delay_ms() advances the clock instead of sleeping (and
// every clock read costs 20 us), so the run is fast and repeatable. The
// loader's tasks are refused, so frames are presented and sound is mixed on
// the app's own task.
//
// Checks, by frame since the project started (30 fps):
//    30  the sprite says "Hello from the P4!"
//    80  right arrow (D-pad) held for 30 frames: the sprite moves right,
//        the "moves" variable counts
//   120  space (A): next costume (the PNG), colour effect 25, the beep plays
//        and mixes to non-silent samples
//   130  a finger held at frame (120, 60): the sprite follows (mouse down)
//   160  a tap on the sprite: "when this sprite clicked" turns it 15 degrees
//   180  Menu+X: the app quits
// Frames 30, 115 and 152 are written as PNGs to $SCRATCH_HOST_OUT (default .).
//
//   usage: scratch_host_test   (needs /sd/scratch/papp_test.sb3)
#include "papp_port.h"

#include <audiostack.hpp>
#include <render.hpp>
#include <runtime.hpp>
#include <speech_manager.hpp>
#include <sprite.hpp>

#include <miniz.h>

#include <cmath>
#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

int papp_scratch_run(void);  // papp_scratch.cpp

// ── The fake loader ─────────────────────────────────────────────────────────

static long long s_now_us = 1000000;
static jmp_buf s_quit_jmp;
static int s_quit_code = 0;

extern "C" {
const app_services_t *papp_svc = nullptr;

long long papp_time_us(void)
{
    s_now_us += 20;
    return s_now_us;
}

void papp_yield_maybe(void) {}
void papp_free_all_memory(void) {}
void papp_close_all_files(void) {}
void *papp_alloc_raw(size_t size) { return malloc(size); }

void papp_quit(int code)
{
    s_quit_code = code;
    longjmp(s_quit_jmp, 1);
}
}

static int host_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

static int host_vlog(const char *fmt, va_list ap) { return vprintf(fmt, ap); }
static void host_delay(int ms) { s_now_us += (ms > 0 ? ms : 1) * 1000LL; }
static int64_t host_time() { return papp_time_us(); }

static void *host_open(const char *path, const char *mode) { return fopen(path, mode); }
static int host_close(void *f) { return fclose((FILE *)f); }
static size_t host_read(void *p, size_t s, size_t n, void *f) { return fread(p, s, n, (FILE *)f); }
static size_t host_write(const void *p, size_t s, size_t n, void *f) { return fwrite(p, s, n, (FILE *)f); }
static int host_seek(void *f, long o, int w) { return fseek((FILE *)f, o, w); }
static long host_tell(void *f) { return ftell((FILE *)f); }

static int host_list_dir(const char *path, char *buf, int len)
{
    DIR *dir = opendir(path);
    if (dir == nullptr) {
        return -1;
    }
    int count = 0, used = 0;
    while (struct dirent *e = readdir(dir)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        std::string name = e->d_name;
        struct stat st;
        if (stat((std::string(path) + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            name += "/";
        }
        if (used + (int)name.size() + 1 > len) {
            break;
        }
        memcpy(buf + used, name.c_str(), name.size() + 1);
        used += (int)name.size() + 1;
        count++;
    }
    closedir(dir);
    return count;
}

static int host_mkdir(const char *path)
{
    std::string p = path;
    for (size_t i = 1; i <= p.size(); i++) {
        if (i == p.size() || p[i] == '/') {
            mkdir(p.substr(0, i).c_str(), 0755);
        }
    }
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

static int host_stat(const char *path, papp_file_stat_t *out)
{
    memset(out, 0, sizeof(*out));
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    out->size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    out->mtime = st.st_mtime;
    return 0;
}

static int host_remove(const char *path) { return remove(path) == 0 ? 0 : -1; }
static int host_rename(const char *a, const char *b) { return rename(a, b) == 0 ? 0 : -1; }
static void *host_calloc(size_t n, size_t s) { return calloc(n, s); }
static void *host_caps_alloc(size_t size, uint32_t) { return malloc(size); }
static int host_task_create(void (*)(void *), const char *, uint32_t, void *, int, void *, int) { return -1; }
static void host_task_delete(void *) {}
static void host_get_size(int *w, int *h)
{
    if (w) *w = 1024;
    if (h) *h = 600;
}
static int host_set_canvas(int, int) { return 0; }
static void host_clear(uint16_t) {}
static void host_audio_init(int) {}
static void host_audio_submit(short *, int) {}
static int host_get_arg(char *buf, int len)
{
    if (len > 0) buf[0] = '\0';
    return 0;
}

// ── The script ──────────────────────────────────────────────────────────────

static int s_frame = -1;  // frames since the project started (-1: not yet)
static int s_touch_x = -1, s_touch_y = -1;  // frame coordinates, -1: none
static papp_gamepad_state_t s_pad;
static std::vector<std::string> s_failures;
static double s_x0 = 0;
static bool s_quit_requested = false;

static void fail(const std::string &what)
{
    printf("host: FAIL at frame %d: %s\n", s_frame, what.c_str());
    s_failures.push_back(what);
}

static void check(bool ok, const std::string &what)
{
    if (ok) {
        printf("host: ok   at frame %d: %s\n", s_frame, what.c_str());
    } else {
        fail(what);
    }
}

static Sprite *cat()
{
    for (Sprite *s : Scratch::sprites) {
        if (s->name == "Cat" && !s->isClone) {
            return s;
        }
    }
    return nullptr;
}

static double moves()
{
    if (Scratch::stageSprite == nullptr) {
        return -1;
    }
    auto it = Scratch::stageSprite->variables.find("papp-moves-var");
    return it == Scratch::stageSprite->variables.end() ? -1 : it->second.value.asDouble();
}

static void save_png(const uint16_t *frame, int w, int h)
{
    const char *dir = getenv("SCRATCH_HOST_OUT");
    std::vector<unsigned char> rgb((size_t)w * h * 3);
    for (int i = 0; i < w * h; i++) {
        const uint16_t p = frame[i];
        rgb[i * 3] = (unsigned char)(((p >> 11) & 31) * 255 / 31);
        rgb[i * 3 + 1] = (unsigned char)(((p >> 5) & 63) * 255 / 63);
        rgb[i * 3 + 2] = (unsigned char)((p & 31) * 255 / 31);
    }
    size_t size = 0;
    void *png = tdefl_write_image_to_png_file_in_memory(rgb.data(), w, h, 3, &size);
    const std::string path = std::string(dir ? dir : ".") + "/frame" + std::to_string(s_frame) + ".png";
    if (FILE *f = fopen(path.c_str(), "wb")) {
        fwrite(png, 1, size, f);
        fclose(f);
        printf("host: wrote %s\n", path.c_str());
    }
    mz_free(png);
}

// Every presented frame: advance the script.
static void host_frame(const uint16_t *buffer, uint16_t w, uint16_t h, float, bool)
{
    if (s_frame < 0) {
        if (cat() == nullptr) {
            return;  // the loading screen or the project list
        }
        s_frame = 0;
    }
    const int f = s_frame;
    Sprite *c = cat();
    memset(&s_pad, 0, sizeof(s_pad));
    s_touch_x = s_touch_y = -1;

    if (f == 30) {
        SpeechManager *sm = Render::getSpeechManager();
        check(sm != nullptr && c != nullptr && sm->getSpeechText(c) == "Hello from the P4!", "the sprite says hello");
        save_png(buffer, w, h);
    }
    if (f == 75 && c) {
        s_x0 = c->xPosition;
        check(std::fabs(c->xPosition) < 0.5 && std::fabs(c->yPosition) < 0.5, "the sprite starts at 0,0");
    }
    if (f >= 80 && f < 110) {
        s_pad.values[PAPP_INPUT_RIGHT] = 1;
    }
    if (f == 115 && c) {
        check(c->xPosition - s_x0 >= 60, "right arrow moved the sprite right (x=" + std::to_string(c->xPosition) + ")");
        check(moves() >= 15, "the moves variable counted (" + std::to_string(moves()) + ")");
        save_png(buffer, w, h);
    }
    if (f >= 120 && f < 122) {
        s_pad.values[PAPP_INPUT_A] = 1;
    }
    if (f == 126 && c) {
        check(c->currentCostume == 1, "space switched to the next costume");
        check(std::fabs(c->colorEffect - 25) < 0.01, "space changed the colour effect");
        const bool playing = !c->sounds.empty() && Mixer::isSoundPlaying(c->sounds[0].fullName);
        check(playing, "space started the beep");
        static short mix[2048 * 2];
        Mixer::requestSound(mix, 2048);
        int peak = 0;
        for (short s : mix) {
            peak = std::max(peak, std::abs((int)s));
        }
        check(peak > 1000, "the mixer produced sound (peak " + std::to_string(peak) + ")");
    }
    if (f >= 130 && f < 150) {
        s_touch_x = 120;
        s_touch_y = 60;
    }
    if (f == 150 && c) {
        check(std::fabs(c->xPosition + 120) < 3 && std::fabs(c->yPosition - 120) < 3,
              "a finger down made the sprite go to the pointer (" + std::to_string(c->xPosition) + ", " +
                  std::to_string(c->yPosition) + ")");
    }
    if (f == 152) {
        save_png(buffer, w, h);
    }
    if (f >= 160 && f < 162) {
        s_touch_x = 120;
        s_touch_y = 60;
    }
    if (f == 170 && c) {
        check(std::fabs(c->rotation - 105) < 0.01, "a tap on the sprite turned it (direction " + std::to_string(c->rotation) + ")");
    }
    if (f >= 180) {
        s_pad.values[PAPP_INPUT_MENU] = 1;
        s_pad.values[PAPP_INPUT_X] = 1;
        s_quit_requested = true;
    }
    if (f > 400) {
        fail("the app did not quit");
        papp_quit(-9);
    }
    s_frame++;
}

static void host_gamepad(papp_gamepad_state_t *state) { *state = s_pad; }

static int host_touch(int *x, int *y)
{
    if (s_touch_x < 0) {
        return 0;
    }
    // Frame -> the 800x600 canvas the app picks from the 1024x600 offer.
    if (x) *x = s_touch_x * 800 / 480 + 1;
    if (y) *y = s_touch_y * 600 / 360 + 1;
    return 1;
}

int main()
{
    static app_services_t svc;
    memset(&svc, 0, sizeof(svc));
    svc.abi_version = PAPP_ABI_VERSION;
    svc.display_write_frame_custom = host_frame;
    svc.display_clear = host_clear;
    svc.audio_init = host_audio_init;
    svc.audio_submit = host_audio_submit;
    svc.input_gamepad_read = host_gamepad;
    svc.file_open = host_open;
    svc.file_close = host_close;
    svc.file_read = host_read;
    svc.file_write = host_write;
    svc.file_seek = host_seek;
    svc.file_tell = host_tell;
    svc.mem_alloc = malloc;
    svc.mem_calloc = host_calloc;
    svc.mem_realloc = realloc;
    svc.mem_free = free;
    svc.mem_caps_alloc = host_caps_alloc;
    svc.log_printf = host_log;
    svc.log_vprintf = host_vlog;
    svc.delay_ms = host_delay;
    svc.get_time_us = host_time;
    svc.task_create = host_task_create;
    svc.task_delete = host_task_delete;
    svc.touch_read = host_touch;
    svc.display_get_size = host_get_size;
    svc.display_set_canvas = host_set_canvas;
    svc.app_get_arg = host_get_arg;
    svc.file_list_dir = host_list_dir;
    svc.file_mkdir = host_mkdir;
    svc.file_remove = host_remove;
    svc.file_rename = host_rename;
    svc.file_stat = host_stat;
    papp_svc = &svc;

    int code;
    if (setjmp(s_quit_jmp) == 0) {
        code = papp_scratch_run();
    } else {
        code = s_quit_code;
    }
    printf("host: app returned %d after %d project frames\n", code, s_frame);
    if (s_frame < 180) {
        fail("the project ran only " + std::to_string(s_frame) + " frames");
    }
    if (!s_quit_requested) {
        fail("the script never reached the quit");
    }
    if (code != 0) {
        fail("exit code " + std::to_string(code));
    }
    if (!s_failures.empty()) {
        printf("host: %zu check(s) failed\n", s_failures.size());
        return 1;
    }
    printf("host: all checks passed\n");
    return 0;
}
