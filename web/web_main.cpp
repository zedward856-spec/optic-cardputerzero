// Web (WebAssembly) entry point for the Velvet UI: the same velvet_ui.cpp the device runs,
// drawn into a 320x170 SDL canvas. Keyboard comes from SDL; the page's on-screen keys call
// velvet_key() directly.
#include "asset_manager.h"
#include "velvet/keys.h"
#include "velvet/velvet_ui.h"

#include <SDL2/SDL.h>
#include <emscripten.h>

#include <cstring>
#include <vector>

namespace {
constexpr int W = 320, H = 170;
constexpr uint32_t kLongPressMs = 600;

SDL_Renderer* g_ren = nullptr;
SDL_Texture* g_tex = nullptr;
std::vector<uint32_t> g_fb(W * H);
std::vector<uint32_t> g_draw(W * H);
app::AssetManager* g_assets = nullptr;
velvet::VelvetUI* g_ui = nullptr;

uint32_t g_held_key = 0;
uint32_t g_held_since = 0;
bool g_long_sent = false;

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
    const int32_t w = lv_area_get_width(area);
    const auto* src = reinterpret_cast<const uint32_t*>(px);
    for (int32_t y = area->y1; y <= area->y2; ++y) {
        std::memcpy(&g_fb[y * W + area->x1], src + (y - area->y1) * w, w * 4);
    }
    if (lv_display_flush_is_last(disp)) {
        SDL_UpdateTexture(g_tex, nullptr, g_fb.data(), W * 4);
        SDL_RenderClear(g_ren);
        SDL_RenderCopy(g_ren, g_tex, nullptr, nullptr);
        SDL_RenderPresent(g_ren);
    }
    lv_display_flush_ready(disp);
}

uint32_t map_key(SDL_Keycode k, Uint16 mod) {
    using namespace velvet;
    // Ctrl / Alt + a printable key: the browser sends no text for these, so build them here
    if ((mod & (KMOD_CTRL | KMOD_ALT)) && k >= 32 && k < 127) {
        return ((mod & KMOD_CTRL) ? kKeyCtrl : kKeyAlt) | static_cast<uint32_t>(k);
    }
    if (k >= SDLK_F1 && k <= SDLK_F12) return kKeyF1 + static_cast<uint32_t>(k - SDLK_F1);
    switch (k) {
        case SDLK_PAGEUP: return kKeyPgUp;
        case SDLK_PAGEDOWN: return kKeyPgDn;
        case SDLK_INSERT: return kKeyInsert;
        case SDLK_HOME: return LV_KEY_HOME;
        case SDLK_END: return LV_KEY_END;
        case SDLK_UP: return LV_KEY_UP;
        case SDLK_DOWN: return LV_KEY_DOWN;
        case SDLK_LEFT: return LV_KEY_LEFT;
        case SDLK_RIGHT: return LV_KEY_RIGHT;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: return LV_KEY_ENTER;
        case SDLK_ESCAPE: return LV_KEY_ESC;
        case SDLK_BACKSPACE: return LV_KEY_BACKSPACE;
        case SDLK_TAB: return (mod & KMOD_SHIFT) ? kKeyBackTab : LV_KEY_NEXT;
        case SDLK_DELETE: return LV_KEY_DEL;
        default: return 0;  // printable keys arrive as SDL_TEXTINPUT

    }
}

void key_state(uint32_t key, bool down) {
    if (!key || !g_ui) return;
    if (down) {
        if (g_held_key == key) return;  // ignore auto-repeat
        g_held_key = key;
        g_held_since = SDL_GetTicks();
        g_long_sent = false;
        g_ui->handle_key(key, false);
    } else {
        if (g_held_key == key) g_held_key = 0;
        g_ui->handle_key_release(key);
    }
}

void loop() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            key_state(map_key(e.key.keysym.sym, e.key.keysym.mod), e.type == SDL_KEYDOWN);
        } else if (e.type == SDL_TEXTINPUT && g_ui) {
            // typed characters (with Shift etc. applied): one key press each
            for (const char* c = e.text.text; *c; ++c) {
                const auto ch = static_cast<unsigned char>(*c);
                if (ch >= 32 && ch < 127) g_ui->handle_key(ch, false);
            }
        }
    }
    if (g_held_key && !g_long_sent && SDL_GetTicks() - g_held_since >= kLongPressMs) {
        g_long_sent = true;
        g_ui->handle_key(g_held_key, true);
    }
    lv_timer_handler();
}
} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void velvet_key(int key, int down) {
    key_state(static_cast<uint32_t>(key), down != 0);
}

// The page's fn key changed layer (0 normal, 1 orange, 2 blue): show the same badge as the device.
extern "C" EMSCRIPTEN_KEEPALIVE void velvet_key_layer(int layer) {
    if (g_ui) g_ui->set_key_layer(layer);
}

// The page's modifier keys pressed (1 SYM, 2 Aa, 3 fn, 4 ctrl, 5 alt): for key sequences.
extern "C" EMSCRIPTEN_KEEPALIVE void velvet_mod(int which) {
    if (g_ui) g_ui->mod_tap(which);
}

int main() {
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
    SDL_Init(SDL_INIT_VIDEO);
    SDL_StartTextInput();
    SDL_Window* win = SDL_CreateWindow("Velvet", 0, 0, W, H, 0);
    // software renderer: the canvas gets a plain 2D context whose picture stays readable, so the
    // page can copy it (the HDMI monitor); WebGL would throw each frame away after showing it
    g_ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);

    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return SDL_GetTicks(); });
    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(disp, g_draw.data(), nullptr, g_draw.size() * 4,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    g_assets = new app::AssetManager();
    g_ui = new velvet::VelvetUI(
        *g_assets, [](void*) { emscripten_run_script("window.velvetQuit && window.velvetQuit()"); }, nullptr);
    g_ui->build();

    emscripten_set_main_loop(loop, 0, 1);
    return 0;
}
