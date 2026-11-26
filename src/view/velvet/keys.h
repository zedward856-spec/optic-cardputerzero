// Key codes Velvet uses on top of LVGL's (LV_KEY_UP, LV_KEY_ENTER, ... and plain Unicode
// characters). All of these sit above the Unicode range, so they never collide with text.
// Shared by the device keyboard (platform/linux_input.cpp), the web build and the pages.
#pragma once

#include <cstdint>

namespace velvet {

constexpr uint32_t kKeyCtrl = 0x01000000;  // kKeyCtrl | 'c'  = Ctrl+C (lowercase letter or symbol)
constexpr uint32_t kKeyAlt = 0x02000000;   // kKeyAlt  | 'x'  = Alt+X
constexpr uint32_t kKeySpecial = 0x03000000;
constexpr uint32_t kKeyPgUp = kKeySpecial | 1;
constexpr uint32_t kKeyPgDn = kKeySpecial | 2;
constexpr uint32_t kKeyInsert = kKeySpecial | 3;
constexpr uint32_t kKeyBackTab = kKeySpecial | 4;   // Shift+Tab
constexpr uint32_t kKeyVolUp = kKeySpecial | 5;     // the keyboard's volume keys: fn+D, fn+S, fn+A
constexpr uint32_t kKeyVolDown = kKeySpecial | 6;
constexpr uint32_t kKeyMute = kKeySpecial | 7;
constexpr uint32_t kKeyMediaPlay = kKeySpecial | 8;   // fn+Q: play / pause
constexpr uint32_t kKeyMediaPrev = kKeySpecial | 9;   // fn+W: the song before
constexpr uint32_t kKeyMediaNext = kKeySpecial | 10;  // fn+E: the next song
constexpr uint32_t kKeyF1 = kKeySpecial | 0x10;     // F1..F12 = kKeyF1 + 0..11

inline bool is_ctrl_key(uint32_t k) { return (k & 0xFF000000) == kKeyCtrl; }
inline bool is_alt_key(uint32_t k) { return (k & 0xFF000000) == kKeyAlt; }

}  // namespace velvet
