// The Scratch Everywhere! PAPP itself: find the projects on the card, let
// the player pick one (touch or gamepad), load it and run it until it stops
// or the player quits. Replaces upstream's main.cpp and its menus.
//
// Projects are the .sb3 files in /sd/scratch/. With exactly one there it
// starts at once; with several a list comes up. A launch argument naming an
// .sb3 file (app_open / the loader's launch argument) starts that one.
#include "papp_port.h"
#include "papp_raster.h"
#include "papp_render.h"

#include <audio.hpp>
#include <log.hpp>
#include <os.hpp>
#include <render.hpp>
#include <runtime.hpp>
#include <unzip.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

static bool ends_with_sb3(const std::string &name)
{
    if (name.size() < 4) {
        return false;
    }
    std::string ext = name.substr(name.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".sb3";
}

static std::vector<std::string> list_projects()
{
    std::vector<std::string> names;
    if (papp_svc->file_list_dir == nullptr) {
        return names;
    }
    std::vector<char> buf(16384);
    int n = papp_svc->file_list_dir(PAPP_SCRATCH_DIR, buf.data(), (int)buf.size());
    const char *p = buf.data();
    for (; n > 0; n--, p += strlen(p) + 1) {
        const std::string name = p;
        if (!name.empty() && name.back() != '/' && name[0] != '.' && ends_with_sb3(name)) {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
            return std::tolower((unsigned char)x) < std::tolower((unsigned char)y);
        });
    });
    return names;
}

// ── The app's own screens (drawn with the rasteriser, 30 fps) ──────────────

static const uint8_t BG[3] = {0x85, 0x5C, 0xD6};  // Scratch purple

static void header(const raster::Target &t, const char *title)
{
    raster::fillRect(t, 0, 0, PAPP_FRAME_W, 34, BG[0], BG[1], BG[2], 255);
    raster::text(t, title, 12, 9, 16, 255, 255, 255, 255);
}

static void centred(const raster::Target &t, const std::string &line, int y, float px, uint8_t r, uint8_t g,
                    uint8_t b)
{
    int w = 0;
    raster::textSize(line.c_str(), px, &w, nullptr);
    raster::text(t, line.c_str(), (PAPP_FRAME_W - w) / 2, y, px, r, g, b, 255);
}

static void show_message(const std::string &title, const std::vector<std::string> &lines)
{
    const raster::Target &t = papp_frame_begin(0xF9, 0xF9, 0xFB);
    header(t, title.c_str());
    int y = 70;
    for (const std::string &line : lines) {
        centred(t, line, y, 8, 0x57, 0x5E, 0x75);
        y += 16;
    }
    papp_frame_end(30);
}

static void show_loading(const std::string &name)
{
    // Twice: the presenter shows the frame before last.
    for (int i = 0; i < 2; i++) {
        show_message("Scratch Everywhere!", {"Loading", name, "", "This can take a few seconds."});
    }
}

// Shown until any button or a tap; false when the player quit instead.
static bool wait_for_ok(const std::string &title, const std::vector<std::string> &lines)
{
    bool touched = true;
    for (;;) {
        papp_input_poll();
        if (papp_input.quit) {
            return false;
        }
        for (int i = 0; i < PAPP_INPUT_MAX; i++) {
            if (i != PAPP_INPUT_MENU && papp_input_pressed(i)) {
                return true;
            }
        }
        if (papp_input.touching && !touched) {
            return true;
        }
        touched = papp_input.touching != 0;
        show_message(title, lines);
    }
}

