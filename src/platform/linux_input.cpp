#include "linux_input.h"

#include "../view/velvet/keys.h"

#include <array>
#include <utility>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if !USE_DESKTOP
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#ifndef APP_KEY_INPUT_DEVICE
#define APP_KEY_INPUT_DEVICE ""
#endif

namespace platform {
namespace {

std::array<lv_obj_t*, kNavKeyCount> nav_buttons{};
uint32_t last_key = 0;
bool last_key_pressed = false;
bool nav_shortcut_mode = false;
uint32_t pressed_key = 0;
uint32_t press_started_at = 0;
bool long_press_sent = false;
bool pressed_key_consumed = false;
lv_timer_t* long_press_timer = nullptr;
KeyReleaseListener key_release_listener = nullptr;
void* key_release_listener_user_data = nullptr;
GlobalKeyListener global_key_listener = nullptr;
KeyLayerListener layer_listener = nullptr;
void* layer_listener_user_data = nullptr;
ModTapListener mod_listener = nullptr;   // a modifier pressed (1 SYM, 2 Aa, 3 fn, 4 ctrl, 5 alt)
void* mod_listener_user_data = nullptr;
void mod_tap(int which) { if (mod_listener) mod_listener(which, mod_listener_user_data); }
void* global_key_listener_user_data = nullptr;

constexpr uint32_t kLongPressMs = 700;

#if !USE_DESKTOP
struct EvdevKeypad {
    int fd{-1};
    lv_indev_state_t state{LV_INDEV_STATE_RELEASED};
    uint32_t key{0};
    bool router_event_pending{false};
    uint32_t router_key{0};
    bool router_pressed{false};
};
#endif

size_t nav_key_to_index(uint32_t key) {
    if (nav_shortcut_mode) {
        switch (key) {
            case LV_KEY_ESC:
                return 4;
            case 'z':
            case 'Z':
            case LV_KEY_LEFT:
                return 1;
            case 'c':
            case 'C':
            case LV_KEY_RIGHT:
                return 3;
            default:
                return kNavKeyCount;
        }
    }

    switch (key) {
        case '4':
        case LV_KEY_ESC:
            return 0;
        case '5':
            return 1;
        case '6':
            return 2;
        case '7':
            return 3;
        case '8':
            return 4;
        default:
            return kNavKeyCount;
    }
}

void dispatch_nav_key(uint32_t key) {
    const auto index = nav_key_to_index(key);
    if (index >= nav_buttons.size()) {
        return;
    }

    auto* button = nav_buttons[index];
    if (!button || !lv_obj_is_valid(button) || !lv_obj_has_flag(button, LV_OBJ_FLAG_CLICKABLE)) {
        return;
    }

    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
}

bool emit_global_key(uint32_t key, bool long_pressed) {
    return global_key_listener &&
           global_key_listener(key, describe_key(key), long_pressed, global_key_listener_user_data);
}

void long_press_timer_cb(lv_timer_t*) {
    if (!pressed_key || long_press_sent || lv_tick_elaps(press_started_at) < kLongPressMs) {
        return;
    }

    long_press_sent = true;
    if (emit_global_key(pressed_key, true)) {
        pressed_key_consumed = true;
    }
}

void ensure_long_press_timer() {
    if (!long_press_timer) {
        long_press_timer = lv_timer_create(long_press_timer_cb, 40, nullptr);
    }
    if (long_press_timer) {
        lv_timer_resume(long_press_timer);
        lv_timer_reset(long_press_timer);
    }
}

void route_key_state(uint32_t key, bool pressed) {
    const bool new_hold = pressed && (!last_key_pressed || pressed_key != key);
    if (new_hold) {
        pressed_key = key;
        press_started_at = lv_tick_get();
        long_press_sent = false;
        pressed_key_consumed = emit_global_key(key, false);
        ensure_long_press_timer();
        if (!pressed_key_consumed) {
            dispatch_nav_key(key);
        }
    }
    else if (!pressed && last_key_pressed && pressed_key == key) {
        if (key_release_listener) {
            key_release_listener(key, describe_key(key), key_release_listener_user_data);
        }
        pressed_key = 0;
        pressed_key_consumed = false;
        if (long_press_timer) {
            lv_timer_pause(long_press_timer);
        }
    }

    last_key = key;
    last_key_pressed = pressed;
}

void key_event_cb(lv_event_t* event) {
    LV_UNUSED(event);

    auto* indev = lv_indev_active();
    if (!indev) {
        return;
    }

#if !USE_DESKTOP
    auto* keypad = static_cast<EvdevKeypad*>(lv_indev_get_driver_data(indev));
    if (keypad) {
        // LVGL emits LV_EVENT_KEY on every poll, even when evdev had no new event.
        if (!keypad->router_event_pending) {
            return;
        }

        keypad->router_event_pending = false;
        const auto key = keypad->router_key;
        const bool pressed = keypad->router_pressed;

        route_key_state(key, pressed);
        return;
    }
#endif

    const auto key = lv_indev_get_key(indev);
    const bool pressed = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;

    route_key_state(key, pressed);
}

#if !USE_DESKTOP
// ---- device keyboard -> characters ----
// The CardputerZero keypad (TCA8418) sends most symbols as custom keycodes, listed in
// /usr/share/keymaps/tca8418_keypad_m5stack_keymap.map. Like the stock launcher, those
// win first; everything else uses a US layout with Shift / Caps Lock.
struct SymName {
    const char* name;
    char ch;
};
constexpr SymName kSymNames[] = {
    {"exclam", '!'}, {"at", '@'}, {"numbersign", '#'}, {"dollar", '$'}, {"percent", '%'},
    {"asciicircum", '^'}, {"ampersand", '&'}, {"asterisk", '*'}, {"parenleft", '('},
    {"parenright", ')'}, {"asciitilde", '~'}, {"grave", '`'}, {"underscore", '_'},
    {"minus", '-'}, {"plus", '+'}, {"equal", '='}, {"bracketleft", '['}, {"bracketright", ']'},
    {"braceleft", '{'}, {"braceright", '}'}, {"semicolon", ';'}, {"colon", ':'},
    {"apostrophe", '\''}, {"quotedbl", '"'}, {"less", '<'}, {"greater", '>'},
    {"backslash", '\\'}, {"bar", '|'}, {"comma", ','}, {"period", '.'}, {"slash", '/'},
    {"question", '?'}, {"space", ' '},
};

// Built-in copy of the stock keymap (used when the .map file is missing).
constexpr std::pair<uint16_t, char> kDefaultTcaMap[] = {
    {26, '!'}, {27, '@'}, {39, '#'}, {40, '$'}, {41, '%'}, {43, '^'}, {51, '&'}, {52, '*'},
    {53, '('}, {94, ')'}, {55, '~'}, {69, '`'}, {70, '_'}, {71, '-'}, {72, '+'}, {73, '='},
    {74, '['}, {75, ']'}, {76, '{'}, {77, '}'}, {79, ';'}, {80, ':'}, {81, '\''}, {82, '"'},
    {83, '<'}, {85, '>'}, {86, '\\'}, {89, '|'}, {90, ','}, {91, '.'}, {92, '/'}, {93, '?'},
};

std::array<char, KEY_MAX + 1> tca_map{};
bool tca_map_loaded = false;

void load_tca_map() {
    if (tca_map_loaded) return;
    tca_map_loaded = true;
    FILE* f = std::fopen("/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map", "r");
    if (!f) {
        for (const auto& [code, ch] : kDefaultTcaMap) tca_map[code] = ch;
        return;
    }
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        unsigned code = 0;
        char name[64] = {};
        if (std::sscanf(line, " keycode %u = %63s", &code, name) != 2 || code > KEY_MAX) continue;
        for (const auto& s : kSymNames) {
            if (std::strcmp(s.name, name) == 0) tca_map[code] = s.ch;
        }
    }
    std::fclose(f);
}

struct Shifted {
    char normal;
    char shifted;
};

Shifted us_layout(uint16_t code) {
    static const char* const kRow1 = "1234567890";  // KEY_1..KEY_0
    static const char* const kRow1s = "!@#$%^&*()";
    if (code >= KEY_1 && code <= KEY_0) return {kRow1[code - KEY_1], kRow1s[code - KEY_1]};
    switch (code) {
        case KEY_MINUS: return {'-', '_'};
        case KEY_EQUAL: return {'=', '+'};
        case KEY_LEFTBRACE: return {'[', '{'};
        case KEY_RIGHTBRACE: return {']', '}'};
        case KEY_SEMICOLON: return {';', ':'};
        case KEY_APOSTROPHE: return {'\'', '"'};
        case KEY_GRAVE: return {'`', '~'};
        case KEY_BACKSLASH: return {'\\', '|'};
        case KEY_COMMA: return {',', '<'};
        case KEY_DOT: return {'.', '>'};
        case KEY_SLASH: return {'/', '?'};
        case KEY_SPACE: return {' ', ' '};
        default: break;
    }
    static const struct { uint16_t code; char ch; } kLetters[] = {
        {KEY_Q, 'q'}, {KEY_W, 'w'}, {KEY_E, 'e'}, {KEY_R, 'r'}, {KEY_T, 't'}, {KEY_Y, 'y'},
        {KEY_U, 'u'}, {KEY_I, 'i'}, {KEY_O, 'o'}, {KEY_P, 'p'}, {KEY_A, 'a'}, {KEY_S, 's'},
        {KEY_D, 'd'}, {KEY_F, 'f'}, {KEY_G, 'g'}, {KEY_H, 'h'}, {KEY_J, 'j'}, {KEY_K, 'k'},
        {KEY_L, 'l'}, {KEY_Z, 'z'}, {KEY_X, 'x'}, {KEY_C, 'c'}, {KEY_V, 'v'}, {KEY_B, 'b'},
        {KEY_N, 'n'}, {KEY_M, 'm'},
    };
    for (const auto& l : kLetters) {
        if (l.code == code) return {l.ch, static_cast<char>(l.ch - 'a' + 'A')};
    }
    return {0, 0};
}

bool shift_l = false, shift_r = false, caps_lock = false;
bool ctrl_l = false, ctrl_r = false, alt_l = false, alt_r = false;

// ---- key layers ----
// fn  (orange legends): tap to turn the orange layer on, tap again for off. Hold fn + key
//                       for a single orange key.
// SYM (blue legends):   tap to turn the symbol layer on, tap again for off.
// A layer also turns off when you press a key that has nothing on that layer.
#ifndef KEY_FN
#define KEY_FN 0x1d0
#endif
bool fn_down = false, fn_used = false;
int layer = 0;            // 0 normal, 1 orange, 2 blue
bool layer_used = false;  // a key was pressed since the layer turned on

// The blue legends printed on the keys.
char sym_layer(uint16_t code) {
    static const char* const kDigits = "!@#$%^&*()";
    if (code >= KEY_1 && code <= KEY_0) return kDigits[code - KEY_1];
    switch (code) {
        case KEY_Q: return '~';
        case KEY_W: return '`';
        case KEY_E: return '+';
        case KEY_R: return '-';
        case KEY_T: return '/';
        case KEY_Y: return '\\';
        case KEY_U: return '{';
        case KEY_I: return '}';
        case KEY_O: return '[';
        case KEY_P: return ']';
        case KEY_A: return '.';
        case KEY_D: return '|';
        case KEY_G: return '=';
        case KEY_H: return ':';
        case KEY_J: return ';';
        case KEY_K: return '_';
        case KEY_L: return '?';
        case KEY_V: return '<';
        case KEY_B: return '>';
        case KEY_N: return '\'';
        case KEY_M: return '"';
        default: return 0;
    }
}

uint32_t fn_layer(uint16_t code) {
    switch (code) {
        case KEY_F: return LV_KEY_UP;
        case KEY_X: return LV_KEY_DOWN;
        case KEY_Z: return LV_KEY_LEFT;
        case KEY_C: return LV_KEY_RIGHT;
        case KEY_K: return velvet::kKeyPgUp;
        case KEY_L: return velvet::kKeyPgDn;
        case KEY_N: return LV_KEY_HOME;
        case KEY_M: return velvet::kKeyInsert;
        case KEY_BACKSPACE: return LV_KEY_DEL;
        case KEY_O: return velvet::kKeyF1 + 10;
        case KEY_P: return velvet::kKeyF1 + 11;
        default: break;
    }
    if (code >= KEY_1 && code <= KEY_0) return velvet::kKeyF1 + (code - KEY_1);  // F1..F10
    return 0;
}
std::array<uint32_t, KEY_MAX + 1> pressed_as{};  // what each held key was reported as

// Returns the key to report, or 0 to ignore. `value`: 1 press, 0 release.
uint32_t map_evdev_key(uint16_t code, int value) {
    if (code > KEY_MAX) return 0;
    if (value == 1) {
        switch (code) {
            case KEY_COMPOSE: case 0x17f: mod_tap(1); break;   // 0x17f: the CardputerZero's SYM key (its driver sends KEY_CD)
            case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: case KEY_CAPSLOCK: mod_tap(2); break;
            case KEY_FN: case 0x185: mod_tap(3); break;        // 0x185: its fn key (KEY_DVD)
            case KEY_LEFTCTRL: case KEY_RIGHTCTRL: mod_tap(4); break;
            case KEY_LEFTALT: case KEY_RIGHTALT: mod_tap(5); break;
            default: break;
        }
    }
    switch (code) {
        case KEY_LEFTSHIFT: shift_l = value != 0; return 0;
        case KEY_RIGHTSHIFT: shift_r = value != 0; return 0;
        case KEY_CAPSLOCK: if (value == 1) caps_lock = !caps_lock; return 0;
        case KEY_LEFTCTRL: ctrl_l = value != 0; return 0;
        case KEY_RIGHTCTRL: ctrl_r = value != 0; return 0;
        case KEY_LEFTALT: alt_l = value != 0; return 0;
        case KEY_RIGHTALT: alt_r = value != 0; return 0;
        case KEY_COMPOSE:  // the SYM key: symbol layer on / off
            if (value == 1) {
                layer = layer == 2 ? 0 : 2;
                layer_used = false;
                if (layer_listener) layer_listener(layer, layer_listener_user_data);
            }
            return 0;
        case KEY_FN:
            if (value == 1) {
                fn_down = true;
                fn_used = false;
            } else if (value == 0) {
                fn_down = false;
                if (!fn_used) {  // tapped alone: orange layer on / off
                    layer = layer == 1 ? 0 : 1;
                    layer_used = false;
                    if (layer_listener) layer_listener(layer, layer_listener_user_data);
                }
            }
            return 0;
        default: break;
    }
    if (value == 0) {  // release: report exactly what the press reported
        const uint32_t k = pressed_as[code];
        pressed_as[code] = 0;
        return k;
    }

    uint32_t key = 0;
    if (fn_down) fn_used = true;
    else if (layer) layer_used = true;
    if (fn_down || layer == 1) key = fn_layer(code);
    else if (layer == 2) {
        key = static_cast<unsigned char>(sym_layer(code));
        load_tca_map();  // the keyboard driver may already send the symbol itself
        if (!key && tca_map[code]) key = static_cast<unsigned char>(tca_map[code]);
    }
    if (!key && layer && !fn_down) {
        // a key with no legend on this layer: drop back to normal and type it as usual
        layer = 0;
        layer_used = false;
        if (layer_listener) layer_listener(0, layer_listener_user_data);
    }
    if (!key) switch (code) {
        case KEY_ESC: key = LV_KEY_ESC; break;
        case KEY_UP: key = LV_KEY_UP; break;
        case KEY_DOWN: key = LV_KEY_DOWN; break;
        case KEY_LEFT: key = LV_KEY_LEFT; break;
        case KEY_RIGHT: key = LV_KEY_RIGHT; break;
        case KEY_ENTER:
        case KEY_KPENTER: key = LV_KEY_ENTER; break;
        case KEY_BACKSPACE: key = LV_KEY_BACKSPACE; break;
        case KEY_DELETE: key = LV_KEY_DEL; break;
        case KEY_TAB: key = (shift_l || shift_r) ? velvet::kKeyBackTab : static_cast<uint32_t>(LV_KEY_NEXT); break;
        case KEY_PAGEUP: key = velvet::kKeyPgUp; break;
        case KEY_PAGEDOWN: key = velvet::kKeyPgDn; break;
        case KEY_INSERT: key = velvet::kKeyInsert; break;
        case KEY_F1: case KEY_F2: case KEY_F3: case KEY_F4: case KEY_F5: case KEY_F6:
        case KEY_F7: case KEY_F8: case KEY_F9: case KEY_F10:
            key = velvet::kKeyF1 + (code - KEY_F1);
            break;
        case KEY_F11: key = velvet::kKeyF1 + 10; break;
        case KEY_F12: key = velvet::kKeyF1 + 11; break;
        case KEY_HOME: key = LV_KEY_HOME; break;
        case KEY_END: key = LV_KEY_END; break;
        case KEY_VOLUMEUP: key = velvet::kKeyVolUp; break;       // fn+D on the CardputerZero
        case KEY_VOLUMEDOWN: key = velvet::kKeyVolDown; break;   // fn+S
        case KEY_MUTE: key = velvet::kKeyMute; break;            // fn+A
        case KEY_SYSRQ: key = kKeyPrintScreen; break;
        case KEY_HELP: key = kKeyHelp; break;
        default: break;
    }
    if (!key) {
        load_tca_map();
        if (tca_map[code]) {
            key = static_cast<unsigned char>(tca_map[code]);
        } else {
            const Shifted s = us_layout(code);
            const bool shift = shift_l || shift_r;
            const bool letter = s.normal >= 'a' && s.normal <= 'z';
            key = static_cast<unsigned char>((letter ? shift != caps_lock : shift) ? s.shifted : s.normal);
        }
        // Ctrl / Alt + a character: Ctrl+C, Alt+B ... (the terminal page turns these into bytes)
        if (key >= 0x20 && key < 0x7F) {
            if (ctrl_l || ctrl_r) key = velvet::kKeyCtrl | (key >= 'A' && key <= 'Z' ? key + 32 : key);
            else if (alt_l || alt_r) key = velvet::kKeyAlt | key;
        }
    }
    pressed_as[code] = key;
    return key;
}

bool has_nav_keys(int fd) {
    unsigned long key_bits[(KEY_MAX / (sizeof(unsigned long) * 8)) + 1] = {};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
        return false;
    }

