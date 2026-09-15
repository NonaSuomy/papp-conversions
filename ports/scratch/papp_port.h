// Scratch Everywhere! on the ESP32-P4 PAPP loader: shared declarations for
// the port's glue code (platform, renderer, audio and input backends).
// Nothing here is part of Scratch Everywhere! itself.
#pragma once

#ifndef PAPP_APP_SIDE
#define PAPP_APP_SIDE 1
#endif
// ESP-IDF builds get this C++ spelling from newlib's <sys/cdefs.h>; the loader
// header uses the C11 keyword.
#if defined(__cplusplus) && !defined(_Static_assert)
#define _Static_assert static_assert
#endif

#include "esphome/components/papp_loader/psram_app.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The loader's service table, set by app_entry before anything else runs.
extern const app_services_t *papp_svc;

// Projects (*.sb3) are looked for here; settings go here too. Ends in '/'.
#define PAPP_SCRATCH_DIR "/sd/scratch/"
// Optional upstream assets (gfx/ingame/fonts/*.ttf for SVG text,
// gfx/ingame/scratch.sf2 for the Music extension) are read from this folder,
// Scratch Everywhere's "RomFS". Ends in '/'.
#define PAPP_SCRATCH_ROMFS "/sd/scratch/se/"

// The stage is always drawn into a 480x360 RGB565 frame (Scratch's native
// stage size; other project sizes are letterboxed into it by the runtime)
// and the loader scales it to the canvas with the P4's PPA.
#define PAPP_FRAME_W 480
#define PAPP_FRAME_H 360

// The runtime mixes 48 kHz stereo (Mixer::rate).
#define PAPP_AUDIO_RATE 48000

// Leave the app and return to the loader (exit(), abort(), out of memory).
void papp_quit(int code) __attribute__((noreturn));

// Microseconds since boot.
long long papp_time_us(void);

// Sleep one real FreeRTOS tick when the game task has not slept for a while,
// so the idle task (and its watchdog) on this core still runs. Game task only.
void papp_yield_maybe(void);

// Release every heap block / file the app still holds (papp_syscalls.c).
void papp_free_all_memory(void);
void papp_close_all_files(void);

// Heap allocation that bypasses the leak list (buffers shared with other
// tasks, freed explicitly with papp_svc->mem_free). PSRAM.
void *papp_alloc_raw(size_t size);

// ── Video (papp_video.cpp) ────────────────────────────────────────────────
// Two RGB565 frames of PAPP_FRAME_W x PAPP_FRAME_H: the game draws into
// papp_video_back() while a presenter task on the other core scales the
// previous one to the canvas. papp_video_init picks the canvas.
int papp_video_init(void);
uint16_t *papp_video_back(void);
void papp_video_present(void);  // hand the back buffer over; papp_video_back() is the next one
void papp_video_shutdown(void);
// Canvas -> frame coordinates (touch). Returns 0 when outside the frame.
int papp_video_canvas_to_frame(int cx, int cy, int *fx, int *fy);

// ── Audio (papp_audio.cpp) ────────────────────────────────────────────────
// A ring of stereo frames the game task fills from the runtime's mixer
// (papp_audio_pump, once per frame) and a task that feeds it to the loader's
// speaker in real time. Only the game task touches the runtime.
int papp_audio_start(void);
void papp_audio_pump(void);
void papp_audio_shutdown(void);

// ── Input (papp_input.cpp) ────────────────────────────────────────────────
// The gamepad, keyboard, mouse and touch as last read (papp_input_poll, once
// per frame from Input::getInput and the app's own screens).
typedef struct {
    papp_gamepad_state_t pad;   // this frame
    papp_gamepad_state_t prev;  // last frame
    int touching;               // a finger is down
    int touch_x, touch_y;       // frame coordinates (0..479, 0..359)
    int quit;                   // the player asked to leave the app
} papp_input_t;
extern papp_input_t papp_input;
void papp_input_poll(void);
// Pressed this frame (not held in the last one).
int papp_input_pressed(int input);
// Forget a pending green flag / back-to-list request (a project starts).
void papp_input_clear_actions(void);

#ifdef __cplusplus
}
#endif