// The project list: returns the index to run, or -1 to quit the app.
static int pick_project(const std::vector<std::string> &names)
{
    const int rowH = 26, top = 42, visible = (PAPP_FRAME_H - top - 22) / rowH;
    static int selected = 0;
    int first = 0;
    selected = std::clamp(selected, 0, std::max(0, (int)names.size() - 1));
    int touchRow = -1;         // the row a finger went down on
    long long repeatAt = 0;    // D-pad auto-repeat
    bool touchedAtStart = true;  // ignore a finger still down from before
    for (;;) {
        papp_input_poll();
        if (papp_input.quit) {
            return -1;
        }
        const long long now = papp_time_us();
        const int *pad = papp_input.pad.values;
        const int n = (int)names.size();
        if (n > 0) {
            int dir = 0;
            if (papp_input_pressed(PAPP_INPUT_UP) || papp_input_pressed(PAPP_INPUT_DOWN)) {
                dir = pad[PAPP_INPUT_UP] ? -1 : 1;
                repeatAt = now + 400000;
            } else if ((pad[PAPP_INPUT_UP] || pad[PAPP_INPUT_DOWN]) && now >= repeatAt) {
                dir = pad[PAPP_INPUT_UP] ? -1 : 1;
                repeatAt = now + 90000;
            }
            if (dir != 0) {
                selected = (selected + dir + n) % n;
            }
            if (papp_input_pressed(PAPP_INPUT_A) || papp_input_pressed(PAPP_INPUT_START)) {
                return selected;
            }
        } else if (papp_input_pressed(PAPP_INPUT_A) || papp_input_pressed(PAPP_INPUT_START)) {
            return -2;  // look again
        }
        // Touch: a tap on a row runs it (on release, on the same row).
        if (papp_input.touching) {
            const int row = (papp_input.touch_y - top) / rowH;
            const int index = papp_input.touch_y >= top ? first + row : -1;
            if (!touchedAtStart && touchRow == -1) {
                touchRow = index >= 0 && index < n && row < visible ? index : -2;
            }
            if (touchRow >= 0) {
                selected = touchRow;
            }
        } else {
            if (touchRow >= 0) {
                return touchRow;
            }
            if (touchRow == -2 && n == 0) {
                return -2;
            }
            touchRow = -1;
            touchedAtStart = false;
        }
        if (selected < first) {
            first = selected;
        } else if (selected >= first + visible) {
            first = selected - visible + 1;
        }

        const raster::Target &t = papp_frame_begin(0xF9, 0xF9, 0xFB);
        header(t, "Scratch Everywhere!");
        if (n == 0) {
            centred(t, "No projects found.", 80, 8, 0x57, 0x5E, 0x75);
            centred(t, "Copy .sb3 files to " PAPP_SCRATCH_DIR " on the SD card,", 100, 8, 0x57, 0x5E, 0x75);
            centred(t, "then press A or tap to look again.", 116, 8, 0x57, 0x5E, 0x75);
        }
        for (int i = 0; i < visible && first + i < n; i++) {
            const int index = first + i;
            const int y = top + i * rowH;
            const bool sel = index == selected;
            raster::fillRoundRect(t, 8, y, PAPP_FRAME_W - 16, rowH - 4, 6, sel ? BG[0] : 0xE9,
                                  sel ? BG[1] : 0xEE, sel ? BG[2] : 0xF2, 255);
            std::string label = names[index].substr(0, names[index].size() - 4);
            if (label.size() > 52) {
                label = label.substr(0, 50) + "..";
            }
            raster::text(t, label.c_str(), 18, y + 6, 8, sel ? 255 : 0x57, sel ? 255 : 0x5E, sel ? 255 : 0x75, 255);
        }
        raster::text(t, "Tap or A: play      Menu+X: quit", 8, PAPP_FRAME_H - 16, 8, 0x85, 0x8A, 0x99, 255);
        papp_frame_end(30);
    }
}

// ── Running a project ───────────────────────────────────────────────────────

// Returns false when the player quit the app.
static bool run_project(const std::string &path)
{
    std::string name = path.substr(path.find_last_of('/') + 1);
    show_loading(name);
    papp_svc->log_printf("SE: loading %s\n", path.c_str());
    papp_input_clear_actions();
    Unzip::filePath = path;
    if (!Unzip::load()) {
        papp_svc->log_printf("SE: could not load %s (%d)\n", path.c_str(), (int)Unzip::projectOpened);
        Unzip::filePath = "";
        return wait_for_ok("Could not load the project", {name, "", "See the device log for details.",
                                                          "Press a button to go back."});
    }
    papp_svc->log_printf("SE: running %s (%d sprites, %dx%d, %d fps)\n", name.c_str(), (int)Scratch::sprites.size(),
                         Scratch::projectWidth, Scratch::projectHeight, Scratch::FPS);
    papp_input_clear_actions();
    Scratch::startScratchProject();  // cleans the project up when it ends
    Unzip::filePath = "";
    Scratch::nextProject = false;
    return !OS::toExit;
}

int papp_scratch_run(void)
{
    if (!OS::init() || !Render::Init()) {
        papp_svc->log_printf("SE: no display\n");
        return -1;
    }
    if (!SoundPlayer::init()) {
        papp_svc->log_printf("SE: no sound\n");
    }
    if (papp_svc->file_mkdir != nullptr) {
        papp_svc->file_mkdir(PAPP_SCRATCH_DIR);
    }

    // A launch argument naming an .sb3 file runs that project first.
    std::string start;
    if (papp_svc->app_get_arg != nullptr) {
        char arg[PAPP_APP_ARG_MAX];
        if (papp_svc->app_get_arg(arg, sizeof arg) > 0 && ends_with_sb3(arg)) {
            start = arg;
            if (start[0] != '/') {
                start = PAPP_SCRATCH_DIR + start;
            }
        }
    }

    bool first = true;
    while (!OS::toExit && !papp_input.quit) {
        std::vector<std::string> projects = list_projects();
        std::string path;
        if (first && !start.empty()) {
            path = start;
        } else if (first && projects.size() == 1) {
            path = PAPP_SCRATCH_DIR + projects[0];
        } else {
            const int pick = pick_project(projects);
            if (pick == -1) {
                break;
            }
            if (pick < 0) {
                continue;  // look again
            }
            path = PAPP_SCRATCH_DIR + projects[pick];
        }
        first = false;
        if (!run_project(path)) {
            break;
        }
    }
    papp_svc->log_printf("SE: leaving\n");
    // papp_main.cpp returns the heap and files; the runtime's own cleanup is
    // skipped (it frees block by block what goes back as a whole anyway).
    return 0;
}
