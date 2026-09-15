// Input for the Scratch Everywhere! PAPP (the runtime's "windowing" input:
// Input::getInput and friends, include/input.hpp).
//
//   touch      the mouse: position and button (a finger down = pressed)
//   USB mouse  moves the mouse pointer (drawn while in use), buttons press
//   D-pad      the arrow keys (the loader also turns keyboard arrows/WASD
//              into the D-pad)
//   A          space (the loader also maps keyboard Space/Enter/Z to A)
//   B X Y L R  the keys b x y l r (Scratch Everywhere's default controls)
//   Start      green flag (restart); keyboard Enter (Start+A) is "enter"
//   Select     held 1 s: stop the project, back to the project list
//   keyboard   other keys arrive as taps and are held for a few frames
//   Menu+X, L3 or Menu held 3 s: quit the app
#include "papp_port.h"
#include "papp_raster.h"
#include "papp_render.h"

#include <blockExecutor.hpp>
#include <input.hpp>
#include <log.hpp>
#include <os.hpp>
#include <render.hpp>
#include <runtime.hpp>

#include <algorithm>
#include <cstring>
#include <string>

extern "C" {
papp_input_t papp_input;
}

// Loader key numbers for keys that are not printable ASCII (papp_loader.cpp
// enqueue_keyboard_text).
enum {
    LK_TAB = 9,
    LK_ENTER = 13,
    LK_ESCAPE = 27,
    LK_BACKSPACE = 127,
    LK_CTRL = 133,
    LK_SHIFT = 134,
    LK_MAX = 256,
};

// The loader reports keyboard keys (other than the ones it turns into
// gamepad inputs) as taps: hold each for TAP_US so scripts see it.
static const long long TAP_US = 150000;
static long long s_key_until[LK_MAX];

// Typed characters for the "ask" prompt (a small ring).
static int s_typed[32];
static unsigned s_typed_head = 0, s_typed_tail = 0;

static long long s_menu_since = 0;
static long long s_select_since = 0;
static bool s_start_is_enter = false;
static bool s_green_flag = false;   // Start pressed: restart the project
static bool s_back_to_list = false; // Select held: stop the project

// USB mouse: pointer in frame coordinates while it is in use.
static int s_mouse_x = PAPP_FRAME_W / 2, s_mouse_y = PAPP_FRAME_H / 2;
static int s_mouse_buttons = 0;
static long long s_mouse_seen = 0;

extern "C" void papp_input_clear_actions(void)
{
    s_green_flag = false;
    s_back_to_list = false;
}

extern "C" int papp_input_pressed(int input)
{
    return papp_input.pad.values[input] && !papp_input.prev.values[input];
}

