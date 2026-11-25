// The framebuffer of the device's own screen.
//
// Not always /dev/fb0: a CardputerZero booted with an HDMI screen plugged in gets the HDMI output
// as fb0 and its own 320x170 panel as fb1 (found on the first real unit, 2026-11-25: the app drew
// on the HDMI framebuffer and the panel kept showing the launcher). M5's image has a stable name
// for the panel, /dev/fb_lcd; without it, the panel is the framebuffer whose driver is
// panel-mipi-dbi (or any 320x170 one). OPTIC_FB=/dev/fbN overrides all of it.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace velvet {

inline std::string lcd_fb_path(const char* fallback = "/dev/fb0") {
    if (const char* e = std::getenv("OPTIC_FB"); e && *e) return e;
    if (access("/dev/fb_lcd", R_OK | W_OK) == 0) return "/dev/fb_lcd";
    std::string by_size;
    for (int i = 0; i < 8; ++i) {
        char p[64], buf[64] = {};
        std::snprintf(p, sizeof p, "/sys/class/graphics/fb%d/name", i);
        if (FILE* f = std::fopen(p, "r")) {
            if (std::fgets(buf, sizeof buf, f) && std::strstr(buf, "mipi-dbi")) {
                std::fclose(f);
                return "/dev/fb" + std::to_string(i);
            }
            std::fclose(f);
        }
        std::snprintf(p, sizeof p, "/sys/class/graphics/fb%d/virtual_size", i);
        if (FILE* f = std::fopen(p, "r")) {
            int w = 0, h = 0;
            if (std::fscanf(f, "%d,%d", &w, &h) == 2 && w == 320 && h == 170 && by_size.empty())
                by_size = "/dev/fb" + std::to_string(i);
            std::fclose(f);
        }
    }
    return by_size.empty() ? fallback : by_size;
}

}  // namespace velvet