    auto has_key = [&](int code) {
        const auto bits_per_word = static_cast<int>(sizeof(unsigned long) * 8);
        return (key_bits[code / bits_per_word] & (1UL << (code % bits_per_word))) != 0;
    };

    return has_key(KEY_ESC) || has_key(KEY_LEFT) || has_key(KEY_RIGHT) || has_key(KEY_Z) ||
           has_key(KEY_C) || has_key(KEY_ENTER) || has_key(KEY_UP) || has_key(KEY_4) || has_key(KEY_5) || has_key(KEY_6) ||
           has_key(KEY_7) || has_key(KEY_8) || has_key(KEY_SYSRQ) || has_key(KEY_HELP);
}

void evdev_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* keypad = static_cast<EvdevKeypad*>(lv_indev_get_driver_data(indev));
    data->continue_reading = false;
    if (!keypad) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    input_event input{};
    while (read(keypad->fd, &input, sizeof(input)) == sizeof(input)) {
        if (input.type != EV_KEY) {
            continue;
        }

        if (input.value == 2) {  // auto-repeat
            continue;
        }
        const auto key = map_evdev_key(input.code, input.value);
        if (!key) {
            continue;
        }

        keypad->key = key;
        keypad->state = input.value ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        keypad->router_event_pending = true;
        keypad->router_key = key;
        keypad->router_pressed = keypad->state == LV_INDEV_STATE_PRESSED;
        data->continue_reading = true;
        break;
    }

    data->key = keypad->key;
    data->state = keypad->state;
}