extern "C" void papp_input_poll(void)
{
    const long long now = papp_time_us();
    papp_input.prev = papp_input.pad;
    memset(&papp_input.pad, 0, sizeof(papp_input.pad));
    if (papp_svc->input_gamepad_read != nullptr) {
        papp_svc->input_gamepad_read(&papp_input.pad);
    }
    const int *pad = papp_input.pad.values;

    // Leaving: the loader's close control (Menu+X, or its L3 service), or
    // Menu held for 3 s.
    if ((papp_svc->input_l3_read != nullptr && papp_svc->input_l3_read()) || (pad[PAPP_INPUT_MENU] && pad[PAPP_INPUT_X])) {
        papp_input.quit = 1;
    }
    if (pad[PAPP_INPUT_MENU]) {
        if (s_menu_since == 0) {
            s_menu_since = now;
        } else if (now - s_menu_since >= 3000000) {
            papp_input.quit = 1;
        }
    } else {
        s_menu_since = 0;
    }

    // Keyboard Enter arrives as Start and A together: that is "enter", not
    // the green flag.
    const bool a_pressed = pad[PAPP_INPUT_A] && !papp_input.prev.values[PAPP_INPUT_A];
    const bool start_pressed = pad[PAPP_INPUT_START] && !papp_input.prev.values[PAPP_INPUT_START];
    if (start_pressed && a_pressed) {
        s_start_is_enter = true;
    } else if (!pad[PAPP_INPUT_START]) {
        s_start_is_enter = false;
    } else if (start_pressed) {
        s_green_flag = true;
    }
    if (pad[PAPP_INPUT_SELECT]) {
        if (s_select_since == 0) {
            s_select_since = now;
        } else if (now - s_select_since >= 1000000) {
            s_back_to_list = true;
            s_select_since = now + 60000000;  // once per hold
        }
    } else {
        s_select_since = 0;
    }

    // Touch, in frame coordinates.
    int tx = 0, ty = 0;
    papp_input.touching = 0;
    if (papp_svc->touch_read != nullptr && papp_svc->touch_read(&tx, &ty)) {
        int fx, fy;
        if (papp_video_canvas_to_frame(tx, ty, &fx, &fy)) {
            papp_input.touching = 1;
            papp_input.touch_x = fx;
            papp_input.touch_y = fy;
        }
    }

    // USB keyboard taps.
    if (papp_svc->input_keyboard_read != nullptr) {
        papp_keyboard_event_t event;
        while (papp_svc->input_keyboard_read(&event)) {
            if (!event.down || event.key <= 0 || event.key >= LK_MAX) {
                continue;  // released after TAP_US instead
            }
            s_key_until[event.key] = now + TAP_US;
            if (s_typed_head - s_typed_tail < sizeof(s_typed) / sizeof(s_typed[0])) {
                s_typed[s_typed_head++ % (sizeof(s_typed) / sizeof(s_typed[0]))] = event.key;
            }
        }
    }

    // USB mouse.
    int dx = 0, dy = 0, buttons = 0;
    if (papp_svc->input_mouse_read != nullptr && papp_svc->input_mouse_read(&dx, &dy, &buttons)) {
        s_mouse_x = std::clamp(s_mouse_x + dx, 0, PAPP_FRAME_W - 1);
        s_mouse_y = std::clamp(s_mouse_y + dy, 0, PAPP_FRAME_H - 1);
        s_mouse_buttons = buttons;
        s_mouse_seen = now;
    }
}

static bool key_held(int key, long long now)
{
    return key > 0 && key < LK_MAX && s_key_until[key] > now;
}

// Scratch's name for a loader key number ("" for none).
static std::string key_name(int key)
{
    if (key == ' ') {
        return "space";
    }
    if (key == LK_ENTER) {
        return "enter";
    }
    if (key == LK_SHIFT) {
        return "shift";
    }
    if (key == LK_CTRL) {
        return "control";
    }
    if (key > ' ' && key < 127) {
        const char c = (key >= 'A' && key <= 'Z') ? (char)(key - 'A' + 'a') : (char)key;
        return std::string(1, c);
    }
    return "";
}

static void press_key(const std::string &key)
{
    if (!key.empty() &&
        std::find(Input::inputKeys.begin(), Input::inputKeys.end(), key) == Input::inputKeys.end()) {
        Input::inputKeys.push_back(key);
    }
}

std::array<int, 2> Input::getTouchPosition()
{
    if (papp_input.touching) {
        return {papp_input.touch_x, papp_input.touch_y};
    }
    return {s_mouse_x, s_mouse_y};
}

