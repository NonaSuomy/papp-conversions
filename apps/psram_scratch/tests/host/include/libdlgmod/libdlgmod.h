// Host test stand-in for libdlgmod (upstream log.cpp shows its error dialogs
// with it on desktop Linux). A fatal error ends the test run with exit code 3.
#pragma once
#include <cstdio>
#include <cstdlib>

inline const char *widget_get_caption() { return ""; }
inline void widget_set_caption(const char *) {}
inline const char *widget_get_button_name(int) { return ""; }
inline void widget_set_button_name(int, const char *) {}
inline void show_error(const char *message, bool fatal)
{
    std::fprintf(stderr, "host: %s error: %s\n", fatal ? "fatal" : "critical", message);
    if (fatal) {
        std::exit(3);
    }
}