void evdev_delete_cb(lv_event_t* event) {
    auto* indev = static_cast<lv_indev_t*>(lv_event_get_target(event));
    auto* keypad = static_cast<EvdevKeypad*>(lv_indev_get_driver_data(indev));
    if (!keypad) {
        return;
    }

    if (keypad->fd >= 0) {
        close(keypad->fd);
    }
    delete keypad;
}

lv_indev_t* create_keypad_from_fd(int fd) {
    auto* keypad = new EvdevKeypad;
    keypad->fd = fd;

    auto* indev = lv_indev_create();
    if (!indev) {
        delete keypad;
        close(fd);
        return nullptr;
    }

    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, evdev_read_cb);
    lv_indev_set_driver_data(indev, keypad);
    lv_indev_add_event_cb(indev, evdev_delete_cb, LV_EVENT_DELETE, nullptr);
    attach_key_router(indev);
    return indev;
}

lv_indev_t* try_create_keypad(const char* path) {
    if (!path || path[0] == '\0') {
        return nullptr;
    }

    const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        LV_LOG_WARN("failed to open input device %s: %s", path, strerror(errno));
        return nullptr;
    }

    if (!has_nav_keys(fd)) {
        close(fd);
        return nullptr;
    }

    LV_LOG_INFO("using evdev key input %s", path);
    return create_keypad_from_fd(fd);
}