void Input::getInput()
{
    papp_render_mark_frame();
    papp_input_poll();
    const long long now = papp_time_us();
    const int *pad = papp_input.pad.values;

    inputButtons.clear();
    inputKeys.clear();
    mousePointer.isPressed = false;

    if (papp_input.quit) {
        OS::toExit = true;
    }
    if (s_green_flag) {
        s_green_flag = false;
        Scratch::greenFlagClicked();
    }
    if (s_back_to_list) {
        s_back_to_list = false;
        Scratch::shouldStop = true;
    }

    // The gamepad: D-pad = arrows, A = space, the rest by Scratch
    // Everywhere's default controls (also reported as controller buttons for
    // its own extension blocks).
    static const struct {
        int input;
        SCRATCH_KEY_INDEX button;
        const char *key;  // nullptr: the default control
    } PAD_MAP[] = {
        {PAPP_INPUT_UP, SCRATCH_KEY_INDEX::DPAD_UP, "up arrow"},
        {PAPP_INPUT_DOWN, SCRATCH_KEY_INDEX::DPAD_DOWN, "down arrow"},
        {PAPP_INPUT_LEFT, SCRATCH_KEY_INDEX::DPAD_LEFT, "left arrow"},
        {PAPP_INPUT_RIGHT, SCRATCH_KEY_INDEX::DPAD_RIGHT, "right arrow"},
        {PAPP_INPUT_A, SCRATCH_KEY_INDEX::A, "space"},
        {PAPP_INPUT_B, SCRATCH_KEY_INDEX::B, nullptr},
        {PAPP_INPUT_X, SCRATCH_KEY_INDEX::X, nullptr},
        {PAPP_INPUT_Y, SCRATCH_KEY_INDEX::Y, nullptr},
        {PAPP_INPUT_L, SCRATCH_KEY_INDEX::SHOULDER_L, nullptr},
        {PAPP_INPUT_R, SCRATCH_KEY_INDEX::SHOULDER_R, nullptr},
    };
    const bool enter = s_start_is_enter && pad[PAPP_INPUT_START];
    for (const auto &m : PAD_MAP) {
        if (!pad[m.input] || pad[PAPP_INPUT_MENU]) {
            continue;
        }
        if (m.input == PAPP_INPUT_A && enter) {
            continue;  // keyboard Enter, not A
        }
        inputButtons.push_back(CONTROLLER_STRINGS[static_cast<int>(m.button)]);
        if (m.key != nullptr) {
            press_key(m.key);
        } else {
            auto it = inputControls.find(CONTROLLER_STRINGS[static_cast<int>(m.button)]);
            if (it != inputControls.end()) {
                press_key(it->second);
            }
        }
    }
    if (enter) {
        press_key("enter");
    }
    if (pad[PAPP_INPUT_START] && !enter) {
        inputButtons.push_back(CONTROLLER_STRINGS[static_cast<int>(SCRATCH_KEY_INDEX::START)]);
    }
    if (pad[PAPP_INPUT_SELECT]) {
        inputButtons.push_back(CONTROLLER_STRINGS[static_cast<int>(SCRATCH_KEY_INDEX::BACK)]);
    }

    // Keyboard taps still being held.
    for (int key = 1; key < LK_MAX; key++) {
        if (key_held(key, now)) {
            press_key(key_name(key));
        }
    }

    if (!inputKeys.empty()) {
        inputKeys.push_back("any");
    }
    BlockExecutor::executeKeyHats();

    // The mouse: a finger on the panel, else the USB mouse.
    if (papp_input.touching) {
        auto coords = Scratch::screenToScratchCoords(papp_input.touch_x, papp_input.touch_y, Render::getWidth(),
                                                      Render::getHeight());
        mousePointer.x = coords.first;
        mousePointer.y = coords.second;
        mousePointer.isPressed = true;
        mousePointer.mouseButton = Mouse::LEFT;
    } else if (s_mouse_seen != 0) {
        auto coords = Scratch::screenToScratchCoords(s_mouse_x, s_mouse_y, Render::getWidth(), Render::getHeight());
        mousePointer.x = coords.first;
        mousePointer.y = coords.second;
        mousePointer.isPressed = (s_mouse_buttons & 3) != 0;
        mousePointer.mouseButton = (s_mouse_buttons & 2) ? Mouse::RIGHT : ((s_mouse_buttons & 4) ? Mouse::MIDDLE : Mouse::LEFT);
    }
    BlockExecutor::doSpriteClicking();
}

