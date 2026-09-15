// Internals of the Scratch Everywhere! PAPP renderer shared by its files
// (papp_render.cpp, papp_image.cpp, papp_text.cpp, papp_input.cpp and the
// app's own screens in papp_scratch.cpp).
#pragma once

#include "papp_raster.h"

// Where Image::render, text objects and Render::drawBox draw right now: the
// frame being built, or the pen layer while a sprite is stamped.
const raster::Target &papp_render_target();

// The frame being built (papp_video_back()), cleared to one colour.
const raster::Target &papp_frame_begin(uint8_t r, uint8_t g, uint8_t b);
// Hand the frame to the presenter, top up the audio, then wait for the next
// frame slot (fps) so the app task does not spin.
void papp_frame_end(int fps);

// Draw the stage (sprites, pen, speech, monitors) into the current frame
// without presenting it.
void papp_draw_stage();

// The runtime started a frame (Input::getInput runs right after its frame
// timer fires): the next one is due 1/FPS s later.
void papp_render_mark_frame();

// A small pointer for the USB mouse, when one moved recently (papp_input.cpp).
void papp_draw_mouse_pointer();