void discover_keypads(lv_display_t* display) {
    const char* configured_device = APP_KEY_INPUT_DEVICE;
    if (configured_device[0] != '\0') {
        auto* indev = try_create_keypad(configured_device);
        if (indev) {
            lv_indev_set_display(indev, display);
        }
        return;
    }

    auto* dir = opendir("/dev/input");
    if (!dir) {
        LV_LOG_WARN("failed to open /dev/input: %s", strerror(errno));
        return;
    }

    while (auto* entry = readdir(dir)) {
        if (std::strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        std::string path = "/dev/input/";
        path += entry->d_name;
        auto* indev = try_create_keypad(path.c_str());
        if (indev) {
            lv_indev_set_display(indev, display);
        }
    }

    closedir(dir);
}
#endif

} // namespace

void init_key_input(lv_display_t* display) {
#if !USE_DESKTOP
    discover_keypads(display);
#else
    LV_UNUSED(display);
#endif
}

void attach_key_router(lv_indev_t* indev) {
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_KEYPAD) {
        return;
    }

    lv_indev_add_event_cb(indev, key_event_cb, LV_EVENT_KEY, nullptr);
}

void set_nav_shortcut_mode(bool enabled) {
    nav_shortcut_mode = enabled;
}

void register_nav_button(size_t index, lv_obj_t* button) {
    if (index >= nav_buttons.size()) {
        return;
    }

    nav_buttons[index] = button;
}