void papp_draw_mouse_pointer()
{
    if (s_mouse_seen == 0 || papp_time_us() - s_mouse_seen > 5000000) {
        return;
    }
    const raster::Target &t = papp_render_target();
    raster::fillRect(t, s_mouse_x - 1, s_mouse_y - 4, 3, 9, 0, 0, 0, 255);
    raster::fillRect(t, s_mouse_x - 4, s_mouse_y - 1, 9, 3, 0, 0, 0, 255);
    raster::fillRect(t, s_mouse_x, s_mouse_y - 3, 1, 7, 255, 255, 255, 255);
    raster::fillRect(t, s_mouse_x - 3, s_mouse_y, 7, 1, 255, 255, 255, 255);
}

// "ask and wait": Scratch's prompt at the bottom of the stage. Type on a USB
// keyboard; Enter (or A/Start, or a tap on the check mark) answers, Escape
// answers with nothing.
std::string Input::openSoftwareKeyboard(const char *hintText)
{
    std::string answer;
    s_typed_tail = s_typed_head;
    const std::string question = hintText != nullptr ? hintText : "";
    bool touched = true;   // wait for the finger that may have tapped to ask
    bool keyboard = false;  // typed on a USB keyboard: only Enter answers (its Space and Z also press A)
    for (;;) {
        papp_input_poll();
        if (papp_input.quit) {
            OS::toExit = true;
            return "";
        }
        while (s_typed_tail != s_typed_head) {
            const int key = s_typed[s_typed_tail++ % (sizeof(s_typed) / sizeof(s_typed[0]))];
            keyboard = true;
            if (key == LK_ENTER) {
                return answer;
            }
            if (key == LK_ESCAPE) {
                return "";
            }
            if (key == LK_BACKSPACE || key == 8) {
                // Drop one UTF-8 character.
                while (!answer.empty() && (answer.back() & 0xC0) == 0x80) {
                    answer.pop_back();
                }
                if (!answer.empty()) {
                    answer.pop_back();
                }
            } else if (key >= ' ' && key < 127 && answer.size() < 200) {
                answer += (char)key;
            }
        }
        if (!keyboard && (papp_input_pressed(PAPP_INPUT_A) || papp_input_pressed(PAPP_INPUT_START))) {
            return answer;
        }

        // The stage as it is, with the prompt over its bottom.
        papp_draw_stage();
        const raster::Target &t = papp_render_target();
        const int boxH = question.empty() ? 36 : 52;
        const int bx = 8, by = PAPP_FRAME_H - boxH - 8, bw = PAPP_FRAME_W - 16;
        raster::fillRoundRect(t, bx - 1, by - 1, bw + 2, boxH + 2, 9, 0xC0, 0xC4, 0xCC, 255);
        raster::fillRoundRect(t, bx, by, bw, boxH, 8, 255, 255, 255, 255);
        int y = by + 6;
        if (!question.empty()) {
            raster::text(t, question.c_str(), bx + 10, y, 8, 0x57, 0x5E, 0x75, 255);
            y += 16;
        }
        const int fieldX = bx + 8, fieldW = bw - 16 - 30;
        raster::fillRoundRect(t, fieldX, y, fieldW, 22, 10, 0xD9, 0xD9, 0xD9, 255);
        raster::fillRoundRect(t, fieldX + 1, y + 1, fieldW - 2, 20, 9, 255, 255, 255, 255);
        const bool blink = (papp_time_us() / 500000) % 2 == 0;
        const std::string shown = answer + (blink ? "_" : " ");
        raster::text(t, shown.c_str(), fieldX + 8, y + 6, 8, 0x57, 0x5E, 0x75, 255);
        // The check mark button.
        const int cx = fieldX + fieldW + 4, cy = y - 1;
        raster::fillRoundRect(t, cx, cy, 24, 24, 12, 0x85, 0x5C, 0xD6, 255);
        raster::text(t, "OK", cx + 4, cy + 8, 8, 255, 255, 255, 255);
        papp_frame_end(30);

        if (papp_input.touching) {
            if (!touched && papp_input.touch_x >= cx && papp_input.touch_x < cx + 24 && papp_input.touch_y >= cy &&
                papp_input.touch_y < cy + 24) {
                return answer;
            }
            touched = true;
        } else {
            touched = false;
        }
    }
}
