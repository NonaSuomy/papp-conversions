// Audio for the Scratch Everywhere! PAPP (the runtime's "audio engine").
//
// The runtime mixes every playing sound itself (Mixer::requestSound, 48 kHz
// stereo). The app task calls papp_audio_pump() once per frame to top up a
// ring buffer, so all runtime code stays on one task (its heap and mixer
// have no locks here). This file's task only moves those frames to the
// loader's speaker, paced to real time with a small lead, and fills gaps (a
// project loading) with silence so the speaker chain keeps running. Same
// scheme as the OpenLara port (ports/openlara/papp_audio.cpp).
#include "papp_port.h"

#include <audio.hpp>
#include <audiostack.hpp>

#include <string.h>

enum {
    RING_FRAMES = 16384,   // ~340 ms
    TARGET_FRAMES = 4800,  // the app keeps ~100 ms queued
    BLOCK_FRAMES = 1024,   // per audio_submit: ~21 ms
    PUMP_FRAMES = 1024,    // per Mixer::requestSound call
};

static int16_t *s_ring = nullptr;     // RING_FRAMES stereo frames
static volatile unsigned s_head = 0;  // frames written (app task)
static volatile unsigned s_tail = 0;  // frames read (audio task)
static volatile bool s_quit = false;
static volatile bool s_done = true;
static void *s_task = nullptr;

// How far the output may run ahead of real time. audio_submit() only blocks
// once the speaker chain is full, which would add its whole buffer as delay.
static const long long LEAD_US = 120000;
static const int TICK_MS = 10;  // one FreeRTOS tick on this firmware (100 Hz)

static unsigned queued(void)
{
    return s_head - s_tail;
}

static void audio_task(void *)
{
    static int16_t block[BLOCK_FRAMES * 2];
    long long anchor_us = 0;   // when the current run of audio started
    long long frames_out = 0;  // frames submitted since anchor_us
    while (!s_quit) {
        const long long now = papp_time_us();
        const long long played_us = frames_out * 1000000LL / PAPP_AUDIO_RATE;
        if (frames_out == 0 || now - anchor_us > played_us + 250000) {
            anchor_us = now;  // a new run, or we fell far behind: start over
            frames_out = 0;
        } else {
            const long long ahead = played_us - (now - anchor_us);
            if (ahead > LEAD_US) {
                const int ms = static_cast<int>((ahead - LEAD_US) / 1000) + 1;
                papp_svc->delay_ms(ms < TICK_MS ? TICK_MS : ms);
                continue;
            }
            // Not yet a full block and still some output queued downstream:
            // give the app a moment to top the ring up.
            if (queued() < BLOCK_FRAMES && ahead > 40000) {
                papp_svc->delay_ms(TICK_MS);
                continue;
            }
        }
        unsigned n = queued();
        if (n > BLOCK_FRAMES) {
            n = BLOCK_FRAMES;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        for (unsigned i = 0; i < n; i++) {
            const unsigned at = ((s_tail + i) % RING_FRAMES) * 2;
            block[i * 2] = s_ring[at];
            block[i * 2 + 1] = s_ring[at + 1];
        }
        __atomic_thread_fence(__ATOMIC_RELEASE);
        s_tail += n;
        if (n < BLOCK_FRAMES) {
            memset(block + n * 2, 0, (BLOCK_FRAMES - n) * 2 * sizeof(int16_t));  // underrun: silence
        }
        frames_out += BLOCK_FRAMES;
        papp_svc->audio_submit(block, BLOCK_FRAMES);
    }
    s_done = true;
    for (;;) {
        papp_svc->delay_ms(1000);  // deleted by papp_audio_shutdown
    }
}

extern "C" int papp_audio_start(void)
{
    if (s_task != nullptr) {
        return 0;
    }
    if (papp_svc->audio_init == nullptr || papp_svc->audio_submit == nullptr) {
        return -1;
    }
    s_ring = static_cast<int16_t *>(papp_alloc_raw(RING_FRAMES * 2 * sizeof(int16_t)));
    if (s_ring == nullptr) {
        papp_svc->log_printf("SE: no memory for the audio ring\n");
        return -1;
    }
    s_head = s_tail = 0;
    papp_svc->audio_init(PAPP_AUDIO_RATE);
    s_quit = false;
    s_done = false;
    // Core 1 next to ESPHome's loop and the presenter; it mostly sleeps or
    // waits in audio_submit.
    if (papp_svc->task_create(audio_task, "se_audio", 8 * 1024, nullptr, 6, &s_task, 1) != 0) {
        s_task = nullptr;
        s_done = true;
        papp_svc->mem_free(s_ring);
        s_ring = nullptr;
        papp_svc->log_printf("SE: could not start the audio task\n");
        return -1;
    }
    papp_svc->log_printf("SE: sound %d Hz\n", PAPP_AUDIO_RATE);
    return 0;
}

// App task, once per frame: mix what the ring needs to reach TARGET_FRAMES.
extern "C" void papp_audio_pump(void)
{
    if (s_task == nullptr) {
        return;
    }
    static int16_t mix[PUMP_FRAMES * 2];
    unsigned wanted = queued() >= TARGET_FRAMES ? 0 : TARGET_FRAMES - queued();
    while (wanted > 0) {
        const unsigned n = wanted < PUMP_FRAMES ? wanted : PUMP_FRAMES;
        Mixer::requestSound(mix, static_cast<int>(n));
        const unsigned head = s_head;
        for (unsigned i = 0; i < n; i++) {
            const unsigned at = ((head + i) % RING_FRAMES) * 2;
            s_ring[at] = mix[i * 2];
            s_ring[at + 1] = mix[i * 2 + 1];
        }
        __atomic_thread_fence(__ATOMIC_RELEASE);
        s_head = head + n;
        wanted -= n;
    }
}

extern "C" void papp_audio_shutdown(void)
{
    if (s_task != nullptr) {
        s_quit = true;
        for (int i = 0; i < 100 && !s_done; i++) {
            papp_svc->delay_ms(10);
        }
        papp_svc->task_delete(s_task);
        s_task = nullptr;
    }
    if (s_ring != nullptr) {
        papp_svc->mem_free(s_ring);
        s_ring = nullptr;
    }
}

// ── The runtime's audio engine interface (include/audio.hpp) ────────────────

bool SoundPlayer::init()
{
    return papp_audio_start() == 0;
}

void SoundPlayer::deinit()
{
    Mixer::cleanupAudio();
}