void unregister_nav_button(size_t index, lv_obj_t* button) {
    if (index >= nav_buttons.size()) {
        return;
    }

    if (!button || nav_buttons[index] == button) {
        nav_buttons[index] = nullptr;
    }
}

void set_mod_tap_listener(ModTapListener listener, void* user_data) {
    mod_listener = listener;
    mod_listener_user_data = user_data;
}

void set_key_layer_listener(KeyLayerListener listener, void* user_data) {
    layer_listener = listener;
    layer_listener_user_data = user_data;
}

void set_key_release_listener(KeyReleaseListener listener, void* user_data) {
    key_release_listener = listener;
    key_release_listener_user_data = user_data;
}

void clear_key_release_listener(KeyReleaseListener listener, void* user_data) {
    if (key_release_listener == listener && key_release_listener_user_data == user_data) {
        key_release_listener = nullptr;
        key_release_listener_user_data = nullptr;
    }
}

void set_global_key_listener(GlobalKeyListener listener, void* user_data) {
    global_key_listener = listener;
    global_key_listener_user_data = user_data;
}

void clear_global_key_listener(GlobalKeyListener listener, void* user_data) {
    if (global_key_listener == listener && global_key_listener_user_data == user_data) {
        global_key_listener = nullptr;
        global_key_listener_user_data = nullptr;
    }
}

const char* describe_key(uint32_t key) {
    switch (key) {
        case LV_KEY_ESC:
            return "Esc";
        case LV_KEY_LEFT:
            return "Left";
        case LV_KEY_RIGHT:
            return "Right";
        case kKeyPrintScreen:
            return "PrintScreen";
        case kKeyHelp:
            return "Help";
        default:
            break;
    }

    static char buffer[24];
    if (key >= 32 && key <= 126) {
        std::snprintf(buffer, sizeof(buffer), "%c", static_cast<char>(key));
    }
    else {
        std::snprintf(buffer, sizeof(buffer), "Key_%u", key);
    }
    return buffer;
}

} // namespace platform
