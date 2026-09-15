// Scratch Everywhere! PAPP entry point.
//
// app_entry() runs on the loader's small worker task. It stores the service
// table, then starts the runtime on its own task with a large stack and
// waits. The game task runs the C++ global constructors (the runtime has
// many: block handler registration, static maps and strings) and then the
// app (papp_scratch.cpp). exit()/abort() and running out of memory end up in
// papp_quit(), which longjmps back to the game task's start.
#include "papp_port.h"

#include <new>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
const app_services_t *papp_svc = nullptr;
}

int papp_scratch_run(void);  // papp_scratch.cpp

typedef void (*init_fn)(void);
extern "C" init_fn __init_array_start[];
extern "C" init_fn __init_array_end[];

static jmp_buf s_exit_jmp;
static volatile bool s_game_running = false;
static volatile bool s_game_done = false;
static volatile int s_exit_code = 0;
static long long s_last_sleep_us = 0;

extern "C" long long papp_time_us(void)
{
    return papp_svc->get_time_us();
}

// delay_ms() below one tick (10 ms on this firmware) is only a yield to tasks
// of the same priority; the idle task needs a real sleep now and then.
extern "C" void papp_yield_maybe(void)
{
    const long long now = papp_time_us();
    if (now - s_last_sleep_us > 500000) {
        papp_svc->delay_ms(10);
        s_last_sleep_us = papp_time_us();
    }
}

extern "C" void papp_quit(int code)
{
    s_exit_code = code;
    if (s_game_running) {
        longjmp(s_exit_jmp, 1);
    }
    papp_svc->log_printf("SE: quit(%d) before the app started\n", code);
    for (;;) {
        papp_svc->delay_ms(1000);
    }
}

static void game_task(void *)
{
    if (setjmp(s_exit_jmp) == 0) {
        s_game_running = true;
        s_last_sleep_us = papp_time_us();
        const unsigned ctors = (unsigned)(__init_array_end - __init_array_start);
        papp_svc->log_printf("SE: running %u global constructors\n", ctors);
        for (init_fn *fn = __init_array_start; fn < __init_array_end; ++fn) {
            (*fn)();
        }
        s_exit_code = papp_scratch_run();
    }
    s_game_running = false;
    papp_svc->log_printf("SE: app ended (%d)\n", s_exit_code);
    // The loader does not reclaim an app's files or heap: hand everything back.
    // The audio and presenter tasks read the app's buffers, so they stop first.
    papp_audio_shutdown();
    papp_video_shutdown();
    papp_close_all_files();
    papp_free_all_memory();
    s_game_done = true;
    // The loader deletes this task; returning from a FreeRTOS task aborts.
    for (;;) {
        papp_svc->delay_ms(1000);
    }
}

extern "C" __attribute__((section(".text.entry"), used)) int app_entry(const app_services_t *svc)
{
    papp_svc = svc;
    svc->log_printf("SE: Scratch Everywhere! PAPP starting\n");

    void *handle = nullptr;
    // The runtime evaluates nested reporters recursively and keeps big locals
    // (JSON parsing); the loader puts stacks this big in PSRAM. Core 0 like
    // the loader's own worker; core 1 runs ESPHome's loop and the presenter
    // and audio tasks. Priority 4, one below the loader's screen-stream task
    // on this core (as OpenLara).
    if (svc->task_create(game_task, "scratch", 256 * 1024, nullptr, 4, &handle, 0) != 0) {
        svc->log_printf("SE: could not create the app task\n");
        return -1;
    }
    while (!s_game_done) {
        svc->delay_ms(50);
    }
    svc->delay_ms(50);
    if (handle != nullptr) {
        svc->task_delete(handle);
    }
    return s_exit_code;
}

// ── C++ runtime ─────────────────────────────────────────────────────────────
// No exceptions: running out of memory ends the app instead of throwing.

static void *checked_alloc(size_t size)
{
    void *p = malloc(size != 0 ? size : 1);
    if (p == nullptr) {
        papp_svc->log_printf("SE: out of memory (%u bytes)\n", (unsigned)size);
        papp_quit(-2);
    }
    return p;
}

// Over-aligned objects: malloc gives 16-byte aligned blocks; anything more
// is carved out of a bigger block with the original pointer stored in front.
static void *aligned_new(size_t size, size_t align)
{
    if (align <= 16) {
        return checked_alloc(size);
    }
    unsigned char *raw = static_cast<unsigned char *>(checked_alloc(size + align + sizeof(void *)));
    uintptr_t p = (reinterpret_cast<uintptr_t>(raw) + sizeof(void *) + align - 1) & ~(uintptr_t)(align - 1);
    reinterpret_cast<void **>(p)[-1] = raw;
    return reinterpret_cast<void *>(p);
}

static void aligned_delete(void *p, size_t align)
{
    if (p == nullptr) {
        return;
    }
    if (align <= 16) {
        free(p);
    } else {
        free(reinterpret_cast<void **>(p)[-1]);
    }
}

void *operator new(size_t size) { return checked_alloc(size); }
void *operator new[](size_t size) { return checked_alloc(size); }
void *operator new(size_t size, const std::nothrow_t &) noexcept { return malloc(size != 0 ? size : 1); }
void *operator new[](size_t size, const std::nothrow_t &) noexcept { return malloc(size != 0 ? size : 1); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }
void *operator new(size_t size, std::align_val_t al) { return aligned_new(size, (size_t)al); }
void *operator new[](size_t size, std::align_val_t al) { return aligned_new(size, (size_t)al); }
void operator delete(void *p, std::align_val_t al) noexcept { aligned_delete(p, (size_t)al); }
void operator delete[](void *p, std::align_val_t al) noexcept { aligned_delete(p, (size_t)al); }
void operator delete(void *p, size_t, std::align_val_t al) noexcept { aligned_delete(p, (size_t)al); }
void operator delete[](void *p, size_t, std::align_val_t al) noexcept { aligned_delete(p, (size_t)al); }

extern "C" void __cxa_pure_virtual(void)
{
    papp_svc->log_printf("SE: pure virtual call\n");
    papp_quit(-1);
}
