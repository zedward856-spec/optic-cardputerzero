// CardputerZero UI ("Velvet" in the code), in MinUI's look: black, white pills, one bitmap
// font. Software renderer friendly: no shadows or big blends. Text is never rotated or scaled,
// so every glyph stays on the pixel grid.
#include "lcdfb.h"
#include "velvet_ui.h"
#include "legacy.h"
#include "wordmark.h"
#include "egg_art.h"
#include "keys.h"
#if !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include "asset_manager.h"
#include "keys.h"


#include <png.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

extern "C" {
extern const lv_font_t velvet_ter_b14;
extern const lv_font_t velvet_ter_b16;
extern const lv_font_t velvet_ter_b18;
extern const lv_font_t velvet_ter_t14;  // ter_b14 + box drawing, for the terminal
extern const lv_font_t velvet_ter_t12;  // Terminus 6x12 + box drawing: compact terminal (F1)
void lv_image_cache_drop(const void* src);  // lv_image_cache.h (not in lvgl.h)
}

namespace velvet {

namespace {

// ---- palette: MinUI's (the same as Optic DE / Optic Suite). Two themes: Dark (MinUI's own) and
// White (the same UI turned over: white screen, black text and selected pill). Set by
// apply_theme() before the screen is built; the screen is rebuilt when it changes.
uint32_t kBg = 0x000000;     // screen
uint32_t kDark = 0x262626;   // pills, cards, tracks
uint32_t kWhite = 0xffffff;  // text, the selected pill, fills
uint32_t kGrey = 0x999999;   // secondary text
uint32_t kFaint = 0x7c7c7c;  // dim text, idle marks
uint32_t kLight = 0xcccccc;  // tracks inside a white pill
uint32_t kInk = 0x000000;    // text on a white pill
uint32_t kTermBg = 0x000000, kTermFg = 0xffffff;
// terminal colours 0-15 (programs' own colours), tuned to read on the theme's background
uint32_t kAnsi[16] = {0x000000, 0xff5f5f, 0x5fd75f, 0xffd75f, 0x5f87ff, 0xd75fd7, 0x5fd7d7, 0xcccccc,
                      0x7c7c7c, 0xff8787, 0x87ff87, 0xffff87, 0x87afff, 0xff87ff, 0x87ffff, 0xffffff};

void apply_theme(int theme) {
    static const uint32_t dark[16] = {0x000000, 0xff5f5f, 0x5fd75f, 0xffd75f, 0x5f87ff, 0xd75fd7, 0x5fd7d7, 0xcccccc,
                                      0x7c7c7c, 0xff8787, 0x87ff87, 0xffff87, 0x87afff, 0xff87ff, 0x87ffff, 0xffffff};
    static const uint32_t light[16] = {0x000000, 0xc62828, 0x2e7d32, 0x8d6e00, 0x1f4fd0, 0x9c27b0, 0x00838f, 0x555555,
                                       0x8a8a8a, 0xe53935, 0x43a047, 0xa07d00, 0x3a6ee8, 0xb040c8, 0x00a0ad, 0x000000};
    const bool w = theme == 1;
    kBg = w ? 0xffffff : 0x000000;
    kDark = w ? 0xe2e2e2 : 0x262626;
    kWhite = w ? 0x000000 : 0xffffff;
    kGrey = w ? 0x5c5c5c : 0x999999;
    kFaint = w ? 0x8c8c8c : 0x7c7c7c;
    kLight = w ? 0x4a4a4a : 0xcccccc;
    kInk = w ? 0xffffff : 0x000000;
    kTermBg = kBg;
    kTermFg = kWhite;
    std::memcpy(kAnsi, w ? light : dark, sizeof kAnsi);
}

lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

// Terminus bitmap fonts converted pixel-for-pixel by tools/psf2lvgl.py (1 bpp, no anti-alias).
lv_font_t* ter(const lv_font_t& f) { return const_cast<lv_font_t*>(&f); }

// ---- crisp polygons: filled row by row with 1 px rects, so edges are hard pixels and
// there are no anti-aliased seams. Even-odd rule.
void fill_poly(lv_layer_t* layer, const std::vector<lv_point_precise_t>& pts, lv_color_t color,
               lv_opa_t opa, int32_t ox, int32_t oy) {
    if (pts.size() < 3) return;
    float ymin = pts[0].y, ymax = pts[0].y;
    for (const auto& p : pts) {
        ymin = std::min(ymin, static_cast<float>(p.y));
        ymax = std::max(ymax, static_cast<float>(p.y));
    }
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = color;
    r.bg_opa = opa;
    std::vector<float> xs;
    for (int32_t y = static_cast<int32_t>(std::floor(ymin)); y < static_cast<int32_t>(std::ceil(ymax)); ++y) {
        const float sy = y + 0.5f;
        xs.clear();
        for (size_t i = 0; i < pts.size(); ++i) {
            const auto& a = pts[i];
            const auto& b = pts[(i + 1) % pts.size()];
            if ((a.y <= sy) != (b.y <= sy)) xs.push_back(a.x + (sy - a.y) * (b.x - a.x) / (b.y - a.y));
        }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2) {
            const int32_t x0 = static_cast<int32_t>(std::ceil(xs[i] - 0.5f));
            const int32_t x1 = static_cast<int32_t>(std::ceil(xs[i + 1] - 0.5f)) - 1;
            if (x1 < x0) continue;
            const lv_area_t span{ox + x0, oy + y, ox + x1, oy + y};
            lv_draw_rect(layer, &r, &span);
        }
    }
}

lv_obj_t* plain(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// MinUI's one shape: a filled rounded box. radius < 0 = a pill (half the height).
lv_obj_t* pill(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color, int32_t radius = -1) {
    lv_obj_t* o = plain(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, C(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius < 0 ? LV_RADIUS_CIRCLE : radius, 0);
    return o;
}

void pill_set_color(lv_obj_t* o, uint32_t color) { lv_obj_set_style_bg_color(o, C(color), 0); }

// Signal strength bars (4), colour = bg colour.
void bars_draw_cb(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const auto level = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(obj)));
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
    for (int i = 0; i < 4; ++i) {
        const int32_t h = 4 + i * 3;
        lv_area_t b{a.x1 + i * 4, a.y2 + 1 - h, a.x1 + i * 4 + 1, a.y2};
        r.bg_opa = i < level ? LV_OPA_COVER : LV_OPA_30;
        lv_draw_rect(lv_event_get_layer(e), &r, &b);
    }
}

// Padlock icon (7x9), color = bg color.
void lock_draw_cb(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
    r.bg_opa = LV_OPA_COVER;
    lv_layer_t* layer = lv_event_get_layer(e);
    const lv_area_t parts[] = {
        {a.x1 + 2, a.y1, a.x1 + 4, a.y1},          // shackle top
        {a.x1 + 1, a.y1 + 1, a.x1 + 1, a.y1 + 3},  // shackle left
        {a.x1 + 5, a.y1 + 1, a.x1 + 5, a.y1 + 3},  // shackle right
        {a.x1, a.y1 + 4, a.x1 + 6, a.y1 + 8},      // body
    };
    for (const auto& p : parts) lv_draw_rect(layer, &r, &p);
}

int signal_level(int signal) { return signal >= 75 ? 4 : signal >= 50 ? 3 : signal >= 25 ? 2 : 1; }

lv_obj_t* label(lv_obj_t* parent, const char* text, lv_font_t* font, uint32_t color,
                int32_t x, int32_t y) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, C(color), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

int32_t text_width(lv_obj_t* l) {
    lv_obj_update_layout(l);
    return lv_obj_get_width(l);
}

int32_t utf8_cols(const char* s) {
    int32_t n = 0;
    for (; *s; ++s) n += (static_cast<unsigned char>(*s) & 0xC0) != 0x80;
    return n;
}

// A pill with a label in it: `pad` px either side of the text, text centred vertically.
lv_obj_t* text_pill(lv_obj_t* parent, const char* text, lv_font_t* font, uint32_t bg, uint32_t fg, int32_t x,
                    int32_t y, int32_t h, int32_t pad = 8) {
    const int32_t w = utf8_cols(text) * (font->line_height >= 18 ? 10 : 8) + pad * 2;
    lv_obj_t* o = pill(parent, x, y, w, h, bg);
    label(o, text, font, fg, pad, (h - font->line_height) / 2);
    return o;
}

// Button hints, MinUI style: the key in a small white pill (black text), then what it does.
// Returns a container as wide as the hints.
lv_obj_t* hints(lv_obj_t* parent, int32_t x, int32_t y, lv_font_t* font,
                std::initializer_list<std::pair<const char*, const char*>> items) {
    lv_obj_t* box = plain(parent);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, 320, 16);
    int32_t cx = 0, end = 0;
    for (const auto& [key, what] : items) {
        if (key && key[0]) {
            lv_obj_t* k = text_pill(box, key, font, kWhite, kInk, cx, 1, 14, 4);
            end = cx + lv_obj_get_style_width(k, LV_PART_MAIN);
            cx = end + 4;
        }
        if (what && what[0]) {
            label(box, what, font, kGrey, cx, 1);
            end = cx + utf8_cols(what) * 8;
            cx = end + 10;
        }
    }
    lv_obj_set_width(box, std::max<int32_t>(1, end));
    return box;
}

// The same, right-aligned so it ends at `right`.
lv_obj_t* hints_r(lv_obj_t* parent, int32_t right, int32_t y, lv_font_t* font,
                  std::initializer_list<std::pair<const char*, const char*>> items) {
    lv_obj_t* box = hints(parent, 0, y, font, items);
    lv_obj_set_x(box, right - lv_obj_get_style_width(box, LV_PART_MAIN));
    return box;
}

// ---- line icons (14x14), drawn in the object's text colour ----
enum Icon { IcTerminal, IcBrowser, IcMusic, IcCamera, IcGallery, IcSensors, IcPwn, IcStats, IcWifi, IcSystem,
            IcNet, IcEth, IcTor, IcTailscale, IcSpeed, IcTools, IcOptic, IcHdmi, IcBt };

// The Optic eye (the logo's two lids and iris halves, logo units 7.5..112.5 x 30..90), [w] px
// wide with its top-left at (x, y) from (ox, oy). Its height is w * 4 / 7.
void draw_optic_eye(lv_layer_t* layer, float x, float y, float w, int32_t ox, int32_t oy, lv_color_t col) {
    static const float kParts[4][16] = {
        {7.5f, 57.5f, 35, 30, 85, 30, 112.5f, 57.5f, 102.5f, 57.5f, 82.5f, 37.5f, 37.5f, 37.5f, 17.5f, 57.5f},
        {17.5f, 62.5f, 37.5f, 82.5f, 82.5f, 82.5f, 102.5f, 62.5f, 112.5f, 62.5f, 85, 90, 35, 90, 7.5f, 62.5f},
        {40, 57.5f, 60, 37.5f, 80, 57.5f, 67.5f, 57.5f, 60, 50, 52.5f, 57.5f, 0, 0, 0, 0},
        {52.5f, 62.5f, 60, 70, 67.5f, 62.5f, 80, 62.5f, 60, 82.5f, 40, 62.5f, 0, 0, 0, 0}};
    const float k = w / 105.0f;
    for (int q = 0; q < 4; ++q) {
        std::vector<lv_point_precise_t> pts;
        for (int i = 0; i < (q < 2 ? 8 : 6); ++i)
            pts.push_back({x + (kParts[q][i * 2] - 7.5f) * k, y + (kParts[q][i * 2 + 1] - 30) * k});
        fill_poly(layer, pts, col, LV_OPA_COVER, ox, oy);
    }
}

// draws icon [ic] with its top-left at a.x1, a.y1
void draw_icon(lv_layer_t* layer, int ic, lv_area_t a, lv_color_t col) {
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = col;
    r.bg_opa = LV_OPA_COVER;
    auto rect = [&](int32_t x, int32_t y, int32_t w, int32_t h) {
        const lv_area_t b{a.x1 + x, a.y1 + y, a.x1 + x + w - 1, a.y1 + y + h - 1};
        lv_draw_rect(layer, &r, &b);
    };
    auto frame = [&](int32_t x, int32_t y, int32_t w, int32_t h) {  // 1 px outline, corners cut
        rect(x + 1, y, w - 2, 1);
        rect(x + 1, y + h - 1, w - 2, 1);
        rect(x, y + 1, 1, h - 2);
        rect(x + w - 1, y + 1, 1, h - 2);
    };
    auto seg = [&](float x0, float y0, float x1, float y1, float t) {
        const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy) + 0.001f;
        const float nx = -dy / len * t, ny = dx / len * t;
        fill_poly(layer, {{x0 - nx, y0 - ny}, {x1 - nx, y1 - ny}, {x1 + nx, y1 + ny}, {x0 + nx, y0 + ny}}, col,
                  LV_OPA_COVER, a.x1, a.y1);
    };
    auto ring = [&](float cx, float cy, float rad, float a0, float a1) {  // degrees, 0 = right, clockwise
        const int n = 24;
        float px = cx + rad * std::cos(a0 * 0.0174533f), py = cy + rad * std::sin(a0 * 0.0174533f);
        for (int i = 1; i <= n; ++i) {
            const float t = (a0 + (a1 - a0) * i / n) * 0.0174533f;
            const float x = cx + rad * std::cos(t), y = cy + rad * std::sin(t);
            seg(px, py, x, y, 0.55f);
            px = x;
            py = y;
        }
    };
    auto disc = [&](float cx, float cy, float rad) {
        std::vector<lv_point_precise_t> p;
        for (int i = 0; i < 16; ++i) p.push_back({cx + rad * std::cos(i * 0.3927f), cy + rad * std::sin(i * 0.3927f)});
        fill_poly(layer, p, col, LV_OPA_COVER, a.x1, a.y1);
    };
    switch (ic) {
        case IcTerminal:
            frame(0, 1, 14, 12);
            seg(3.5f, 4.5f, 6.0f, 7.0f, 0.6f);
            seg(6.0f, 7.0f, 3.5f, 9.5f, 0.6f);
            rect(7, 9, 4, 1);
            break;
        case IcBrowser:  // a browser window: solid tab bar on top, the page pointer inside
            frame(0, 1, 14, 12);
            rect(1, 2, 12, 2);
            fill_poly(layer, {{4.5f, 5.5f}, {4.5f, 12.0f}, {6.3f, 10.4f}, {7.6f, 12.6f}, {8.9f, 11.9f}, {7.7f, 9.8f}, {10.0f, 9.6f}},
                      col, LV_OPA_COVER, a.x1, a.y1);
            break;
        case IcMusic:
            rect(9, 1, 1, 9);
            seg(9.5f, 1.5f, 13.0f, 3.5f, 0.6f);
            rect(12, 3, 1, 2);
            disc(7, 10.5f, 2.6f);
            break;
        case IcCamera:
            frame(0, 3, 14, 10);
            rect(4, 1, 6, 1);
            rect(3, 2, 1, 1);
            rect(10, 2, 1, 1);
            ring(7, 8, 2.6f, 0, 360);
            break;
        case IcGallery:
            frame(0, 1, 14, 12);
            seg(1.5f, 11.0f, 5.0f, 6.5f, 0.6f);
            seg(5.0f, 6.5f, 8.0f, 10.0f, 0.6f);
            seg(7.5f, 9.5f, 9.5f, 7.5f, 0.6f);
            seg(9.5f, 7.5f, 12.5f, 11.0f, 0.6f);
            disc(10, 4.5f, 1.3f);
            break;
        case IcSensors:
            ring(7, 7, 6, 0, 360);
            fill_poly(layer, {{7, 2.5f}, {9, 7}, {7, 11.5f}, {5, 7}}, col, LV_OPA_COVER, a.x1, a.y1);
            break;
        case IcPwn:
            frame(1, 3, 12, 10);
            rect(7, 0, 1, 3);
            rect(4, 6, 2, 3);
            rect(8, 6, 2, 3);
            rect(0, 7, 1, 2);
            rect(13, 7, 1, 2);
            break;
        case IcStats:
            rect(1, 8, 3, 5);
            rect(5, 4, 3, 9);
            rect(9, 1, 3, 12);
            rect(0, 13, 14, 1);
            break;
        case IcWifi:
            ring(7, 12, 9.5f, 225, 315);
            ring(7, 12, 6.0f, 225, 315);
            disc(7, 11.5f, 1.6f);
            break;
        case IcSystem:  // three sliders
            for (int i = 0; i < 3; ++i) {
                const int32_t y = 2 + i * 5, k = i == 1 ? 9 : i == 0 ? 3 : 6;
                rect(0, y, 14, 1);
                rect(k - 1, y - 1, 3, 3);
            }
            break;
        case IcBt:  // the Bluetooth rune
            seg(7, 0.5f, 7, 13.5f, 0.6f);
            seg(7, 0.5f, 11, 4, 0.55f);
            seg(11, 4, 3, 10, 0.55f);
            seg(7, 13.5f, 11, 10, 0.55f);
            seg(11, 10, 3, 4, 0.55f);
            break;
        case IcHdmi:  // a screen on its stand
            frame(0, 1, 14, 9);
            rect(6, 10, 2, 2);
            rect(3, 12, 8, 1);
            break;
        case IcNet:  // three linked nodes
            seg(7, 3, 2.5f, 11, 0.55f);
            seg(7, 3, 11.5f, 11, 0.55f);
            seg(2.5f, 11, 11.5f, 11, 0.55f);
            disc(7, 3, 2.2f);
            disc(2.5f, 11, 2.2f);
            disc(11.5f, 11, 2.2f);
            break;
        case IcEth:  // the RJ45 socket: a box with the latch notch and the pins
            rect(0, 2, 14, 1);
            rect(0, 2, 1, 9);
            rect(13, 2, 1, 9);
            rect(0, 10, 4, 1);
            rect(10, 10, 4, 1);
            rect(3, 10, 1, 3);
            rect(10, 10, 1, 3);
            rect(3, 12, 8, 1);
            for (int i = 0; i < 4; ++i) rect(3 + i * 2, 4, 1, 3);
            break;
        case IcTor:  // an onion: the bulb, a layer inside, the sprout
            ring(7, 8.5f, 5.0f, 0, 360);
            ring(7, 8.5f, 2.4f, 0, 360);
            seg(7, 3.6f, 7, 1.0f, 0.55f);
            seg(7, 2.0f, 9.5f, 0.6f, 0.55f);
            break;
        case IcTailscale:  // four dots: three across the top, one under the middle
            for (int x = 0; x < 3; ++x) disc(2.5f + x * 4.5f, 4.5f, 1.9f);
            disc(7.0f, 9.5f, 1.9f);
            break;
        case IcSpeed:  // a gauge with its needle (pin)
            ring(7, 9.5f, 6.3f, 180, 360);
            rect(0, 9, 2, 1);
            rect(12, 9, 2, 1);
            seg(7, 9.5f, 11, 4.5f, 0.6f);
            disc(7, 9.5f, 1.9f);
            break;
        case IcOptic: {  // the Optic eye: two lids and the iris halves, from the logo (units 7.5..112.5)
            draw_optic_eye(layer, 0, 3, 14, a.x1, a.y1, col);
            break;
        }
        case IcTools:  // a wrench
            seg(2, 12, 8.2f, 5.8f, 1.0f);
            ring(10.2f, 3.8f, 2.9f, 105, 345);
            break;
        default: break;
    }
}

void icon_draw_cb(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const int ic = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(obj)));
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    draw_icon(lv_event_get_layer(e), ic, a, lv_obj_get_style_text_color(obj, LV_PART_MAIN));
}

lv_obj_t* icon(lv_obj_t* parent, Icon ic, int32_t x, int32_t y, uint32_t color) {
    lv_obj_t* o = plain(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, 14, 14);
    lv_obj_set_style_text_color(o, C(color), 0);
    lv_obj_set_user_data(o, reinterpret_cast<void*>(static_cast<intptr_t>(ic)));
    lv_obj_add_event_cb(o, icon_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    return o;
}

// A busy bar: a white pill sliding along a dark track, squeezed (still round) at the ends.
struct Busy {
    uint32_t track, fill;
};
void busy_draw_cb(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* b = static_cast<Busy*>(lv_obj_get_user_data(obj));
    if (!b) return;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.radius = LV_RADIUS_CIRCLE;
    r.bg_color = C(b->track);
    lv_draw_rect(lv_event_get_layer(e), &r, &a);
    const float phase = static_cast<float>(lv_tick_get() % 1100) / 1100.0f;
    const int32_t seg = w / 4, x = a.x1 - seg + static_cast<int32_t>((w + seg) * phase);
    const int32_t l = std::max(a.x1, x), rr = std::min(a.x2 + 1, x + seg);
    if (rr - l <= 0) return;
    const int32_t hh = std::min(h, rr - l), cy = a.y1 + h / 2;
    r.bg_color = C(b->fill);
    const lv_area_t f{l, cy - hh / 2, rr - 1, cy - hh / 2 + hh - 1};
    lv_draw_rect(lv_event_get_layer(e), &r, &f);
}

lv_obj_t* busy_bar(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t track = kDark,
                   uint32_t fill = kWhite) {
    lv_obj_t* o = plain(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_user_data(o, new Busy{track, fill});
    lv_obj_add_event_cb(o, busy_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(
        o, [](lv_event_t* e) {
            auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
            delete static_cast<Busy*>(lv_obj_get_user_data(obj));
            lv_obj_set_user_data(obj, nullptr);
        },
        LV_EVENT_DELETE, nullptr);
    // redraw ~30 times a second while it exists
    lv_timer_t* t = lv_timer_create([](lv_timer_t* t) { lv_obj_invalidate(static_cast<lv_obj_t*>(lv_timer_get_user_data(t))); },
                                    33, o);
    lv_obj_add_event_cb(
        o, [](lv_event_t* e) { lv_timer_delete(static_cast<lv_timer_t*>(lv_event_get_user_data(e))); }, LV_EVENT_DELETE, t);
    return o;
}

void anim(void* var, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t ms,
          lv_anim_path_cb_t path = lv_anim_path_ease_out, uint32_t delay = 0) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}

void set_x(void* o, int32_t v) { lv_obj_set_x(static_cast<lv_obj_t*>(o), v); }
void set_y(void* o, int32_t v) { lv_obj_set_y(static_cast<lv_obj_t*>(o), v); }
void set_w(void* o, int32_t v) { lv_obj_set_width(static_cast<lv_obj_t*>(o), v); }
void set_opa(void* o, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(o), static_cast<lv_opa_t>(v), 0);
}

// F / Z / X / C are the CardputerZero's arrow keys (printed in orange). Like the stock
// launcher, they work as arrows on their own on screens that don't take text.
bool is_up(uint32_t k) { return k == LV_KEY_UP || k == LV_KEY_LEFT || k == ';' || k == ',' || k == 'f' || k == 'z'; }
bool is_down(uint32_t k) { return k == LV_KEY_DOWN || k == LV_KEY_RIGHT || k == '.' || k == '/' || k == 'x' || k == 'c'; }
bool is_enter(uint32_t k) { return k == LV_KEY_ENTER || k == ' '; }
bool is_left(uint32_t k) { return k == LV_KEY_LEFT || k == ',' || k == '-' || k == 'z'; }
bool is_right(uint32_t k) { return k == LV_KEY_RIGHT || k == '/' || k == '=' || k == '+' || k == 'c'; }
bool is_row_up(uint32_t k) { return k == LV_KEY_UP || k == ';' || k == 'f'; }
bool is_row_down(uint32_t k) { return k == LV_KEY_DOWN || k == '.' || k == 'x'; }

// ---- content ----
const char* kHubItems[] = {"Terminal", "Browser", "Music", "Camera", "Gallery", "Sensors", "Network", "Bluetooth", "HDMI", "System"};
const Icon kHubIcons[] = {IcTerminal, IcBrowser, IcMusic, IcCamera, IcGallery, IcSensors, IcNet, IcBt, IcHdmi, IcSystem};
const Page kHubPages[] = {Page::Terminal, Page::Browser, Page::Music, Page::Camera, Page::Gallery,
                          Page::Sensors,  Page::Net,     Page::Bluetooth, Page::Hdmi, Page::System};
constexpr int kHubCount = 10;
constexpr int kHubVisible = 5;   // menu rows on screen; the list scrolls past that
constexpr int32_t kHubY0 = 29;   // first menu row
constexpr int32_t kHubDy = 24;   // menu row spacing
constexpr int32_t kHubRowH = 22; // a row's pill
constexpr int kWifiRows = 4;     // visible network rows
constexpr size_t kMaxPassword = 63;
constexpr size_t kMaxEapField = 96;   // identity / anonymous identity / password of an enterprise login

struct Stat {
    const char* name;
    int value;
};
const Stat kStats[] = {{"Code", 91}, {"CAD", 74}, {"Lore", 63}, {"Grit", 82}, {"Sleep", 38}};
constexpr int kStatCount = 5;

const char* rank_for(int v) {
    return v >= 90 ? "S" : v >= 80 ? "A" : v >= 70 ? "B" : v >= 60 ? "C" : "D";
}

bool write_png(const std::string& path, lv_draw_buf_t* snap) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        if (png) png_destroy_write_struct(&png, info ? &info : nullptr);
        std::fclose(fp);
        return false;
    }
    const auto w = snap->header.w, h = snap->header.h;
    png_init_io(png, fp);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    std::vector<uint8_t> row(w * 4);
    const auto* src = static_cast<const uint8_t*>(snap->data);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* s = src + y * snap->header.stride;
        for (uint32_t x = 0; x < w; ++x) {
            row[x * 4 + 0] = s[x * 4 + 2];
            row[x * 4 + 1] = s[x * 4 + 1];
            row[x * 4 + 2] = s[x * 4 + 0];
            row[x * 4 + 3] = 255;
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(fp);
    return true;
}

} // namespace

// =====================================================================

namespace {
// Small settings, one number per file in ~/.config/optic-suite/<name>.
std::string setting_file(const char* name) {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::string base;
    if (xdg && xdg[0]) base = xdg;
    else if (home && home[0]) base = std::string(home) + "/.config";
    else return {};
    static bool adopted = false;
    if (!adopted) { adopted = true; adopt_old_dir(base + "/velvet", base + "/optic-suite"); }
    return base + "/optic-suite/" + name;
}

#ifdef __EMSCRIPTEN__
// Browser demo: open a page in the phone's / computer's own browser (a new tab). 1 = opened.
EM_JS(int, web_open_external, (const char* url), {
    try { return window.velvetOpenExternal ? window.velvetOpenExternal(UTF8ToString(url)) : 0; } catch (e) { return 0; }
});

// In the browser, settings live in localStorage (the in-memory file system resets on reload).
EM_JS(int, web_load_setting, (const char* name, int def), {
    try { const v = localStorage.getItem('velvet-' + UTF8ToString(name)); return v === null ? def : (v | 0); }
    catch (e) { return def; }
});
EM_JS(void, web_save_setting, (const char* name, int v), {
    try { localStorage.setItem('velvet-' + UTF8ToString(name), String(v)); } catch (e) {}
});
#endif

int load_setting(const char* name, int def) {
#ifdef __EMSCRIPTEN__
    return web_load_setting(name, def);
#endif
    const std::string path = setting_file(name);
    FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "r");
    if (!f) return def;
    int t = def;
    if (std::fscanf(f, "%d", &t) != 1) t = def;
    std::fclose(f);
    return t;
}

void save_setting(const char* name, int t);

void save_setting(const char* name, int t) {
#ifdef __EMSCRIPTEN__
    web_save_setting(name, t);
    return;
#endif
    const std::string path = setting_file(name);
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        std::fprintf(f, "%d\n", t);
        std::fclose(f);
    }
}
}  // namespace

VelvetUI::VelvetUI(app::AssetManager& assets, QuitFn quit, void* quit_user)
    : assets_(assets), quit_(quit), quit_user_(quit_user) {}

VelvetUI::~VelvetUI() {
    rep_stop();
    clear_page_timers();
    if (cam_buf_) lv_draw_buf_destroy(cam_buf_);
    music_.reset();   // stops the player
    if (mus_vid_buf_) lv_draw_buf_destroy(mus_vid_buf_);
    if (splash_timer_) lv_timer_delete(splash_timer_);
    if (logo_buf_) lv_draw_buf_destroy(logo_buf_);
    if (quit_hint_timer_) lv_timer_delete(quit_hint_timer_);
    if (shot_timer_) lv_timer_delete(shot_timer_);
    if (status_timer_) lv_timer_delete(status_timer_);
    if (link_timer_) lv_timer_delete(link_timer_);
    slink_.reset();
    if (render_cb_added_) {
        lv_display_remove_event_cb_with_user_data(lv_display_get_default(), render_ready_cb, this);
    }
}

void VelvetUI::build() {
    // Every glyph is one solid colour: all text uses Terminus's hand-drawn bitmaps (exact
    // pixels, 1 bpp) at three sizes.
    f_.big = ter(velvet_ter_b18);     // headings, big numbers (10x18)
    f_.mono12 = ter(velvet_ter_b16);  // rows, body text (8x16)
    f_.mono10 = ter(velvet_ter_b14);  // small text, hints (8x14)
    f_.bold11 = ter(velvet_ter_b14);

    // No anti-aliasing anywhere: hard pixel edges on shapes too.
    lv_display_set_antialiasing(lv_display_get_default(), false);

    device_ = make_device();
    nett_ = make_net_tools();
    tor_ = make_tor();
    slink_ = make_screen_link();
    link_timer_ = lv_timer_create(link_tick_cb, 33, this);
    if (const char* l = std::getenv("VELVET_LINK"); l && *l == '1') slink_->set_on(true);   // tests
    sound_ = std::make_unique<Sound>();
    sound_->set_enabled(load_setting("sound", 1) != 0);
    if (device_->is_demo() && device_->volume() >= 0) sound_->set_gain(device_->volume() / 100.0f);
    theme_ = load_setting("ui_theme", 1) == 1 ? 1 : 0;   // before anything is drawn (White unless set to Dark)
    apply_theme(theme_);
    tutorial_at_start_ = load_setting("tutorial", 1) != 0;
    tutorial_pending_ = tutorial_at_start_;
    build_ui();
    started_ms_ = lv_tick_get();
    if (load_setting("eco", 0) == 1) eco_set(true);
    splash_start();

    lv_display_add_event_cb(lv_display_get_default(), render_ready_cb, LV_EVENT_RENDER_READY, this);
    render_cb_added_ = true;

    // network + battery status for the status pills, every 5 s
    auto poll = [](lv_timer_t* t) {
        auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
        self->device_->net_status([self](NetStatus st) {
            self->net_ = st;
            self->update_hub_status();
        });
        self->update_hub_status();
    };
    status_timer_ = lv_timer_create(poll, 5000, this);
    poll(status_timer_);
}

// Creates the whole screen. The old screen, if any, is deleted after the new one is shown.
void VelvetUI::build_ui() {
    lv_obj_t* old = root_;
    root_ = lv_obj_create(nullptr);
    lv_obj_remove_style_all(root_);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root_, C(kBg), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

    hub_ = plain(root_);
    lv_obj_set_size(hub_, 320, 170);
    build_hub(hub_);
    lv_screen_load(root_);
    if (old) lv_obj_delete(old);
}

void VelvetUI::update_hub_status() {
    for (lv_obj_t* o : {hub_status_, page_status_})
        if (o) lv_obj_invalidate(o);
    if (hub_values_.size() == static_cast<size_t>(kHubCount)) {
        // the Wi-Fi and System rows show the network and the battery, like MinUI's option values
        const PowerInfo pw = device_->power();
        // the HDMI row: asked for now and then only (it runs a command), and at once after a change
        if (hdmi_hub_age_-- <= 0) {
            hdmi_ = device_->hdmi();
            hdmi_hub_age_ = 10;
        }
        char buf[48];
        for (int i = 0; i < kHubCount; ++i) {
            buf[0] = 0;
            if (kHubPages[i] == Page::Hdmi) {
                if (!hdmi_.supported) std::snprintf(buf, sizeof buf, "Not here");
                else if (hdmi_.running && !hdmi_.hz.empty()) std::snprintf(buf, sizeof buf, "On  %s Hz", hdmi_.hz.c_str());
                else std::snprintf(buf, sizeof buf, "%s", hdmi_.running ? "On" : hdmi_.connected ? "Off" : "No screen");
            } else if (kHubPages[i] == Page::Bluetooth) {
                if (!bt_) bt_ = make_btaudio();
                if (bt_hub_age_-- <= 0) { bt_->refresh(); bt_hub_age_ = 15; }
                const BtAudioState b = bt_->state();
                std::string who;
                for (const auto& d : b.devices) if (d.connected) { who = d.name; break; }
                std::snprintf(buf, sizeof buf, "%.16s", who.c_str());
            } else if (kHubPages[i] == Page::Net) {
                std::snprintf(buf, sizeof buf, "%.16s", !net_.radio_on ? "Off" : net_.connected ? net_.ssid.c_str() : "Not connected");
            } else if (kHubPages[i] == Page::System && pw.valid) {
                std::snprintf(buf, sizeof buf, "%d%%%s", pw.percent, pw.charging ? " +" : "");
            }
            lv_label_set_text(hub_values_[i], buf);
            lv_obj_set_x(hub_values_[i], 300 - text_width(hub_values_[i]));
        }
    }
}

// ---------------------------------------------------------------- status pill
// MinUI's top-right pill: the time, Wi-Fi bars and the battery. Drawn in one callback so the
// 5 s status poll only has to invalidate it.
void VelvetUI::status_draw_cb(lv_event_t* e) {
    auto* self = static_cast<VelvetUI*>(lv_event_get_user_data(e));
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const bool day = lv_obj_get_user_data(obj) != nullptr;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    auto rect = [&](int32_t x, int32_t y, int32_t w, int32_t h, uint32_t col) {
        r.bg_color = C(col);
        const lv_area_t b{x, y, x + w - 1, y + h - 1};
        lv_draw_rect(layer, &r, &b);
    };
    // time (and day on the hub)
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char t[16];
    std::strftime(t, sizeof t, day ? "%a %H:%M" : "%H:%M", &tm);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = self->f_.mono10;
    d.color = C(kWhite);
    d.text = t;
    d.text_local = 1;
    int32_t x = a.x1 + 8;
    const int32_t tw = utf8_cols(t) * 8;
    lv_area_t ta{x, a.y1 + 3, x + tw + 8, a.y1 + 17};
    lv_draw_label(layer, &d, &ta);
    x += tw + 7;
    // Wi-Fi: four bars (all dim = not connected, nothing = radio off)
    const NetStatus& n = self->net_;
    if (n.radio_on) {
        for (int i = 0; i < 4; ++i) {
            const int32_t h = 3 + i * 2;
            rect(x + i * 3, a.y1 + 15 - h, 2, h, n.connected ? kWhite : kFaint);
        }
        x += 16;
    }
    // battery: the percentage; charging, a green lightning bolt on its left
    const PowerInfo pw = self->device_->power();
    if (pw.charging && pw.valid) {
        static const int8_t kBolt[7][2] = {{3, 4}, {2, 3}, {1, 2}, {0, 5}, {3, 4}, {2, 3}, {1, 2}};
        for (int row = 0; row < 7; ++row)
            rect(x + kBolt[row][0], a.y1 + 6 + row, kBolt[row][1] - kBolt[row][0] + 1, 1, 0x3fb950);
        x += 8;
    }
    char pc[8];
    std::snprintf(pc, sizeof pc, "%s", pw.valid ? (std::to_string(std::clamp(pw.percent, 0, 100)) + "%").c_str() : "--");
    lv_draw_label_dsc_t bd;
    lv_draw_label_dsc_init(&bd);
    bd.font = self->f_.mono10;
    bd.color = C(pw.valid && pw.percent <= 15 && !pw.charging ? kGrey : kWhite);
    bd.text = pc;
    bd.text_local = 1;
    lv_area_t ba{x, a.y1 + 3, x + 40, a.y1 + 17};
    lv_draw_label(layer, &bd, &ba);
}

lv_obj_t* VelvetUI::status_pill(lv_obj_t* parent, int32_t right, int32_t y, bool day) {
    const int32_t w = 8 + (day ? 9 : 5) * 8 + 7 + 16 + 8 + 32 + 6;   // time, Wi-Fi, bolt, "100%"
    lv_obj_t* o = pill(parent, right - w, y, w, 20, kDark);
    lv_obj_set_user_data(o, day ? reinterpret_cast<void*>(1) : nullptr);
    lv_obj_add_event_cb(o, status_draw_cb, LV_EVENT_DRAW_MAIN, this);
    return o;
}

// ---------------------------------------------------------------- hub
// MinUI's main list: a title pill and the status pill on top, five rows (a white pill on the
// selected one), button hints along the bottom.
void VelvetUI::build_hub(lv_obj_t* p) {
    text_pill(p, "Optic Suite", f_.mono10, kDark, kWhite, 4, 4, 20, 10);
    hub_status_ = status_pill(p, 316, 4, true);

    hub_plate_ = pill(p, 4, kHubY0, 302, kHubRowH, kWhite);
    hub_labels_.clear();
    hub_icons_.clear();
    hub_values_.clear();
    for (int i = 0; i < kHubCount; ++i) {
        hub_icons_.push_back(icon(p, kHubIcons[i], 14, 0, kWhite));
        hub_labels_.push_back(label(p, kHubItems[i], f_.mono12, kWhite, 36, 0));
        hub_values_.push_back(label(p, "", f_.mono10, kGrey, 200, 0));
    }
    // scroll bar on the right edge
    pill(p, 311, kHubY0, 4, kHubVisible * kHubDy - (kHubDy - kHubRowH), kDark);
    hub_thumb_ = pill(p, 311, kHubY0, 4, 20, kWhite);

    // bottom bar: position on the left, hints on the right
    hub_count_ = label(p, "", f_.mono10, kFaint, 8, 152);
    hub_hints_ = hints_r(p, 312, 151, f_.mono10, {{"OK", "Open"}, {"ESC", "Hold: quit"}});

    // first start: a blinking "Press any key" (any key opens the tutorial)
    hub_prompt_ = text_pill(p, "Press any key", f_.mono10, kWhite, kInk, 0, 151, 16, 10);
    lv_obj_set_x(hub_prompt_, 312 - lv_obj_get_style_width(hub_prompt_, LV_PART_MAIN));
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, hub_prompt_);
        lv_anim_set_exec_cb(&a, [](void* o, int32_t v) {  // white pill <-> dark pill
            auto* obj = static_cast<lv_obj_t*>(o);
            const bool on = v > 128;
            lv_obj_set_style_bg_color(obj, C(on ? kWhite : kDark), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(obj, 0), C(on ? kInk : kWhite), 0);
        });
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_duration(&a, 600);
        lv_anim_set_playback_duration(&a, 600);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
    if (!tutorial_pending_) lv_obj_add_flag(hub_prompt_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(hub_hints_, LV_OBJ_FLAG_HIDDEN);

    quit_hint_ = text_pill(p, "Hold ESC to quit", f_.mono10, kDark, kWhite, 4, 151, 16, 10);
    lv_obj_add_flag(quit_hint_, LV_OBJ_FLAG_HIDDEN);

    update_hub_status();
    hub_select(hub_index_, false);
}

void VelvetUI::hub_select(int index, bool animate) {
    if (animate) sound_->play(Sfx::Move);
    hub_index_ = (index + kHubCount) % kHubCount;
    if (hub_index_ < hub_top_) hub_top_ = hub_index_;
    if (hub_index_ >= hub_top_ + kHubVisible) hub_top_ = hub_index_ - kHubVisible + 1;
    if (!hub_prompt_ || lv_obj_has_flag(hub_prompt_, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(hub_hints_, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < kHubCount; ++i) {
        const int row = i - hub_top_;
        const bool show = row >= 0 && row < kHubVisible;
        for (lv_obj_t* o : {hub_labels_[i], hub_icons_[i], hub_values_[i]}) {
            if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
        if (!show) continue;
        const bool sel = i == hub_index_;
        const int32_t y = kHubY0 + row * kHubDy;
        lv_obj_set_y(hub_labels_[i], y + 3);
        lv_obj_set_y(hub_icons_[i], y + 4);
        lv_obj_set_y(hub_values_[i], y + 4);
        lv_obj_set_style_text_color(hub_labels_[i], C(sel ? kInk : kWhite), 0);
        lv_obj_set_style_text_color(hub_values_[i], C(sel ? kInk : kGrey), 0);
        lv_obj_set_style_text_color(hub_icons_[i], C(sel ? kInk : kWhite), 0);
        lv_obj_invalidate(hub_icons_[i]);
    }
    const int32_t y = kHubY0 + (hub_index_ - hub_top_) * kHubDy;
    if (animate) anim(hub_plate_, set_y, lv_obj_get_style_y(hub_plate_, LV_PART_MAIN), y, 90, lv_anim_path_ease_out);
    else lv_obj_set_y(hub_plate_, y);
    // scroll bar: the thumb covers the visible rows
    const int32_t track = kHubVisible * kHubDy - (kHubDy - kHubRowH);
    const int32_t th = track * kHubVisible / kHubCount;
    lv_obj_set_height(hub_thumb_, th);
    lv_obj_set_y(hub_thumb_, kHubY0 + (track - th) * hub_top_ / std::max(1, kHubCount - kHubVisible));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d/%d", hub_index_ + 1, kHubCount);
    lv_label_set_text(hub_count_, buf);
}

void VelvetUI::show_quit_hint() {
    sound_->play(Sfx::ToggleOff);
    lv_obj_remove_flag(quit_hint_, LV_OBJ_FLAG_HIDDEN);
    if (quit_hint_timer_) lv_timer_delete(quit_hint_timer_);
    quit_hint_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
            lv_obj_add_flag(self->quit_hint_, LV_OBJ_FLAG_HIDDEN);
            self->quit_hint_timer_ = nullptr;
        },
        1600, this);
    lv_timer_set_repeat_count(quit_hint_timer_, 1);
}

// ---------------------------------------------------------------- transitions
void VelvetUI::transition(bool forward, void (*swap)(VelvetUI*)) {
    LV_UNUSED(forward);
    transitioning_ = true;
    // A quick dip to black: the content swaps while the screen is black.
    lv_obj_t* cover = plain(root_);
    lv_obj_set_size(cover, 320, 170);
    lv_obj_set_style_bg_color(cover, C(kBg), 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(cover, LV_OPA_TRANSP, 0);

    struct Ctx {
        VelvetUI* self;
        void (*swap)(VelvetUI*);
        lv_obj_t* cover;
    };
    auto* ctx = new Ctx{this, swap, cover};

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cover);
    lv_anim_set_exec_cb(&a, set_opa);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, 80);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_user_data(&a, ctx);
    lv_anim_set_completed_cb(&a, [](lv_anim_t* a) {
        auto* c = static_cast<Ctx*>(lv_anim_get_user_data(a));
        c->swap(c->self);
        lv_obj_move_foreground(c->cover);
        lv_anim_t b;
        lv_anim_init(&b);
        lv_anim_set_var(&b, c->cover);
        lv_anim_set_exec_cb(&b, set_opa);
        lv_anim_set_values(&b, 255, 0);
        lv_anim_set_duration(&b, 110);
        lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
        lv_anim_set_user_data(&b, c);
        lv_anim_set_completed_cb(&b, [](lv_anim_t* b) {
            auto* c = static_cast<Ctx*>(lv_anim_get_user_data(b));
            lv_obj_delete(c->cover);
            c->self->transitioning_ = false;
            delete c;
        });
        lv_anim_start(&b);
    });
    lv_anim_start(&a);
}

void VelvetUI::swap_to_target(VelvetUI* self) {
    lv_obj_add_flag(self->hub_, LV_OBJ_FLAG_HIDDEN);
    if (self->page_) {   // page to page (NETWORK <-> Wi-Fi)
        self->clear_page_timers();
        lv_obj_delete(self->page_);
        self->page_ = nullptr;
    }
    self->build_page(self->target_);
}

void VelvetUI::swap_to_hub(VelvetUI* self) {
    self->clear_page_timers();
    if (self->page_) {
        lv_obj_delete(self->page_);
        self->page_ = nullptr;
    }
    self->page_kind_ = Page::Hub;
    lv_obj_remove_flag(self->hub_, LV_OBJ_FLAG_HIDDEN);
    self->hub_select(self->hub_index_, true);
}

void VelvetUI::open_page(Page page) {
    sound_->play(Sfx::Swoosh);  // just the slide: nothing layered on top
    target_ = page;
    transition(true, swap_to_target);
}

void VelvetUI::close_page() {
    sound_->play(Sfx::Swoosh);  // just the slide: nothing layered on top
    if (page_kind_ == Page::Wifi && wifi_from_net_) {   // Wi-Fi came from NETWORK: back there (build_net clears the flag)
        target_ = Page::Net;
        transition(false, swap_to_target);
        return;
    }
    wifi_from_net_ = false;
    transition(false, swap_to_hub);
}

void VelvetUI::clear_page_timers() {
    if (net_timer_) {
        lv_timer_delete(net_timer_);
        net_timer_ = nullptr;
    }
    if (nett_ && page_kind_ == Page::Net) {   // leaving NETWORK: stop what it started
        nett_->speed_cancel();
        nett_->tool_cancel();
    }
    net_canvas_ = net_top_ = net_hint_ = nullptr;
    if (wifi_timer_) {
        lv_timer_delete(wifi_timer_);
        wifi_timer_ = nullptr;
    }
    if (sys_timer_) {
        lv_timer_delete(sys_timer_);
        sys_timer_ = nullptr;
    }
    if (bt_timer_) {
        lv_timer_delete(bt_timer_);
        bt_timer_ = nullptr;
    }
    bt_box_ = bt_note_ = nullptr;
    if (hdmi_timer_) {
        lv_timer_delete(hdmi_timer_);
        hdmi_timer_ = nullptr;
    }
    hdmi_big_ = hdmi_sub_ = hdmi_val_ = hdmi_note_ = nullptr;
    if (term_timer_) {
        lv_timer_delete(term_timer_);
        term_timer_ = nullptr;
    }
    if (sens_timer_) {
        lv_timer_delete(sens_timer_);
        sens_timer_ = nullptr;
    }
    if (pwn_timer_) {
        lv_timer_delete(pwn_timer_);
        pwn_timer_ = nullptr;
    }
    pwn_face_ = pwn_face_img_ = pwn_name_ = pwn_tag_ = pwn_notice_ = pwn_line_ = pwn_stats_ = pwn_about_ = nullptr;
    pwn_face_path_.clear();
    pwn_shown_mood_ = -1;
    pwn_about_open_ = false;
    if (btimer_) {
        lv_timer_delete(btimer_);
        btimer_ = nullptr;
    }
    if (gal_timer_) {
        lv_timer_delete(gal_timer_);
        gal_timer_ = nullptr;
    }
    if (mus_timer_) {
        lv_timer_delete(mus_timer_);
        mus_timer_ = nullptr;
    }
    mus_rows_.clear();
    mus_durs_.clear();
    mus_box_ = mus_query_lbl_ = mus_sel_bg_ = mus_msg_ = mus_now_ = mus_time_ = mus_fill_ = nullptr;
    mus_vid_ = mus_vid_img_ = mus_vid_bar_ = mus_vid_info_ = nullptr;
    mus_full_ = mus_art_img_ = nullptr;
    mus_esc_pending_ = false;
    if (music_ && mus_status().video) music_->stop();   // video doesn't carry on in the background
    if (gal_src_) { lv_draw_buf_destroy(gal_src_); gal_src_ = nullptr; }
    if (gal_out_) { lv_draw_buf_destroy(gal_out_); gal_out_ = nullptr; }
    gal_cells_.clear();
    gal_imgs_.clear();
    gal_sel_box_ = gal_count_ = gal_empty_ = gal_view_ = gal_view_img_ = gal_info_ = nullptr;
    gal_open_ = -1;
    if (chrome_) chrome_->stop();  // frees Chromium's memory; F2 / reopening starts it again
    bview_ = btool_ = blist_ = bload_ = bfbox_ = bengine_ = nullptr;
    bpan_key_ = 0;
    bfield_ = -1;
    b_esc_pending_ = false;
    sens_values_ = nullptr;
    compass_ = level_ = nullptr;
    if (cam_timer_) {
        lv_timer_delete(cam_timer_);
        cam_timer_ = nullptr;
        if (camera_) camera_->stop();  // free the sensor (and the phone camera) when leaving
    }
    cam_img_ = nullptr;
    cam_banner_until_ = 0;
    term_view_ = nullptr;
    term_info_ = nullptr;
    term_banner_ = nullptr;
    term_esc_pending_ = false;
    ++wifi_gen_;  // drop late Wi-Fi callbacks for this page
    wifi_rows_.clear();
    wifi_action_plates_.clear();
    wifi_action_labels_.clear();
    wifi_overlay_ = nullptr;
    wifi_hint_ = nullptr;
    wifi_status_ = nullptr;
    wifi_scan_label_ = nullptr;
    wifi_mode_ = WifiMode::List;
    fps_label_ = nullptr;
    sys_batt_ = nullptr;
    page_status_ = nullptr;
    stat_names_.clear();
    stat_markers_.clear();
    stat_parts_.clear();
}

// ---------------------------------------------------------------- pages
void VelvetUI::build_page(Page page) {
    page_kind_ = page;
    page_ = plain(root_);
    lv_obj_set_size(page_, 320, 170);
    lv_obj_set_style_bg_color(page_, C(kBg), 0);
    lv_obj_set_style_bg_opa(page_, LV_OPA_COVER, 0);
    switch (page) {
        case Page::Stats: build_stats(page_); break;
        case Page::Wifi: build_wifi(page_); break;
        case Page::Net: build_net(page_); break;
        case Page::System: build_system(page_); break;
        case Page::Hdmi: build_hdmi(page_); break;
        case Page::Bluetooth: build_bluetooth(page_); break;
        case Page::Terminal: build_terminal(page_); break;
        case Page::Camera: build_camera(page_); break;
        case Page::Sensors: build_sensors(page_); break;
        case Page::Pwn: build_pwn(page_); break;
        case Page::Browser: build_browser(page_); break;
        case Page::Gallery: build_gallery(page_); break;
        case Page::Music: build_music(page_); break;
        default: break;
    }
}

// The top bar every page shares: the title in a dark pill (left), ESC BACK, the status pill (right).
void VelvetUI::build_page_chrome(lv_obj_t* p, const char* title, bool back_hint) {
    text_pill(p, title, f_.mono10, kDark, kWhite, 4, 4, 20, 10);
    page_status_ = status_pill(p, 316, 4, false);
    if (back_hint) hints_r(p, lv_obj_get_style_x(page_status_, LV_PART_MAIN) - 8, 6, f_.mono10, {{"ESC", "Back"}});
}

void VelvetUI::build_stats(lv_obj_t* p) {
    build_page_chrome(p, "Stats");
    stat_parts_.clear();
    for (int i = 0; i < kStatCount; ++i) {
        const int32_t y = 30 + i * 24;
        const Stat& s = kStats[i];
        lv_obj_t* row = pill(p, 4, y, 312, 22, kWhite);
        lv_obj_t* name = label(p, s.name, f_.mono12, kWhite, 16, y + 3);
        lv_obj_t* track = pill(p, 88, y + 8, 150, 6, kDark);
        const int32_t fill_w = 6 + (150 - 6) * s.value / 100;
        lv_obj_t* fill = pill(p, 88, y + 8, 6, 6, kWhite);
        anim(fill, set_w, 6, fill_w, 380, lv_anim_path_ease_out, 60 + i * 50);
        char num[8];
        std::snprintf(num, sizeof num, "%d", s.value);
        lv_obj_t* val = label(p, num, f_.mono10, kGrey, 248, y + 4);
        lv_obj_t* rank_plate = pill(p, 280, y + 3, 28, 16, kDark);
        lv_obj_t* rank = label(rank_plate, rank_for(s.value), f_.mono10, kWhite, 10, 1);
        stat_names_.push_back(name);
        stat_markers_.push_back(row);
        for (lv_obj_t* o : {track, fill, val, rank_plate, rank}) stat_parts_.push_back(o);
    }
    list_index_ = 0;
    stats_select(0);
}

void VelvetUI::stats_select(int index) {
    if (index != list_index_) sound_->play(Sfx::Move);
    list_index_ = (index + kStatCount) % kStatCount;
    for (int i = 0; i < kStatCount; ++i) {
        const bool sel = i == list_index_;
        lv_obj_t* const* q = &stat_parts_[i * 5];  // track, fill, value, rank pill, rank letter
        lv_obj_set_style_text_color(stat_names_[i], C(sel ? kInk : kWhite), 0);
        if (sel) lv_obj_remove_flag(stat_markers_[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(stat_markers_[i], LV_OBJ_FLAG_HIDDEN);
        pill_set_color(q[0], sel ? kLight : kDark);
        pill_set_color(q[1], sel ? kInk : kWhite);
        lv_obj_set_style_text_color(q[2], C(sel ? kInk : kGrey), 0);
        pill_set_color(q[3], sel ? kInk : kDark);
    }
}

namespace {
enum SysRow { SysBright, SysVolume, SysSound, SysTheme, SysPower, SysTutorial, SysBoot, SysR36s };
const char* const kSysNames[] = {"Brightness", "Volume", "Sound", "Theme", "Power", "Tutorial", "Boot", "R36S"};
constexpr int32_t kSysX = 126, kSysW = 190, kSysY0 = 30, kSysDy = 15;   // (8 rows above the hints)
}  // namespace

void VelvetUI::build_system(lv_obj_t* p) {
    build_page_chrome(p, "System");
    if (device_->is_demo()) label(p, "Demo", f_.mono10, kFaint, 80, 7);

    // left card: battery, network, host, frame rate
    lv_obj_t* card = pill(p, 4, 30, 116, 136, kDark, 10);
    sys_batt_ = label(card, "--", f_.big, kWhite, 8, 6);
    sys_batt_shadow_ = nullptr;
    pill(card, 8, 28, 100, 6, kBg);
    sys_batt_bar_ = pill(card, 8, 28, 6, 6, kWhite);
    sys_batt_info_ = label(card, "", f_.mono10, kGrey, 8, 40);
    lv_obj_set_style_text_line_space(sys_batt_info_, 1, 0);
    sys_ip_ = label(card, "", f_.mono10, kWhite, 8, 74);
    lv_obj_set_width(sys_ip_, 104);
    lv_label_set_long_mode(sys_ip_, LV_LABEL_LONG_CLIP);
    lv_obj_t* host = label(card, device_->hostname().c_str(), f_.mono10, kGrey, 8, 89);
    lv_obj_set_width(host, 104);
    lv_label_set_long_mode(host, LV_LABEL_LONG_DOT);
    fps_label_ = label(card, "", f_.mono10, kGrey, 8, 104);
    busy_bar(card, 8, 124, 100, 4, kBg, kGrey);  // constant motion so the FPS number means something

    // right: the option list, MinUI style (name left, value right)
    // Only what can be set here: a row whose thing this device doesn't have (no backlight control,
    // no mixer, no sound output, no R36S link) is left out, and the rest move up. Boot has a single
    // animation, so there is nothing to choose: it is left out too.
    sys_vis_.clear();
    for (int i = 0; i < kSysRows; ++i) {
        const bool have = i == SysBright ? device_->brightness() >= 0
                        : i == SysVolume ? device_->volume() >= 0
                        : i == SysSound ? sound_->available()
                        : i == SysR36s ? slink_->state() != ScreenLink::Unavailable
                        : i == SysBoot ? false
                        : true;
        if (have) sys_vis_.push_back(i);
    }
    for (int i = 0; i < kSysRows; ++i) {
        const auto at = std::find(sys_vis_.begin(), sys_vis_.end(), i);
        const bool shown = at != sys_vis_.end();
        const int32_t y = kSysY0 + (shown ? static_cast<int32_t>(at - sys_vis_.begin()) : 0) * kSysDy;
        sys_markers_[i] = pill(p, kSysX, y, kSysW, 15, kWhite);
        sys_rows_[i] = label(p, kSysNames[i], f_.mono10, kWhite, kSysX + 8, y + 1);
        if (i == SysBright || i == SysVolume) {
            sys_tracks_[i] = pill(p, kSysX + kSysW - 8 - 24 - 6 - 52, y + 4, 52, 6, kDark);
            sys_fills_[i] = pill(p, kSysX + kSysW - 8 - 24 - 6 - 52, y + 4, 6, 6, kWhite);
            sys_values_[i] = label(p, "", f_.mono10, kGrey, 0, y + 1);
        } else {
            sys_tracks_[i] = sys_fills_[i] = nullptr;
            sys_values_[i] = label(p, "", f_.mono10, kGrey, 0, y + 1);
            lv_label_set_long_mode(sys_values_[i], LV_LABEL_LONG_DOT);
        }
        if (!shown)
            for (lv_obj_t* o : {sys_markers_[i], sys_rows_[i], sys_tracks_[i], sys_fills_[i], sys_values_[i]})
                if (o) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    hints_r(p, 316, 151, f_.mono10, {{"\xE2\x86\x91\xE2\x86\x93", "Pick"}, {"\xE2\x86\x90\xE2\x86\x92 OK", "Set"}});

    sys_sel_ = sys_vis_.empty() ? 0 : sys_vis_.front();
    frames_ = 0;
    system_render();
    lv_label_set_text(fps_label_, "-- FPS  00:00");
    sys_timer_ = lv_timer_create(system_tick_cb, 1000, this);
}

// ---------------------------------------------------------------- HDMI
// The Optic desktop on the HDMI screen, from this app: what is plugged in, the mode and refresh
// rate it runs at, and one switch that starts or stops it. The desktop is its own program
// (optic-de-hdmi); this screen and the app keep running beside it.
// ---------------------------------------------------------------- Bluetooth
// One list: where sound goes, a test sound, each thing that is playing (it can be sent
// somewhere else on its own), the devices, and a scan.
namespace {
enum { kBtOutput, kBtTest, kBtStream, kBtDevice, kBtScan, kBtNone };
constexpr int kBtVisible = 5;
}

std::vector<VelvetUI::BtRow> VelvetUI::bt_rows() {
    std::vector<BtRow> r;
    const BtAudioState& s = bt_state_;
    if (!s.available) return r;
    auto out_name = [&](int i) { return i >= 0 && i < static_cast<int>(s.outputs.size()) ? s.outputs[i].label : std::string("--"); };
    const bool choice = s.outputs.size() > 1;
    r.push_back({kBtOutput, 0, "Sound goes to", choice ? "< " + out_name(s.default_output) + " >" : out_name(s.default_output)});
    r.push_back({kBtTest, 0, "Test sound", "OK"});
    if (choice) for (const auto& st : s.streams) r.push_back({kBtStream, st.id, "  " + st.app, "< " + out_name(st.output) + " >"});
    for (size_t i = 0; i < s.devices.size(); ++i) {
        const auto& d = s.devices[i];
        r.push_back({kBtDevice, static_cast<int>(i), d.name, d.connected ? "Connected" : d.paired ? "Paired" : "New"});
    }
    r.push_back({kBtScan, 0, "Look for devices", s.scanning ? "Looking..." : "OK"});
    return r;
}

void VelvetUI::build_bluetooth(lv_obj_t* p) {
    build_page_chrome(p, "Bluetooth");
    if (!bt_) bt_ = make_btaudio();
    bt_box_ = plain(p);
    lv_obj_set_pos(bt_box_, 0, 28);
    lv_obj_set_size(bt_box_, 320, kBtVisible * 20 + 2);
    bt_note_ = label(p, "", f_.mono10, kGrey, 8, 134);
    lv_obj_set_width(bt_note_, 304);
    lv_label_set_long_mode(bt_note_, LV_LABEL_LONG_DOT);
    hints_r(p, 316, 151, f_.mono10, {{"OK", "Choose"}, {"DEL", "Forget"}});
    bt_sel_ = bt_top_ = 0;
    bt_->refresh();
    bt_state_ = bt_->state();
    bt_render();
    bt_timer_ = lv_timer_create(bt_tick_cb, 700, this);
}

void VelvetUI::bt_render() {
    if (!bt_box_) return;
    lv_obj_clean(bt_box_);
    const std::vector<BtRow> rows = bt_rows();
    const BtAudioState& s = bt_state_;
    if (rows.empty()) {
        label(bt_box_, s.busy ? "Looking for Bluetooth..." : "No Bluetooth here", f_.mono12, kWhite, 12, 30);
        lv_label_set_text(bt_note_, "");
        return;
    }
    const int n = static_cast<int>(rows.size());
    bt_sel_ = std::max(0, std::min(n - 1, bt_sel_));
    if (bt_sel_ < bt_top_) bt_top_ = bt_sel_;
    if (bt_sel_ >= bt_top_ + kBtVisible) bt_top_ = bt_sel_ - kBtVisible + 1;
    bt_top_ = std::max(0, std::min(std::max(0, n - kBtVisible), bt_top_));
    for (int i = 0; i < kBtVisible && bt_top_ + i < n; ++i) {
        const BtRow& r = rows[bt_top_ + i];
        const bool sel = bt_top_ + i == bt_sel_;
        const int32_t y = 2 + i * 20;
        if (sel) pill(bt_box_, 4, y, 312, 18, kWhite);
        const uint32_t fg = sel ? kInk : kWhite, dim = sel ? kInk : kGrey;
        lv_obj_t* r_l = label(bt_box_, font_safe_text(f_.mono10, r.right).c_str(), f_.mono10, dim, 0, y + 3);
        const int32_t rw = text_width(r_l);
        lv_obj_set_x(r_l, 316 - 8 - rw);
        lv_obj_t* l_l = label(bt_box_, font_safe_text(f_.mono10, r.left).c_str(), f_.mono10, fg, 12, y + 3);
        lv_obj_set_width(l_l, 316 - 8 - rw - 8 - 12);
        lv_label_set_long_mode(l_l, LV_LABEL_LONG_DOT);
    }
    if (n > kBtVisible) {   // a thin scroll mark at the right edge
        const int32_t track = kBtVisible * 20, th = std::max<int32_t>(8, track * kBtVisible / n);
        pill(bt_box_, 317, 2 + (track - th) * bt_top_ / std::max(1, n - kBtVisible), 2, th, kGrey);
    }
    std::string note = s.note;
    if (note.empty()) {
        const BtRow& r = rows[bt_sel_];
        if (r.kind == kBtOutput) note = s.outputs.size() > 1 ? "< > moves all sound" : "Connect a device first";
        else if (r.kind == kBtStream) note = "< > moves only this one";
        else if (r.kind == kBtTest) note = "Plays left, right, both";
        else if (r.kind == kBtDevice) note = s.devices[r.ref].connected ? "OK disconnects" : s.devices[r.ref].paired ? "OK connects" : "OK pairs and connects";
        else if (r.kind == kBtScan) note = "Pairing mode first, then OK";
    }
    lv_label_set_text(bt_note_, note.c_str());
}

bool VelvetUI::bt_key(uint32_t key) {
    const std::vector<BtRow> rows = bt_rows();
    if (rows.empty()) return true;
    const int n = static_cast<int>(rows.size());
    bt_sel_ = std::max(0, std::min(n - 1, bt_sel_));
    const BtRow& r = rows[bt_sel_];
    const int outs = static_cast<int>(bt_state_.outputs.size());
    auto step_out = [&](int from, int d) { return outs > 0 ? ((from < 0 ? 0 : from) + d + outs) % outs : -1; };
    if (is_row_up(key)) { bt_sel_ = (bt_sel_ + n - 1) % n; sound_->play(Sfx::Move); }
    else if (is_row_down(key)) { bt_sel_ = (bt_sel_ + 1) % n; sound_->play(Sfx::Move); }
    else if (r.kind == kBtOutput && outs > 1 && (is_left(key) || is_right(key) || is_enter(key))) {
        bt_->set_default(step_out(bt_state_.default_output, is_left(key) ? -1 : 1));
        sound_->play(Sfx::Move);
    } else if (r.kind == kBtStream && outs > 1 && (is_left(key) || is_right(key) || is_enter(key))) {
        int cur = -1;
        for (const auto& st : bt_state_.streams) if (st.id == r.ref) cur = st.output;
        bt_->move_stream(r.ref, step_out(cur, is_left(key) ? -1 : 1));
        sound_->play(Sfx::Move);
    } else if (is_enter(key)) {
        if (r.kind == kBtTest) bt_->test();
        else if (r.kind == kBtScan) bt_->scan();
        else if (r.kind == kBtDevice) bt_->act(bt_state_.devices[r.ref].mac);
        sound_->play(Sfx::Open);
    } else if ((key == LV_KEY_DEL || key == LV_KEY_BACKSPACE) && r.kind == kBtDevice && bt_state_.devices[r.ref].paired) {
        bt_->forget(bt_state_.devices[r.ref].mac);
    }
    bt_state_ = bt_->state();
    bt_render();
    return true;
}

void VelvetUI::bt_tick_cb(lv_timer_t* t) {
    auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
    if (!self->bt_ || !self->bt_box_) return;
    if (++self->bt_beat_ % 3 == 0) self->bt_->refresh();
    self->bt_state_ = self->bt_->state();
    self->bt_render();
}

void VelvetUI::build_hdmi(lv_obj_t* p) {
    build_page_chrome(p, "HDMI");
    if (device_->is_demo()) label(p, "Demo", f_.mono10, kFaint, 80, 7);
    lv_obj_t* card = pill(p, 4, 30, 312, 62, kDark, 10);
    hdmi_big_ = label(card, "", f_.big, kWhite, 10, 6);
    hdmi_sub_ = label(card, "", f_.mono10, kGrey, 10, 38);
    lv_obj_set_width(hdmi_sub_, 292);
    lv_label_set_long_mode(hdmi_sub_, LV_LABEL_LONG_DOT);
    pill(p, 4, 98, 312, 17, kWhite);
    label(p, "Optic desktop", f_.mono10, kInk, 12, 100);
    hdmi_val_ = label(p, "", f_.mono10, kInk, 0, 100);
    hdmi_note_ = label(p, "", f_.mono10, kGrey, 8, 120);
    lv_obj_set_width(hdmi_note_, 304);
    lv_label_set_long_mode(hdmi_note_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(hdmi_note_, 1, 0);
    hints_r(p, 316, 151, f_.mono10, {{"OK", "On / Off"}});
    hdmi_busy_ = 0;
    hdmi_ = device_->hdmi();
    hdmi_render();
    hdmi_timer_ = lv_timer_create(hdmi_tick_cb, 1000, this);
}

void VelvetUI::hdmi_render() {
    if (!hdmi_big_) return;
    const HdmiInfo& h = hdmi_;
    char sub[96];
    const char* big;
    const char* note;
    if (!h.supported) {
        big = "Not here";
        std::snprintf(sub, sizeof sub, "The Optic desktop isn't installed");
        note = "Needs the optic-de package.";
    } else if (hdmi_busy_ > 0) {
        big = hdmi_want_ ? "Starting" : "Stopping";
        std::snprintf(sub, sizeof sub, "%s", hdmi_want_ ? "The desktop is coming up on HDMI" : "Closing the desktop");
        note = "";
    } else if (h.running) {
        big = "On";
        if (!h.mode.empty() && !h.hz.empty()) std::snprintf(sub, sizeof sub, "%s   %s Hz", h.mode.c_str(), h.hz.c_str());
        else if (!h.mode.empty()) std::snprintf(sub, sizeof sub, "%s", h.mode.c_str());
        else std::snprintf(sub, sizeof sub, "Running");
        note = "The Optic desktop is on the HDMI screen.";
    } else if (h.connected) {
        big = "Off";
        std::snprintf(sub, sizeof sub, "A screen is plugged in");
        note = "OK starts the Optic desktop on the HDMI screen.";
    } else {
        big = "No screen";
        std::snprintf(sub, sizeof sub, "Nothing is plugged into HDMI");
        note = "Plug a screen in, then press OK.";
    }
    lv_label_set_text(hdmi_big_, big);
    lv_label_set_text(hdmi_sub_, sub);
    lv_label_set_text(hdmi_note_, note);
    const bool on = hdmi_busy_ > 0 ? hdmi_want_ : h.running;
    lv_label_set_text(hdmi_val_, on ? "< On >" : "< Off >");
    lv_obj_set_x(hdmi_val_, 316 - 8 - text_width(hdmi_val_));
}

void VelvetUI::hdmi_toggle() {
    if (!hdmi_.supported || hdmi_busy_ > 0) return;
    hdmi_want_ = !hdmi_.running;
    device_->set_hdmi(hdmi_want_);
    sound_->play(hdmi_want_ ? Sfx::ToggleOn : Sfx::ToggleOff);
    hdmi_busy_ = hdmi_want_ ? 20 : 6;   // the desktop takes a while to come up on a CardputerZero
    hdmi_hub_age_ = 0;
    hdmi_render();
}

void VelvetUI::hdmi_tick_cb(lv_timer_t* t) {
    auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
    // every second while a start / stop is awaited, else every other second
    static int beat = 0;
    if (self->hdmi_busy_ == 0 && (++beat & 1)) return;
    self->hdmi_ = self->device_->hdmi();
    if (self->hdmi_busy_ > 0) {
        // a start is done once the display answers with its mode; a stop once it is gone
        const bool done = self->hdmi_want_ ? (self->hdmi_.running && !self->hdmi_.mode.empty()) : !self->hdmi_.running;
        self->hdmi_busy_ = done ? 0 : self->hdmi_busy_ - 1;
    }
    self->hdmi_render();
}

void VelvetUI::system_render() {
    if (!sys_batt_) return;
    char buf[64];
    const PowerInfo pw = device_->power();
    if (pw.valid) {
        std::snprintf(buf, sizeof buf, "%d%%", pw.percent);
        lv_label_set_text(sys_batt_, buf);
        lv_obj_set_width(sys_batt_bar_, 6 + 94 * std::clamp(pw.percent, 0, 100) / 100);
        std::snprintf(buf, sizeof buf, "%s\n%d.%02d V", pw.charging ? "Charging" : "Battery", pw.millivolts / 1000,
                      (pw.millivolts % 1000) / 10);
        lv_label_set_text(sys_batt_info_, buf);
    } else {
        lv_label_set_text(sys_batt_, "--");
        lv_obj_set_width(sys_batt_bar_, 6);
        lv_label_set_text(sys_batt_info_, "No battery info");
    }
    const int values[2] = {device_->brightness(), device_->volume()};
    for (int i : sys_vis_) {
        const bool sel = i == sys_sel_;
        lv_obj_set_style_text_color(sys_rows_[i], C(sel ? kInk : kWhite), 0);
        if (sel) lv_obj_remove_flag(sys_markers_[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(sys_markers_[i], LV_OBJ_FLAG_HIDDEN);
        bool on = false;  // a value that's "on" reads white, else grey
        if (i == SysR36s) {  // this screen on an R36S running Optic Link
            const int st = slink_->state();
            if (st == ScreenLink::Unavailable) std::snprintf(buf, sizeof buf, "Not here");
            else if (st == ScreenLink::Waiting) std::snprintf(buf, sizeof buf, "Waiting");
            else if (st == ScreenLink::Pairing) std::snprintf(buf, sizeof buf, "Code %s", slink_->code().c_str());
            else if (st == ScreenLink::Live) std::snprintf(buf, sizeof buf, "On %.10s", slink_->peer().c_str());
            else if (!slink_->peer().empty()) std::snprintf(buf, sizeof buf, "%.14s", slink_->peer().c_str());  // why it's off
            else std::snprintf(buf, sizeof buf, "Off");
            on = st == ScreenLink::Pairing || st == ScreenLink::Live;
        } else if (i == SysSound) {
            std::snprintf(buf, sizeof buf, "%s", !sound_->available() ? "No device" : sound_->enabled() ? "On" : "Off");
            on = sound_->available() && sound_->enabled();
        } else if (i == SysPower) {  // Normal, or ECO (slower redraw, dims and sleeps the screen when idle)
            std::snprintf(buf, sizeof buf, "%s", eco_ ? "ECO" : "Normal");
            on = eco_;
        } else if (i == SysTheme) {  // left / right / OK: Dark or White
            std::snprintf(buf, sizeof buf, "%s", theme_ == 1 ? "White" : "Dark");
            on = true;
        } else if (i == SysBoot) {
            std::snprintf(buf, sizeof buf, "%s", "Optic");
            on = true;
        } else if (i == SysTutorial) {  // OK opens it, left/right: show at start or not
            std::snprintf(buf, sizeof buf, "%s", tutorial_at_start_ ? "On" : "Off");
            on = tutorial_at_start_;
        } else {
            const int v = values[i];
            lv_obj_set_width(sys_fills_[i], v < 0 ? 6 : 6 + 46 * v / 100);
            pill_set_color(sys_tracks_[i], sel ? kLight : kDark);
            pill_set_color(sys_fills_[i], v < 0 ? (sel ? kLight : kDark) : sel ? kInk : kWhite);
            if (v < 0) std::snprintf(buf, sizeof buf, "--");
            else std::snprintf(buf, sizeof buf, "%d", v);
            lv_label_set_text(sys_values_[i], buf);
            lv_obj_set_style_text_color(sys_values_[i], C(sel ? kInk : kGrey), 0);
            lv_obj_set_x(sys_values_[i], kSysX + kSysW - 8 - text_width(sys_values_[i]));
            continue;
        }
        // selected: "< value >" (left / right change it)
        std::string shown = sel ? std::string("< ") + buf + " >" : std::string(buf);
        lv_label_set_text(sys_values_[i], shown.c_str());
        lv_obj_set_style_text_color(sys_values_[i], C(sel ? kInk : on ? kWhite : kGrey), 0);
        lv_obj_set_width(sys_values_[i], LV_SIZE_CONTENT);
        int32_t w = text_width(sys_values_[i]);
        if (w > 104) {
            lv_obj_set_width(sys_values_[i], 104);
            w = 104;
        }
        lv_obj_set_x(sys_values_[i], kSysX + kSysW - 8 - w);
    }
    std::snprintf(buf, sizeof buf, "%s", net_.connected && !net_.ip.empty() ? net_.ip.c_str() : "No IP");
    lv_label_set_text(sys_ip_, buf);
}

// the row above / below among the ones shown
void VelvetUI::sys_step(int d) {
    if (sys_vis_.empty()) return;
    const auto at = std::find(sys_vis_.begin(), sys_vis_.end(), sys_sel_);
    const int n = static_cast<int>(sys_vis_.size());
    const int i = at == sys_vis_.end() ? 0 : static_cast<int>(at - sys_vis_.begin());
    sys_sel_ = sys_vis_[(i + d + n) % n];
}

void VelvetUI::system_adjust(int delta) {
    if (sys_sel_ == SysR36s) {
        if (slink_->state() == ScreenLink::Unavailable) return;
        slink_->set_on(!slink_->on());
        sound_->play(slink_->on() ? Sfx::ToggleOn : Sfx::ToggleOff);
        system_render();
        return;
    }
    if (sys_sel_ == SysPower) {
        eco_set(!eco_);
        save_setting("eco", eco_ ? 1 : 0);
        sound_->play(eco_ ? Sfx::ToggleOn : Sfx::ToggleOff);
        system_render();
        return;
    }
    if (sys_sel_ == SysTheme) {
        // the whole screen again in the other colours, back on this row
        theme_ = 1 - theme_;
        save_setting("ui_theme", theme_);
        apply_theme(theme_);
        clear_page_timers();
        page_ = nullptr;          // (goes with the old screen)
        build_ui();
        lv_obj_add_flag(hub_, LV_OBJ_FLAG_HIDDEN);
        build_page(Page::System);
        sys_sel_ = SysTheme;
        system_render();
        sound_->play(Sfx::ToggleOn);
        return;
    }
    if (sys_sel_ == SysBoot) {
        if (delta == 0) splash_start();  // preview
        return;
    }
    if (sys_sel_ == SysTutorial) {
        if (delta == 0) {
            tutorial_open();
            return;
        }
        tutorial_at_start_ = !tutorial_at_start_;
        save_setting("tutorial", tutorial_at_start_ ? 1 : 0);
        sound_->play(tutorial_at_start_ ? Sfx::ToggleOn : Sfx::ToggleOff);
        system_render();
        return;
    }
    if (sys_sel_ == SysSound) {
        sound_->set_enabled(!sound_->enabled());
        save_setting("sound", sound_->enabled() ? 1 : 0);
        sound_->play(Sfx::ToggleOn);  // only heard when it just turned on
        system_render();
        return;
    }
    if (delta == 0) return;  // OK on a slider does nothing
    if (sys_sel_ == SysBright) {
        const int v = device_->brightness();
        if (v >= 0) device_->set_brightness(std::max(10, std::min(100, v + delta)));
    } else {
        const int v = device_->volume();
        if (v >= 0) device_->set_volume(v + delta);
        // the demo has no real mixer: let the VOLUME row drive the effects instead
        if (device_->is_demo() && device_->volume() >= 0) sound_->set_gain(device_->volume() / 100.0f);
    }
    sound_->play(Sfx::Move);
    system_render();
}

// ---- SYSTEM's TOR + R36S, running whatever page is open --------------------------------------

void VelvetUI::link_tick_cb(lv_timer_t* t) { static_cast<VelvetUI*>(lv_timer_get_user_data(t))->link_tick(); }

// R36S buttons (olink.h OLB_*) as Velvet keys: D-pad / left stick = arrows, A = OK, B = Esc,
// X = Tab, Y = Backspace, L1 / R1 = Page Up / Down, L2 / R2 = - / + (zoom), START = Enter,
// SELECT = fn (the orange layer).
void VelvetUI::link_button(int b, bool down) {
    static const uint32_t kMap[16] = {
        LV_KEY_ENTER, LV_KEY_ESC, LV_KEY_NEXT /* Tab */, LV_KEY_BACKSPACE, kKeyPgUp, kKeyPgDn, '-', '+',
        0 /* SELECT */, LV_KEY_ENTER, LV_KEY_UP, LV_KEY_DOWN, LV_KEY_LEFT, LV_KEY_RIGHT, 0, 0};
    if (b < 0 || b >= 16 || link_btn_[b] == down) return;
    link_btn_[b] = down;
    if (b == 8) { if (down) set_key_layer(1); return; }
    const uint32_t k = kMap[b];
    if (!k) return;
    if (down) handle_key(k, false);
    else handle_key_release(k);
}

// the screen as RGB565: the framebuffer itself on the CardputerZero (overlays included),
// else LVGL draws it again (desktop build)
bool VelvetUI::link_capture() {
    link_px_.resize(320 * 170);
#if !defined(__EMSCRIPTEN__)
    static int fb = -2;
    static int fb_bpp = 0, fb_line = 0;
    if (fb == -2) {
        fb = open(lcd_fb_path().c_str(), O_RDONLY);
        fb_var_screeninfo v{};
        fb_fix_screeninfo f{};
        if (fb >= 0 && (ioctl(fb, FBIOGET_VSCREENINFO, &v) || ioctl(fb, FBIOGET_FSCREENINFO, &f) || v.xres != 320 ||
                        v.yres != 170 || (v.bits_per_pixel != 16 && v.bits_per_pixel != 32))) {
            close(fb);
            fb = -1;
        }
        if (fb >= 0) { fb_bpp = (int)v.bits_per_pixel; fb_line = (int)f.line_length; }
    }
    if (fb >= 0) {
        std::vector<uint8_t> row(fb_line);
        for (int y = 0; y < 170; ++y) {
            if (pread(fb, row.data(), fb_line, (off_t)y * fb_line) != fb_line) return false;
            uint16_t* d = link_px_.data() + y * 320;
            if (fb_bpp == 16) std::memcpy(d, row.data(), 640);
            else for (int x = 0; x < 320; ++x) {
                const uint8_t* p = row.data() + x * 4;   // B G R X
                d[x] = (uint16_t)(((p[2] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[0] >> 3));
            }
        }
        return true;
    }
#endif
    lv_draw_buf_t* shot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!shot) return false;
    const int w = std::min<int>(shot->header.w, 320), h = std::min<int>(shot->header.h, 170);
    for (int y = 0; y < h; ++y) std::memcpy(link_px_.data() + y * 320, shot->data + (size_t)y * shot->header.stride, (size_t)w * 2);
    lv_draw_buf_destroy(shot);
    return true;
}

void VelvetUI::link_tick() {
    // TOR changed: a running FULL browser restarts so it uses (or stops using) the proxy
    const std::string proxy = tor_proxy();
    if (proxy != link_proxy_) {
        link_proxy_ = proxy;
        if (chrome_ && chrome_->running() && chrome_->proxy() != proxy) {
            chrome_->stop();
            if (bfull_ && page_kind_ == Page::Browser) browser_set_engine(true);
        }
    }
    if (!slink_ || !slink_->on()) return;
    ScreenLink::Event e;
    while (slink_->next_event(e)) {
        if (e.type == 10) link_button(e.code, e.value != 0);                      // OL_BTN
        else if (e.type == 11 && e.code >= 0 && e.code < 4) link_axis_[e.code] = e.value;   // OL_AXIS
    }
    // the left stick: an arrow held while it's pushed, repeating like a held key
    const int ax = link_axis_[0], ay = link_axis_[1];
    uint32_t want = 0;
    if (std::abs(ax) > 20000 || std::abs(ay) > 20000)
        want = std::abs(ax) > std::abs(ay) ? (ax > 0 ? LV_KEY_RIGHT : LV_KEY_LEFT) : (ay > 0 ? LV_KEY_DOWN : LV_KEY_UP);
    const uint32_t now = lv_tick_get();
    if (want != link_stick_key_) {
        if (link_stick_key_) handle_key_release(link_stick_key_);
        link_stick_key_ = want;
        if (want) { handle_key(want, false); link_stick_next_ = now + 380; }
    } else if (want && now >= link_stick_next_) {
        handle_key(want, false);
        link_stick_next_ = now + 90;
    }
    if (slink_->wants_frame() && link_capture()) slink_->frame(link_px_.data(), 320, 170);
}

void VelvetUI::system_tick_cb(lv_timer_t* t) {
    auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
    if (!self->fps_label_) return;
    char buf[48];
    const uint32_t up = lv_tick_elaps(self->started_ms_) / 1000;
    std::snprintf(buf, sizeof buf, "%u FPS  %02u:%02u", self->frames_, up / 60, up % 60);
    lv_label_set_text(self->fps_label_, buf);
    self->frames_ = 0;
    self->system_render();
}

void VelvetUI::render_ready_cb(lv_event_t* e) {
    auto* self = static_cast<VelvetUI*>(lv_event_get_user_data(e));
    self->frames_++;
}

// ---------------------------------------------------------------- wifi
// List of networks from NetworkManager. ENTER on a network: new secured network ->
// password entry; open network -> join; saved/connected network -> action menu.
void VelvetUI::build_wifi(lv_obj_t* p) {
    build_page_chrome(p, "Wi-Fi");
    if (!device_->real_wifi()) label(p, "Demo", f_.mono10, kFaint, 76, 7);

    // status: a dark pill with a dot, the network + IP, and SCAN while scanning
    pill(p, 4, 29, 312, 20, kDark);
    wifi_status_dot_ = pill(p, 12, 35, 8, 8, kFaint);
    wifi_status_ = label(p, "", f_.mono10, kWhite, 26, 32);
    lv_label_set_long_mode(wifi_status_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(wifi_status_, 232);
    wifi_scan_label_ = label(p, "Scan", f_.mono10, kGrey, 276, 32);

    // network rows: plain text, the selected one in a white pill
    wifi_rows_.clear();
    for (int i = 0; i < kWifiRows; ++i) {
        const int32_t y = 53 + i * 24;
        WifiRow r;
        r.shadow = nullptr;
        r.plate = pill(p, 4, y, 312, 22, kWhite);
        lv_obj_set_style_bg_opa(r.plate, LV_OPA_TRANSP, 0);
        r.name = label(r.plate, "", f_.mono12, kWhite, 12, 3);
        lv_label_set_long_mode(r.name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(r.name, 196);
        r.tag = label(r.plate, "", f_.mono10, kGrey, 214, 4);
        r.lock = plain(r.plate);
        lv_obj_set_pos(r.lock, 268, 6);
        lv_obj_set_size(r.lock, 7, 9);
        lv_obj_add_event_cb(r.lock, lock_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
        r.bars = plain(r.plate);
        lv_obj_set_pos(r.bars, 282, 4);
        lv_obj_set_size(r.bars, 14, 13);
        lv_obj_add_event_cb(r.bars, bars_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
        wifi_rows_.push_back(r);
    }
    wifi_msg_ = label(p, "", f_.mono12, kGrey, 16, 84);
    wifi_count_ = label(p, "", f_.mono10, kFaint, 8, 152);

    wifi_hint_ = plain(p);
    lv_obj_set_pos(wifi_hint_, 0, 151);
    lv_obj_set_size(wifi_hint_, 316, 16);
    wifi_set_hints({{"OK", "Join"}, {"R", "Scan"}, {"W", "Radio"}});

    wifi_mode_ = WifiMode::List;
    wifi_overlay_ = nullptr;
    wifi_sel_ = 0;
    wifi_top_ = 0;
    wifi_render_status();
    wifi_render_list();
    wifi_timer_ = lv_timer_create(wifi_tick_cb, 250, this);
    wifi_refresh_status();
    wifi_scan();
}

void VelvetUI::wifi_set_hints(std::initializer_list<std::pair<const char*, const char*>> items) {
    if (!wifi_hint_) return;
    lv_obj_clean(wifi_hint_);
    hints_r(wifi_hint_, 316, 0, f_.mono10, items);
}

void VelvetUI::wifi_refresh_status() {
    const uint32_t gen = wifi_gen_;
    device_->net_status([this, gen](NetStatus st) {
        net_ = st;
        update_hub_status();
        if (gen == wifi_gen_) wifi_render_status();
    });
}

void VelvetUI::wifi_scan() {
    if (wifi_scanning_) return;
    wifi_scanning_ = true;
    const uint32_t gen = wifi_gen_;
    wifi_render_list();
    device_->wifi_scan([this, gen](std::vector<WifiAp> aps) {
        wifi_scanning_ = false;
        if (gen != wifi_gen_) return;
        // keep the cursor on the same network if it is still there
        const std::string keep =
            wifi_sel_ < static_cast<int>(wifi_aps_.size()) ? wifi_aps_[wifi_sel_].ssid : std::string();
        wifi_aps_ = std::move(aps);
        wifi_sel_ = 0;
        for (size_t i = 0; i < wifi_aps_.size(); ++i) {
            if (wifi_aps_[i].ssid == keep) wifi_sel_ = static_cast<int>(i);
        }
        wifi_render_list();
    });
}

void VelvetUI::wifi_render_status() {
    if (!wifi_status_) return;
    std::string text;
    uint32_t dot = kFaint;
    if (!net_.radio_on) {
        text = "Wi-Fi is off";
    } else if (net_.connected) {
        text = net_.ssid + (net_.ip.empty() ? "" : "  " + net_.ip);
        dot = kWhite;
    } else {
        text = "Not connected";
    }
    lv_label_set_text(wifi_status_, text.c_str());
    lv_obj_set_style_bg_color(wifi_status_dot_, C(dot), 0);
}

void VelvetUI::wifi_render_list() {
    if (wifi_rows_.empty()) return;
    const int n = static_cast<int>(wifi_aps_.size());
    const char* msg = nullptr;
    if (!net_.radio_on) msg = "Wi-Fi is off.\nPress W to turn it on.";
    else if (n == 0) msg = wifi_scanning_ ? "Looking for networks..." : "No networks found.\nPress R to scan again.";
    lv_label_set_text(wifi_msg_, msg ? msg : "");

    if (n) wifi_sel_ = std::max(0, std::min(wifi_sel_, n - 1));
    if (wifi_sel_ < wifi_top_) wifi_top_ = wifi_sel_;
    if (wifi_sel_ >= wifi_top_ + kWifiRows) wifi_top_ = wifi_sel_ - kWifiRows + 1;
    wifi_top_ = std::max(0, std::min(wifi_top_, std::max(0, n - kWifiRows)));

    for (int i = 0; i < kWifiRows; ++i) {
        WifiRow& r = wifi_rows_[i];
        const int idx = wifi_top_ + i;
        const bool show = !msg && idx < n;
        if (show) lv_obj_remove_flag(r.plate, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(r.plate, LV_OBJ_FLAG_HIDDEN);
        if (!show) continue;
        const WifiAp& ap = wifi_aps_[idx];
        const bool sel = idx == wifi_sel_;
        lv_obj_set_style_bg_opa(r.plate, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_label_set_text(r.name, ap.ssid.c_str());
        lv_obj_set_style_text_color(r.name, C(sel ? kInk : kWhite), 0);
        lv_label_set_text(r.tag, ap.active ? "On" : ap.saved ? "Saved" : ap.enterprise ? "Login" : "");
        lv_obj_set_style_text_color(r.tag, C(sel ? kInk : ap.active ? kWhite : kGrey), 0);
        lv_obj_set_x(r.tag, 262 - utf8_cols(lv_label_get_text(r.tag)) * 8);
        if (ap.secure) lv_obj_remove_flag(r.lock, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(r.lock, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(r.lock, C(sel ? kInk : kGrey), 0);
        lv_obj_set_user_data(r.bars, reinterpret_cast<void*>(static_cast<intptr_t>(signal_level(ap.signal))));
        lv_obj_set_style_bg_color(r.bars, C(sel ? kInk : kWhite), 0);
        lv_obj_invalidate(r.plate);
    }
    char buf[16] = "";
    if (n && !msg) std::snprintf(buf, sizeof buf, "%d/%d", wifi_sel_ + 1, n);
    lv_label_set_text(wifi_count_, buf);
}

void VelvetUI::wifi_close_overlay() {
    sound_->play(Sfx::Back);
    if (wifi_overlay_) lv_obj_delete(wifi_overlay_);
    wifi_overlay_ = nullptr;
    wifi_action_plates_.clear();
    wifi_action_labels_.clear();
    wifi_pw_label_ = nullptr;
    wifi_pw_info_ = nullptr;
    wifi_eap_info_ = nullptr;
    for (int i = 0; i < kEapRows; ++i) wifi_eap_plates_[i] = wifi_eap_names_[i] = wifi_eap_values_[i] = nullptr;
    wifi_result_until_ = 0;
    wifi_mode_ = WifiMode::List;
    wifi_set_hints({{"OK", "Join"}, {"R", "Scan"}, {"W", "Radio"}});
}

namespace {
// Overlays: the page dimmed once (drawn when it opens, not every frame), a dark card on top.
lv_obj_t* dim_layer(lv_obj_t* parent) {
    lv_obj_t* o = plain(parent);
    lv_obj_set_pos(o, 0, 28);
    lv_obj_set_size(o, 320, 122);
    lv_obj_set_style_bg_color(o, C(kBg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_80, 0);
    return o;
}
}  // namespace

void VelvetUI::wifi_open_actions() {
    const WifiAp& ap = wifi_aps_[wifi_sel_];
    wifi_target_ = ap.ssid;
    wifi_actions_.clear();
    wifi_actions_.push_back(ap.active ? "Disconnect" : "Connect");
    wifi_actions_.push_back("Forget");
    wifi_actions_.push_back("Cancel");
    wifi_action_sel_ = 0;

    wifi_close_overlay();
    wifi_mode_ = WifiMode::Actions;
    wifi_overlay_ = dim_layer(page_);
    lv_obj_t* box = pill(wifi_overlay_, 70, 2, 180, 112, kDark, 12);
    lv_obj_t* name = label(box, ap.ssid.c_str(), f_.mono12, kWhite, 12, 7);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 156);
    for (size_t i = 0; i < wifi_actions_.size(); ++i) {
        const int32_t y = 30 + static_cast<int32_t>(i) * 26;
        lv_obj_t* plate = pill(box, 6, y, 168, 22, kWhite);
        lv_obj_t* l = label(plate, wifi_actions_[i].c_str(), f_.mono12, kWhite, 12, 3);
        wifi_action_plates_.push_back(plate);
        wifi_action_labels_.push_back(l);
    }
    wifi_set_hints({{"OK", "Pick"}, {"ESC", "Back"}});
    wifi_render_actions();
}

void VelvetUI::wifi_render_actions() {
    for (size_t i = 0; i < wifi_action_plates_.size(); ++i) {
        const bool sel = static_cast<int>(i) == wifi_action_sel_;
        lv_obj_set_style_bg_opa(wifi_action_plates_[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(wifi_action_labels_[i], C(sel ? kInk : kWhite), 0);
    }
}

void VelvetUI::wifi_run_action() {
    const std::string act = wifi_actions_[wifi_action_sel_];
    const uint32_t gen = wifi_gen_;
    auto done = [this, gen](bool ok, const std::string& msg) {
        if (gen == wifi_gen_) wifi_result(ok, msg);
    };
    if (act == "Cancel") {
        wifi_close_overlay();
    } else if (act == "Connect") {
        wifi_connect("");
    } else if (act == "Disconnect") {
        wifi_busy("Disconnecting");
        device_->wifi_disconnect(done);
    } else if (act == "Forget") {
        wifi_busy("Forgetting");
        device_->wifi_forget(wifi_target_, done);
    }
}

void VelvetUI::wifi_open_password() {
    wifi_target_ = wifi_aps_[wifi_sel_].ssid;
    wifi_password_.clear();
    wifi_show_pw_ = false;

    wifi_close_overlay();
    wifi_mode_ = WifiMode::Password;
    wifi_overlay_ = dim_layer(page_);
    lv_obj_t* box = pill(wifi_overlay_, 4, 2, 312, 112, kDark, 12);
    label(box, "Password for", f_.mono10, kGrey, 12, 8);
    lv_obj_t* name = label(box, wifi_target_.c_str(), f_.mono12, kWhite, 12, 24);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 288);
    lv_obj_t* field = pill(box, 6, 46, 300, 24, kBg);
    wifi_pw_label_ = label(field, "", f_.mono12, kWhite, 12, 4);
    wifi_pw_info_ = label(box, "", f_.mono10, kGrey, 12, 82);
    wifi_set_hints({{"OK", "Join"}, {"TAB", "Show"}, {"ESC", "Cancel"}});
    wifi_render_password();
}

void VelvetUI::wifi_render_password() {
    if (!wifi_pw_label_) return;
    // show the end of long passwords: 32 columns fit in the field
    std::string shown = wifi_show_pw_ ? wifi_password_ : std::string(wifi_password_.size(), '*');
    if (shown.size() > 31) shown = shown.substr(shown.size() - 31);
    const bool cursor_on = (lv_tick_get() / 400) % 2 == 0;
    shown += cursor_on ? "_" : " ";
    lv_label_set_text(wifi_pw_label_, shown.c_str());
}

// WPA-Enterprise login: four rows on the same card as the password entry. Up / down pick a row,
// typing edits it, OK goes on to the next row and joins from the last one (the method).
void VelvetUI::wifi_open_eap() {
    wifi_target_ = wifi_aps_[wifi_sel_].ssid;
    wifi_eap_ = WifiEap{};
    wifi_eap_sel_ = 0;
    wifi_show_pw_ = false;

    wifi_close_overlay();
    wifi_mode_ = WifiMode::Eap;
    wifi_overlay_ = dim_layer(page_);
    lv_obj_t* box = pill(wifi_overlay_, 4, 2, 312, 112, kDark, 12);
    lv_obj_t* name = label(box, wifi_target_.c_str(), f_.mono10, kGrey, 12, 5);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 136);
    wifi_eap_info_ = label(box, "", f_.mono10, kGrey, 152, 5);
    lv_obj_set_width(wifi_eap_info_, 148);
    lv_obj_set_style_text_align(wifi_eap_info_, LV_TEXT_ALIGN_RIGHT, 0);
    static const char* const kNames[kEapRows] = {"Identity", "Anonymous", "Password", "Method"};
    for (int i = 0; i < kEapRows; ++i) {
        lv_obj_t* plate = pill(box, 6, 21 + i * 22, 300, 20, kWhite);
        wifi_eap_plates_[i] = plate;
        wifi_eap_names_[i] = label(plate, kNames[i], f_.mono10, kGrey, 8, 3);
        wifi_eap_values_[i] = label(plate, "", f_.mono12, kWhite, 86, 2);
    }
    wifi_eap_hints();
    wifi_render_eap();
}

void VelvetUI::wifi_eap_hints() {
    if (wifi_eap_sel_ == kEapRows - 1) wifi_set_hints({{"OK", "Join"}, {"\xE2\x86\x90\xE2\x86\x92", "Method"}, {"ESC", "Cancel"}});
    else if (wifi_eap_sel_ == 2) wifi_set_hints({{"OK", "Next"}, {"TAB", "Show"}, {"ESC", "Cancel"}});
    else wifi_set_hints({{"OK", "Next"}, {"\xE2\x86\x91\xE2\x86\x93", "Field"}, {"ESC", "Cancel"}});
}

void VelvetUI::wifi_render_eap() {
    if (!wifi_eap_plates_[0]) return;
    const bool cursor_on = (lv_tick_get() / 400) % 2 == 0;
    for (int i = 0; i < kEapRows; ++i) {
        const bool sel = i == wifi_eap_sel_;
        std::string shown;
        bool faint = false;
        if (i == 3) {
            shown = wifi_eap_.method == WifiEap::Method::TtlsPap ? "TTLS / PAP" : "PEAP / MSCHAPv2";
        } else {
            const std::string& v = i == 0 ? wifi_eap_.identity : i == 1 ? wifi_eap_.anonymous : wifi_eap_.password;
            shown = i == 2 && !wifi_show_pw_ ? std::string(v.size(), '*') : v;
            // show the end of long values: 25 columns and the cursor fit in the row
            if (shown.size() > 25) shown = shown.substr(shown.size() - 25);
            if (sel) {
                shown += cursor_on ? "_" : " ";
            } else if (v.empty()) {
                shown = i == 1 ? "none" : "";
                faint = true;
            }
        }
        lv_obj_set_style_bg_opa(wifi_eap_plates_[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(wifi_eap_names_[i], C(sel ? kInk : kGrey), 0);
        lv_obj_set_style_text_color(wifi_eap_values_[i], C(sel ? kInk : faint ? kFaint : kWhite), 0);
        lv_label_set_text(wifi_eap_values_[i], shown.c_str());
    }
}

void VelvetUI::wifi_connect(const std::string& password) {
    const uint32_t gen = wifi_gen_;
    wifi_busy("Connecting");
    device_->wifi_connect(wifi_target_, password, [this, gen](bool ok, const std::string& msg) {
        if (gen == wifi_gen_) wifi_result(ok, msg);
    });
}

void VelvetUI::wifi_busy(const char* text) {
    wifi_close_overlay();
    wifi_mode_ = WifiMode::Busy;
    wifi_overlay_ = dim_layer(page_);
    lv_obj_t* box = pill(wifi_overlay_, 30, 20, 260, 76, kDark, 12);
    label(box, text, f_.big, kWhite, 14, 8);
    busy_bar(box, 14, 34, 232, 6, kBg);
    if (!wifi_target_.empty()) {
        lv_obj_t* who = label(box, wifi_target_.c_str(), f_.mono10, kGrey, 14, 50);
        lv_label_set_long_mode(who, LV_LABEL_LONG_DOT);
        lv_obj_set_width(who, 232);
    }
    wifi_set_hints({{"", "Please wait"}});
}

void VelvetUI::wifi_result(bool ok, const std::string& message) {
    sound_->play(ok ? Sfx::Success : Sfx::Error);
    wifi_close_overlay();
    wifi_mode_ = WifiMode::Busy;
    wifi_overlay_ = dim_layer(page_);
    lv_obj_t* box = pill(wifi_overlay_, 30, 24, 260, 68, ok ? kWhite : kDark, 12);
    label(box, ok ? "Done" : "Failed", f_.big, ok ? kInk : kWhite, 14, 8);
    lv_obj_t* msg = label(box, message.c_str(), f_.mono10, ok ? kInk : kGrey, 14, 34);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, 232);
    wifi_set_hints({{"ANY", "Close"}});
    wifi_result_until_ = lv_tick_get() + (ok ? 1600 : 3200);
    // refresh what NetworkManager now says
    wifi_refresh_status();
    wifi_scan();
}

void VelvetUI::wifi_tick_cb(lv_timer_t* t) {
    auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
    if (self->wifi_scan_label_) {
        const bool blink = self->wifi_scanning_ && (lv_tick_get() / 250) % 2 == 0;
        if (blink) lv_obj_remove_flag(self->wifi_scan_label_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(self->wifi_scan_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (self->wifi_mode_ == WifiMode::Password) self->wifi_render_password();
    if (self->wifi_mode_ == WifiMode::Eap) self->wifi_render_eap();
    if (self->wifi_result_until_ && static_cast<int32_t>(lv_tick_get() - self->wifi_result_until_) >= 0) {
        self->wifi_close_overlay();
        self->wifi_render_list();
    }
}

bool VelvetUI::wifi_key(uint32_t key, bool long_pressed) {
    switch (wifi_mode_) {
        case WifiMode::Busy:
            // nmcli can't be cancelled half way; a shown result closes on any key
            if (wifi_result_until_ && !long_pressed) {
                wifi_close_overlay();
                wifi_render_list();
            }
            return true;

        case WifiMode::Password:
            if (long_pressed) {
                if (key == LV_KEY_BACKSPACE) wifi_password_.clear();
                wifi_render_password();
                return true;
            }
            if (key == LV_KEY_ESC) {
                wifi_close_overlay();
            } else if (key == LV_KEY_BACKSPACE) {
                if (!wifi_password_.empty()) wifi_password_.pop_back();
                sound_->play(Sfx::Key);
            } else if (key == LV_KEY_NEXT || key == LV_KEY_UP || key == LV_KEY_DOWN) {
                wifi_show_pw_ = !wifi_show_pw_;
            } else if (key == LV_KEY_ENTER) {
                if (wifi_password_.size() < 8) {
                    lv_label_set_text(wifi_pw_info_, "Needs 8+ characters");
                    lv_obj_set_style_text_color(wifi_pw_info_, C(kWhite), 0);
                    return true;
                }
                wifi_connect(wifi_password_);
                return true;
            } else if (key >= 32 && key <= 126 && wifi_password_.size() < kMaxPassword) {
                wifi_password_ += static_cast<char>(key);
                sound_->play(Sfx::Key);
            }
            if (wifi_pw_info_) {
                char buf[48];
                std::snprintf(buf, sizeof buf, "%zu characters%s", wifi_password_.size(),
                              wifi_show_pw_ ? ", shown" : "");
                lv_label_set_text(wifi_pw_info_, buf);
                lv_obj_set_style_text_color(wifi_pw_info_, C(kGrey), 0);
            }
            wifi_render_password();
            return true;

        case WifiMode::Eap: {
            const bool method_row = wifi_eap_sel_ == kEapRows - 1;
            std::string* field = wifi_eap_sel_ == 0 ? &wifi_eap_.identity
                               : wifi_eap_sel_ == 1 ? &wifi_eap_.anonymous
                               : wifi_eap_sel_ == 2 ? &wifi_eap_.password : nullptr;
            auto flip_method = [this] {
                wifi_eap_.method = wifi_eap_.method == WifiEap::Method::PeapMschapv2 ? WifiEap::Method::TtlsPap
                                                                                     : WifiEap::Method::PeapMschapv2;
                sound_->play(Sfx::Move);
            };
            auto go = [this](int row) {
                wifi_eap_sel_ = (row + kEapRows) % kEapRows;
                sound_->play(Sfx::Move);
                wifi_eap_hints();
            };
            if (long_pressed) {
                if (key == LV_KEY_BACKSPACE && field) field->clear();
                wifi_render_eap();
                return true;
            }
            if (wifi_eap_info_) lv_label_set_text(wifi_eap_info_, "");
            if (key == LV_KEY_ESC) {
                wifi_eap_ = WifiEap{};   // the login is not kept once the form is gone
                wifi_close_overlay();
                return true;
            } else if (key == LV_KEY_UP) {
                go(wifi_eap_sel_ - 1);
            } else if (key == LV_KEY_DOWN) {
                go(wifi_eap_sel_ + 1);
            } else if (key == LV_KEY_NEXT) {
                wifi_show_pw_ = !wifi_show_pw_;
                if (wifi_eap_info_) lv_label_set_text(wifi_eap_info_, wifi_show_pw_ ? "Password shown" : "Password hidden");
            } else if (method_row && (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || is_left(key) || is_right(key))) {
                flip_method();
            } else if (key == LV_KEY_ENTER && !method_row) {
                go(wifi_eap_sel_ + 1);
            } else if (key == LV_KEY_ENTER) {
                const int missing = wifi_eap_.identity.empty() ? 0 : wifi_eap_.password.empty() ? 2 : -1;
                if (missing >= 0) {
                    sound_->play(Sfx::Error);
                    wifi_eap_sel_ = missing;
                    wifi_eap_hints();
                    if (wifi_eap_info_) lv_label_set_text(wifi_eap_info_, missing == 0 ? "Needs an identity" : "Needs a password");
                    wifi_render_eap();
                    return true;
                }
                const WifiEap login = std::move(wifi_eap_);
                wifi_eap_ = WifiEap{};   // don't keep the password around after handing it over
                const uint32_t gen = wifi_gen_;
                wifi_busy("Connecting");
                device_->wifi_connect_eap(wifi_target_, login, [this, gen](bool ok, const std::string& msg) {
                    if (gen == wifi_gen_) wifi_result(ok, msg);
                });
                return true;
            } else if (field && key >= 32 && key <= 126 && field->size() < kMaxEapField) {
                *field += static_cast<char>(key);
                sound_->play(Sfx::Key);
            } else if (field && key == LV_KEY_BACKSPACE) {
                if (!field->empty()) field->pop_back();
                sound_->play(Sfx::Key);
            }
            wifi_render_eap();
            return true;
        }

        case WifiMode::Actions:
            if (long_pressed) return true;
            if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                wifi_close_overlay();
            } else if (is_up(key)) {
                wifi_action_sel_ = (wifi_action_sel_ + 2) % 3;
                sound_->play(Sfx::Move);
                wifi_render_actions();
            } else if (is_down(key)) {
                wifi_action_sel_ = (wifi_action_sel_ + 1) % 3;
                sound_->play(Sfx::Move);
                wifi_render_actions();
            } else if (is_enter(key)) {
                wifi_run_action();
            }
            return true;

        case WifiMode::List:
            if (long_pressed) return false;
            if (key == 'r' || key == 'R') {
                wifi_scan();
            } else if (key == 'w' || key == 'W') {
                const bool on = !net_.radio_on;
                const uint32_t gen = wifi_gen_;
                wifi_target_ = "";
                wifi_busy(on ? "Turning on" : "Turning off");
                device_->wifi_set_radio(on, [this, gen, on](bool ok, const std::string& msg) {
                    if (gen != wifi_gen_) return;
                    if (ok) net_.radio_on = on;
                    if (!on) wifi_aps_.clear();
                    wifi_result(ok, msg);
                });
            } else if (is_up(key) && !wifi_aps_.empty()) {
                wifi_sel_ = (wifi_sel_ + static_cast<int>(wifi_aps_.size()) - 1) % static_cast<int>(wifi_aps_.size());
                sound_->play(Sfx::Move);
                wifi_render_list();
            } else if (is_down(key) && !wifi_aps_.empty()) {
                wifi_sel_ = (wifi_sel_ + 1) % static_cast<int>(wifi_aps_.size());
                sound_->play(Sfx::Move);
                wifi_render_list();
            } else if (is_enter(key) && wifi_sel_ < static_cast<int>(wifi_aps_.size()) && net_.radio_on) {
                const WifiAp& ap = wifi_aps_[wifi_sel_];
                sound_->play(Sfx::Open);
                if (ap.saved || ap.active) {
                    wifi_open_actions();
                } else if (ap.enterprise) {
                    wifi_open_eap();
                } else if (ap.secure) {
                    wifi_open_password();
                } else {
                    wifi_target_ = ap.ssid;
                    wifi_connect("");
                }
            } else {
                return false;  // ESC etc: normal page handling
            }
            return true;
    }
    return false;
}

// ---------------------------------------------------------------- ECO power mode
// ECO: the screen redraws at 15 frames a second (not 60), dims to a fifth after 20 s without a
// key and turns its backlight off after 60 s; the next key wakes it (and does nothing else).
namespace { constexpr uint32_t kEcoDimMs = 20000, kEcoOffMs = 60000; }

void VelvetUI::eco_set(bool on) {
    eco_ = on;
    if (lv_display_t* d = lv_display_get_default())
        if (lv_timer_t* t = lv_display_get_refr_timer(d)) lv_timer_set_period(t, on ? 66 : LV_DEF_REFR_PERIOD);
    eco_wake();
    if (on && !eco_timer_)
        eco_timer_ = lv_timer_create([](lv_timer_t* t) {
            auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
            const uint32_t idle = lv_tick_elaps(self->last_input_ms_);
            if (!self->eco_ || self->music_playing_video()) return;
            if (self->eco_dim_ == 0 && idle >= kEcoDimMs) {
                self->eco_saved_bright_ = self->device_->brightness();
                if (self->eco_saved_bright_ > 0) self->device_->set_brightness(std::max(5, self->eco_saved_bright_ / 5));
                self->eco_dim_ = 1;
            } else if (self->eco_dim_ == 1 && idle >= kEcoOffMs) {
                self->device_->set_brightness(0);
                self->eco_dim_ = 2;
            }
        }, 1000, this);
    if (!on && eco_timer_) { lv_timer_delete(eco_timer_); eco_timer_ = nullptr; }
}

// a key: back to full brightness; true when the screen was asleep (the key only wakes it)
bool VelvetUI::eco_wake() {
    last_input_ms_ = lv_tick_get();
    if (eco_dim_ == 0) return false;
    const bool was_off = eco_dim_ == 2;
    if (eco_saved_bright_ > 0) device_->set_brightness(eco_saved_bright_);
    eco_dim_ = 0;
    return was_off;
}

bool VelvetUI::music_playing_video() { return music_ && mus_vid_ != nullptr; }

// ---------------------------------------------------------------- network
// NETWORK: a menu of Wi-Fi (its own page), Ethernet, Tor, Tailscale, a speed test and tools.
// Every view but Wi-Fi is drawn by one callback (net_draw_cb) over a title bar and hints.
namespace {
enum NetView { NvMenu, NvEth, NvTor, NvTs, NvSpeed, NvTools, NvToolOut };
struct NetItem { Icon ic; const char* name; };
const NetItem kNetItems[] = {{IcWifi, "Wi-Fi"}, {IcEth, "Ethernet"}, {IcTor, "Tor"},
                             {IcTailscale, "Tailscale"}, {IcSpeed, "Speed test"}, {IcTools, "Tools"}};
constexpr int kNetCount = 6;
struct NetToolItem { NetTool t; const char* name; const char* what; };
const NetToolItem kNetTools[] = {
    {NetTool::Ping, "Ping", "Is it there, how fast"},
    {NetTool::Scan, "LAN scan", "Who's on this network"},
    {NetTool::Ports, "Port scan", "Open services on a host"},
    {NetTool::Dns, "DNS lookup", "A name's addresses"},
    {NetTool::Trace, "Trace route", "The hops to a host"}};
constexpr int kNetToolCount = 5;
constexpr int kNetOutRows = 7;   // tool output lines on screen
constexpr int kNetPeerRows = 4;  // Tailscale devices on screen

int nt_cw(const lv_font_t* f) { return lv_font_get_glyph_width(f, '0', 0); }
std::string nt_fit(const std::string& s, int max_cols) {
    if (max_cols <= 0) return {};
    if (static_cast<int>(utf8_cols(s.c_str())) <= max_cols) return s;
    std::string out;
    int cols = 0;
    for (size_t i = 0; i < s.size() && cols < max_cols - 2;) {
        size_t n = 1;
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
        out.append(s, i, n);
        i += n;
        ++cols;
    }
    return out + "..";
}
void nt_text(lv_layer_t* l, const lv_font_t* f, uint32_t col, const std::string& s, int32_t x, int32_t y) {
    if (s.empty()) return;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = C(col);
    d.text = s.c_str();
    d.text_local = 1;
    const lv_area_t a{x, y, x + 640, y + f->line_height};
    lv_draw_label(l, &d, &a);
}
// right-aligned so it ends at [right]
void nt_text_r(lv_layer_t* l, const lv_font_t* f, uint32_t col, const std::string& s, int32_t right, int32_t y) {
    nt_text(l, f, col, s, right - static_cast<int32_t>(utf8_cols(s.c_str())) * nt_cw(f), y);
}
void nt_box(lv_layer_t* l, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t col, int32_t radius = LV_RADIUS_CIRCLE) {
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = C(col);
    r.radius = radius;
    const lv_area_t a{x, y, x + w - 1, y + h - 1};
    lv_draw_rect(l, &r, &a);
}
void nt_icon(lv_layer_t* l, Icon ic, int32_t x, int32_t y, uint32_t col) {
    const lv_area_t a{x, y, x + 13, y + 13};
    draw_icon(l, ic, a, C(col));
}
// a thin progress bar
void nt_bar(lv_layer_t* l, int32_t x, int32_t y, int32_t w, float v, uint32_t track = kBg) {
    nt_box(l, x, y, w, 4, track);
    if (v > 0) nt_box(l, x, y, std::max<int32_t>(4, static_cast<int32_t>(w * std::min(v, 1.f))), 4, kWhite);
}
std::string mbps(float v) {
    char b[24];
    std::snprintf(b, sizeof b, v >= 100 ? "%.0f" : "%.1f", v);
    return b;
}
// the gauge's scale: 0 1 5 10 50 100 500 1000 Mb/s, spread like a log
float gauge_pos(float v) { return std::clamp(std::log10(1.f + std::max(0.f, v)) / 3.0f, 0.f, 1.f); }
}  // namespace

void VelvetUI::build_net(lv_obj_t* p) {
    if (!nett_) nett_ = make_net_tools();
    net_canvas_ = plain(p);
    lv_obj_set_pos(net_canvas_, 0, 26);
    lv_obj_set_size(net_canvas_, 320, 124);
    lv_obj_add_event_cb(net_canvas_, net_draw_cb, LV_EVENT_DRAW_MAIN, this);
    net_top_ = plain(p);
    lv_obj_set_size(net_top_, 320, 26);
    net_hint_ = plain(p);
    lv_obj_set_pos(net_hint_, 0, 151);
    lv_obj_set_size(net_hint_, 320, 16);
    net_hint_sig_.clear();
    net_editing_ = false;
    if (!wifi_from_net_) net_sel_ = 0;   // from the hub: the top; back from Wi-Fi: where it was
    wifi_from_net_ = false;
    net_set_view(NvMenu);
    nett_->eth_refresh();
    nett_->ts_refresh();
    device_->net_status([this](NetStatus st) {   // the Wi-Fi row's network
        net_ = st;
        update_hub_status();
        if (net_canvas_) lv_obj_invalidate(net_canvas_);
    });
    net_refresh_at_ = lv_tick_get() + 3000;
    net_timer_ = lv_timer_create(net_tick_cb, 50, this);
}

void VelvetUI::net_set_view(int view) {
    net_view_ = view;
    net_scroll_ = 0;
    static const char* const kTitles[] = {"Network", "Ethernet", "Tor", "Tailscale", "Speed test", "Tools", "Tools"};
    lv_obj_clean(net_top_);
    const char* title = view == NvToolOut ? kNetTools[net_tool_].name : kTitles[view];
    build_page_chrome(net_top_, title);
    if (nett_->demo() && (view == NvEth || view == NvTs || view == NvTools || view == NvToolOut))   // made-up data
        label(net_top_, "Demo", f_.mono10, kFaint, 4 + utf8_cols(title) * 8 + 20 + 8, 7);
    net_hint_sig_.clear();
    net_hints();
    if (view == NvEth) nett_->eth_refresh();
    if (view == NvTs) nett_->ts_refresh();
    lv_obj_invalidate(net_canvas_);
}

// the key hints for the view (rebuilt only when they change)
void VelvetUI::net_hints() {
    if (!net_hint_) return;
    using H = std::pair<const char*, const char*>;
    std::vector<H> h;
    switch (net_view_) {
        case NvMenu: h = {{"OK", "Open"}}; break;
        case NvEth: h = {{"R", "Refresh"}, {"T", "Tools"}}; break;
        case NvTor: {
            const int st = tor_->state();
            h = {{"OK", st == Tor::On || st == Tor::Starting ? "Turn off" : st == Tor::Missing ? "Install" : "Turn on"}};
            break;
        }
        case NvTs: {
            const int st = nett_->ts().state;
            if (nett_->ts_external()) h = {{"OK", st == TsInfo::Missing ? "Get Tailscale" : "Open Tailscale"}};
            else if (st == TsInfo::Missing) h = {{"R", "Check again"}};
            else h = {{"OK", st == TsInfo::On ? "Turn off" : st == TsInfo::NeedsLogin ? "Log in" : "Turn on"}, {"R", "Refresh"}};
            break;
        }
        case NvSpeed: {
            const int ph = nett_->speed().phase;
            h = {{"OK", ph >= SpeedInfo::Ping && ph <= SpeedInfo::Up ? "Stop" : ph == SpeedInfo::Idle ? "Start" : "Again"}};
            break;
        }
        case NvTools: h = net_editing_ ? std::vector<H>{{"OK", "Run"}, {"ESC", "Cancel"}} : std::vector<H>{{"OK", "Pick"}}; break;
        case NvToolOut: h = {{"\xE2\x86\x91\xE2\x86\x93", "Scroll"}, {"R", "Again"}}; break;
    }
    std::string sig = std::to_string(net_view_);
    for (auto& x : h) sig += std::string("|") + x.first + x.second;
    if (sig == net_hint_sig_) return;
    net_hint_sig_ = sig;
    lv_obj_clean(net_hint_);
    // hints_r takes an initializer_list: build the few shapes we use
    if (h.size() == 1) hints_r(net_hint_, 316, 0, f_.mono10, {h[0]});
    else if (h.size() == 2) hints_r(net_hint_, 316, 0, f_.mono10, {h[0], h[1]});
}

void VelvetUI::net_tick_cb(lv_timer_t* t) {
    auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
    if (!self->net_canvas_) return;
    const uint32_t now = lv_tick_get();
    if (now >= self->net_refresh_at_) {   // keep Ethernet / Tailscale fresh while they're shown
        self->net_refresh_at_ = now + 3000;
        if (self->net_view_ == NvMenu || self->net_view_ == NvEth) self->nett_->eth_refresh();
        if (self->net_view_ == NvMenu || self->net_view_ == NvTs) self->nett_->ts_refresh();
    }
    if (self->net_view_ == NvSpeed) {
        const SpeedInfo sp = self->nett_->speed();
        const float want = sp.phase == SpeedInfo::Idle || sp.phase == SpeedInfo::Ping || sp.phase == SpeedInfo::Failed ? 0.f : gauge_pos(sp.live);
        self->net_needle_ += (want - self->net_needle_) * 0.18f;
    }
    if (self->net_view_ == NvToolOut) {   // follow the output while it runs, unless scrolled up
        bool running = false;
        const int n = static_cast<int>(self->nett_->tool_lines(&running).size());
        static int last_n = 0;
        if (running && n != last_n && self->net_scroll_ >= std::max(0, last_n - kNetOutRows))
            self->net_scroll_ = std::max(0, n - kNetOutRows);
        last_n = n;
    }
    self->net_hints();
    lv_obj_invalidate(self->net_canvas_);
}

void VelvetUI::net_draw_cb(lv_event_t* e) {
    auto* self = static_cast<VelvetUI*>(lv_event_get_user_data(e));
    lv_layer_t* l = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->net_canvas_, &a);
    const int32_t ox = a.x1, oy = a.y1 - 26;   // page coordinates
    const Fonts& f = self->f_;
    NetTools& nt = *self->nett_;
    char buf[96];

    switch (self->net_view_) {
        case NvMenu: {
            for (int i = 0; i < kNetCount; ++i) {
                const int32_t y = oy + 29 + i * 20;
                const bool sel = i == self->net_sel_;
                if (sel) nt_box(l, ox + 4, y, 312, 19, kWhite);
                nt_icon(l, kNetItems[i].ic, ox + 14, y + 3, sel ? kInk : kWhite);
                nt_text(l, f.mono12, sel ? kInk : kWhite, kNetItems[i].name, ox + 36, y + 2);
                std::string v;
                bool on = false;
                switch (i) {
                    case 0:
                        v = !self->net_.radio_on ? "Off" : self->net_.connected ? self->net_.ssid : "Not connected";
                        on = self->net_.connected;
                        break;
                    case 1: {
                        const EthInfo eth = nt.eth();
                        v = !eth.present ? "No port" : !eth.link ? "Unplugged" : !eth.ip.empty() ? eth.ip : "Connected";
                        on = eth.link;
                        break;
                    }
                    case 2: {
                        const int st = self->tor_->state();
                        if (st == Tor::Unavailable) v = "Not here";
                        else if (st == Tor::Missing) v = "Not installed";
                        else if (st == Tor::Installing) v = "Installing";
                        else if (st == Tor::Starting) { std::snprintf(buf, sizeof buf, "Starting %d%%", self->tor_->percent()); v = buf; }
                        else v = st == Tor::On ? "On" : "Off";
                        on = st == Tor::On;
                        break;
                    }
                    case 3: {
                        const TsInfo ts = nt.ts();
                        v = ts.state == TsInfo::Missing ? "Not installed" : ts.state == TsInfo::On ? (ts.ip.empty() ? "On" : ts.ip)
                          : ts.state == TsInfo::NeedsLogin ? "Log in" : ts.state == TsInfo::Busy ? "..." : "Off";
                        on = ts.state == TsInfo::On;
                        break;
                    }
                    case 4: {
                        const SpeedInfo sp = nt.speed();
                        if (sp.phase == SpeedInfo::Done) v = "\xE2\x86\x93" + mbps(sp.down) + " \xE2\x86\x91" + mbps(sp.up) + " Mb/s";
                        else if (sp.phase > SpeedInfo::Idle) v = "Testing...";
                        on = sp.phase == SpeedInfo::Done;
                        break;
                    }
                    case 5: v = "Ping, scan, ports"; break;
                }
                nt_text_r(l, f.mono10, sel ? kInk : on ? kWhite : kGrey, nt_fit(v, 20), ox + 306, y + 3);
            }
            break;
        }
        case NvEth: {
            const EthInfo eth = nt.eth();
            nt_box(l, ox + 4, oy + 30, 312, 118, kDark, 10);
            nt_icon(l, IcEth, ox + 16, oy + 40, eth.link ? kWhite : kGrey);
            nt_text(l, f.big, kWhite, !eth.present ? "No Ethernet port" : eth.link ? "Connected" : "Unplugged", ox + 40, oy + 37);
            const char* names[] = {"Port", "Speed", "Address", "Gateway", "Hardware"};
            std::string vals[5];
            vals[0] = eth.iface.empty() ? "-" : eth.iface;
            vals[1] = !eth.link ? "-" : eth.speed > 0 ? std::to_string(eth.speed) + " Mb/s" : "?";
            vals[2] = eth.ip.empty() ? (eth.link ? "Waiting for an address" : "-") : eth.ip;
            vals[3] = eth.gateway.empty() ? "-" : eth.gateway;
            vals[4] = eth.mac.empty() ? "-" : eth.mac;
            for (int i = 0; i < 5; ++i) {
                nt_text(l, f.mono10, kGrey, names[i], ox + 16, oy + 64 + i * 16);
                nt_text(l, f.mono10, kWhite, nt_fit(vals[i], 26), ox + 96, oy + 64 + i * 16);
            }
            break;
        }
        case NvTor: {
            const int st = self->tor_->state();
            nt_box(l, ox + 4, oy + 30, 312, 118, kDark, 10);
            nt_icon(l, IcTor, ox + 16, oy + 40, st == Tor::On ? kWhite : kGrey);
            const char* big = st == Tor::On ? "On" : st == Tor::Starting ? "Starting" : st == Tor::Installing ? "Installing"
                            : st == Tor::Missing ? "Not installed" : st == Tor::Unavailable ? "Not available" : "Off";
            nt_text(l, f.big, kWhite, big, ox + 40, oy + 37);
            int32_t y = oy + 64;
            if (st == Tor::Starting) {
                std::snprintf(buf, sizeof buf, "Connecting to the Tor network %d%%", self->tor_->percent());
                nt_text(l, f.mono10, kWhite, buf, ox + 16, y);
                nt_bar(l, ox + 16, y + 18, 288, self->tor_->percent() / 100.f);
                y += 30;
            }
            const char* lines[] = {"The browser goes through Tor:", "sites can't see your address,", "pages load slower.", "Proxy: socks5 127.0.0.1:9052"};
            for (const char* s : lines) { nt_text(l, f.mono10, kGrey, s, ox + 16, y); y += 16; if (y > oy + 136) break; }
            const std::string err = self->tor_->error();
            if (!err.empty()) nt_text(l, f.mono10, kWhite, nt_fit(err, 36), ox + 16, oy + 130);
            break;
        }
        case NvTs: {
            const TsInfo ts = nt.ts();
            nt_box(l, ox + 4, oy + 30, 312, 38, kDark, 10);
            nt_icon(l, IcTailscale, ox + 16, oy + 42, ts.state == TsInfo::On ? kWhite : kGrey);
            const char* big = ts.state == TsInfo::On ? "On" : ts.state == TsInfo::NeedsLogin ? "Log in" : ts.state == TsInfo::Busy ? "Working"
                            : ts.state == TsInfo::Missing ? "Not installed" : "Off";
            nt_text(l, f.big, kWhite, big, ox + 40, oy + 39);
            if (!ts.ip.empty()) {
                nt_text_r(l, f.mono10, kWhite, ts.ip, ox + 306, oy + 36);
                nt_text_r(l, f.mono10, kGrey, nt_fit(ts.name, 18), ox + 306, oy + 51);
            }
            int32_t y = oy + 74;
            if (!ts.error.empty()) { nt_text(l, f.mono10, kWhite, nt_fit(ts.error, 37), ox + 10, y); y += 16; }
            if (ts.state == TsInfo::Missing && nt.ts_external()) {
                nt_text(l, f.mono10, kGrey, "OK opens the Play Store.", ox + 10, y);
            } else if (ts.state == TsInfo::Missing) {
                const char* lines[] = {"Install it once, in Terminal:", "curl -fsSL tailscale.com/install.sh", "  | sh", "then come back and press OK."};
                for (const char* s : lines) { nt_text(l, f.mono10, kGrey, s, ox + 10, y); y += 16; }
            } else if (ts.state == TsInfo::NeedsLogin) {
                nt_text(l, f.mono10, kGrey, "Open this link on your phone:", ox + 10, y);
                y += 16;
                const std::string u = ts.login_url.empty() ? "(press OK to get one)" : ts.login_url;
                for (size_t i = 0; i < u.size() && y < oy + 146; i += 37, y += 16) nt_text(l, f.mono10, kWhite, u.substr(i, 37), ox + 10, y);
            } else if (ts.state == TsInfo::On) {
                const int n = static_cast<int>(ts.peers.size());
                if (!n && ts.error.empty()) nt_text(l, f.mono10, kGrey, "No other devices yet", ox + 10, y);
                for (int i = 0; i < kNetPeerRows && self->net_scroll_ + i < n; ++i) {
                    const TsPeer& pe = ts.peers[self->net_scroll_ + i];
                    const int32_t ry = y + i * 18;
                    nt_box(l, ox + 12, ry + 4, 7, 7, pe.online ? kWhite : kFaint);
                    nt_text(l, f.mono10, pe.online ? kWhite : kGrey, nt_fit(pe.name, 20), ox + 28, ry);
                    nt_text_r(l, f.mono10, pe.online ? kWhite : kGrey, pe.ip, ox + 306, ry);
                }
            } else if (ts.state == TsInfo::Off) {
                nt_text(l, f.mono10, kGrey, "Your devices, anywhere, by name.", ox + 10, y);
                nt_text(l, f.mono10, kGrey, nt.ts_external() ? "OK opens the Tailscale app." : "Press OK to connect.", ox + 10, y + 16);
            }
            break;
        }
        case NvSpeed: {
            const SpeedInfo sp = nt.speed();
            // the gauge: a dark arc, white up to the needle, the needle on its pin
            const int32_t cx = ox + 96, cy = oy + 112, rad = 62;
            lv_draw_arc_dsc_t arc;
            lv_draw_arc_dsc_init(&arc);
            arc.center = {cx, cy};
            arc.radius = rad;
            arc.width = 6;
            arc.rounded = 1;
            arc.color = C(kDark);
            arc.start_angle = 180;
            arc.end_angle = 360;
            lv_draw_arc(l, &arc);
            const float pos = std::clamp(self->net_needle_, 0.f, 1.f);
            if (pos > 0.01f) {
                arc.color = C(kWhite);
                arc.end_angle = 180 + 180 * pos;
                lv_draw_arc(l, &arc);
            }
            const float marks[] = {0, 10, 100, 1000};
            const char* names[] = {"0", "10", "100", "1G"};
            for (int i = 0; i < 4; ++i) {   // outside the arc, clear of the needle
                const float ang = 3.14159265f * (1 + gauge_pos(marks[i]));
                const float k = i == 0 || i == 3 ? 0.f : 1.f;   // the ends sit under the arc's tips
                const int32_t tx = cx + static_cast<int32_t>(std::cos(ang) * (rad + 14 * k)),
                              ty = cy + static_cast<int32_t>(std::sin(ang) * (rad + 14 * k)) + (k ? 0 : 12);
                nt_text(l, f.mono10, kFaint, names[i], tx - static_cast<int32_t>(std::strlen(names[i])) * 4, ty - 7);
            }
            const float ang = 3.14159265f * (1 + pos);
            lv_draw_line_dsc_t ln;
            lv_draw_line_dsc_init(&ln);
            ln.color = C(kWhite);
            ln.width = 3;
            ln.round_start = ln.round_end = 1;
            ln.p1 = {static_cast<lv_value_precise_t>(cx), static_cast<lv_value_precise_t>(cy)};
            ln.p2 = {static_cast<lv_value_precise_t>(cx + std::cos(ang) * (rad - 10)), static_cast<lv_value_precise_t>(cy + std::sin(ang) * (rad - 10))};
            lv_draw_line(l, &ln);
            nt_box(l, cx - 6, cy - 6, 12, 12, kWhite);   // the pin
            nt_box(l, cx - 2, cy - 2, 4, 4, kBg);
            // the reading under the pin
            const bool moving = sp.phase == SpeedInfo::Down || sp.phase == SpeedInfo::Up;
            const float show = moving ? sp.live : sp.phase == SpeedInfo::Done ? sp.down : 0.f;
            if (moving || sp.phase == SpeedInfo::Done) {
                const std::string n = mbps(show);
                nt_text(l, f.big, kWhite, n, cx - static_cast<int32_t>(n.size()) * 5 - 18, cy + 12);
                nt_text(l, f.mono10, kGrey, "Mb/s", cx + static_cast<int32_t>(n.size()) * 5 - 12, cy + 16);
            } else if (sp.phase == SpeedInfo::Failed) {
                const std::string er = nt_fit(sp.error, 20);
                nt_text(l, f.mono10, kWhite, er, cx - static_cast<int32_t>(er.size()) * 4, cy + 14);
            } else if (sp.phase == SpeedInfo::Idle) {
                nt_text(l, f.mono10, kGrey, "OK to start", cx - 44, cy + 14);
            } else if (sp.phase == SpeedInfo::Ping) {
                nt_text(l, f.mono10, kGrey, "Pinging...", cx - 40, cy + 14);
            }
            // the three results on the right; the one running is lit
            struct R { const char* name; float v; const char* unit; int phase; };
            const R rs[] = {{"Ping", sp.ping_ms, "ms", SpeedInfo::Ping}, {"Download", sp.down, "Mb/s", SpeedInfo::Down}, {"Upload", sp.up, "Mb/s", SpeedInfo::Up}};
            for (int i = 0; i < 3; ++i) {
                const int32_t y = oy + 32 + i * 38;
                const bool cur = sp.phase == rs[i].phase;
                const bool have = sp.phase > rs[i].phase || sp.phase == SpeedInfo::Done || (rs[i].phase == SpeedInfo::Ping && sp.ping_ms > 0);
                nt_box(l, ox + 186, y, 130, 34, cur ? kWhite : kDark, 8);
                nt_text(l, f.mono10, cur ? kInk : kGrey, rs[i].name, ox + 194, y + 3);
                std::string v = have || (cur && i > 0) ? (i == 0 ? std::to_string(static_cast<int>(rs[i].v + 0.5f)) : mbps(cur ? sp.live : rs[i].v)) : "-";
                nt_text(l, f.mono12, cur ? kInk : kWhite, v, ox + 194, y + 16);
                nt_text_r(l, f.mono10, cur ? kInk : kGrey, rs[i].unit, ox + 308, y + 18);
                if (cur) { nt_box(l, ox + 194, y + 30, 114, 2, kLight); nt_box(l, ox + 194, y + 30, std::max<int32_t>(2, static_cast<int32_t>(114 * sp.progress)), 2, kInk); }
            }
            break;
        }
        case NvTools: {
            for (int i = 0; i < kNetToolCount; ++i) {
                const int32_t y = oy + 29 + i * 20;
                const bool sel = i == self->net_tool_;
                if (sel) nt_box(l, ox + 4, y, 312, 19, kWhite);
                nt_text(l, f.mono12, sel ? kInk : kWhite, kNetTools[i].name, ox + 14, y + 2);
                nt_text_r(l, f.mono10, sel ? kInk : kGrey, kNetTools[i].what, ox + 306, y + 3);
            }
            if (self->net_editing_) {   // the target, being typed
                nt_box(l, ox + 4, oy + 131, 312, 18, kDark);
                const std::string pre = std::string(kNetTools[self->net_tool_].name) + ":";
                nt_text(l, f.mono10, kGrey, pre, ox + 12, oy + 133);
                const bool blink = (lv_tick_get() / 400) % 2 == 0;
                const int room = 37 - static_cast<int>(pre.size()) - 1;
                std::string t = self->net_target_;
                if (static_cast<int>(t.size()) > room - 1) t = t.substr(t.size() - (room - 1));
                nt_text(l, f.mono10, kWhite, t + (blink ? "_" : " "), ox + 12 + (static_cast<int32_t>(pre.size()) + 1) * 8, oy + 133);
            }
            break;
        }
        case NvToolOut: {
            bool running = false;
            const std::vector<std::string> lines = nt.tool_lines(&running);
            nt_text(l, f.mono10, kGrey, nt_fit(self->net_target_, 30), ox + 8, oy + 30);
            if (running) {   // a small sliding pill while it works
                const float ph = static_cast<float>(lv_tick_get() % 1000) / 1000.f;
                nt_box(l, ox + 260, oy + 35, 48, 4, kDark);
                nt_box(l, ox + 260 + static_cast<int32_t>(36 * (ph < .5f ? ph * 2 : 2 - ph * 2)), oy + 35, 12, 4, kWhite);
            } else {
                nt_text_r(l, f.mono10, kFaint, "Done", ox + 308, oy + 30);
            }
            const int n = static_cast<int>(lines.size());
            self->net_scroll_ = std::clamp(self->net_scroll_, 0, std::max(0, n - kNetOutRows));
            for (int i = 0; i < kNetOutRows && self->net_scroll_ + i < n; ++i)
                nt_text(l, f.mono10, kWhite, nt_fit(lines[self->net_scroll_ + i], 38), ox + 8, oy + 48 + i * 14);
            break;
        }
    }
}

bool VelvetUI::net_key(uint32_t key, bool long_pressed) {
    if (long_pressed) return false;
    NetTools& nt = *nett_;
    auto back_to_menu = [&] {
        if (net_view_ == NvSpeed) nt.speed_cancel();
        sound_->play(Sfx::Swoosh);
        net_set_view(net_view_ == NvToolOut ? NvTools : NvMenu);
    };
    // typing a tool's target: every key is text
    if (net_view_ == NvTools && net_editing_) {
        if (key == LV_KEY_ESC) net_editing_ = false;
        else if (key == LV_KEY_BACKSPACE) { if (!net_target_.empty()) net_target_.pop_back(); sound_->play(Sfx::Key); }
        else if (key == LV_KEY_ENTER) {
            net_editing_ = false;
            nt.tool_run(kNetTools[net_tool_].t, net_target_);
            sound_->play(Sfx::Swoosh);
            net_set_view(NvToolOut);
            return true;
        } else if (key >= 32 && key < 127 && net_target_.size() < 80) { net_target_ += static_cast<char>(key); sound_->play(Sfx::Key); }
        net_hints();
        lv_obj_invalidate(net_canvas_);
        return true;
    }
    if (net_view_ == NvMenu) {
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) return false;   // back to the hub
        if (is_up(key)) net_sel_ = (net_sel_ + kNetCount - 1) % kNetCount;
        else if (is_down(key)) net_sel_ = (net_sel_ + 1) % kNetCount;
        else if (is_enter(key)) {
            if (net_sel_ == 0) {
                wifi_from_net_ = true;
                open_page(Page::Wifi);
            } else {
                sound_->play(Sfx::Swoosh);
                net_set_view(net_sel_);   // NetView matches the menu order
            }
            return true;
        } else return true;
        sound_->play(Sfx::Move);
        lv_obj_invalidate(net_canvas_);
        return true;
    }
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
        if (net_view_ == NvToolOut) nt.tool_cancel();
        back_to_menu();
        return true;
    }
    switch (net_view_) {
        case NvEth:
            if (key == 'r') { nt.eth_refresh(); sound_->play(Sfx::Move); }
            else if (key == 't') { sound_->play(Sfx::Swoosh); net_set_view(NvTools); }
            break;
        case NvTor:
            if (is_enter(key) && tor_->state() != Tor::Unavailable) {
                tor_->toggle();
                sound_->play(tor_->state() == Tor::On ? Sfx::ToggleOff : Sfx::ToggleOn);
            }
            break;
        case NvTs: {
            const TsInfo ts = nt.ts();
            if (key == 'r') { nt.ts_refresh(); sound_->play(Sfx::Move); }
            else if (is_enter(key) && nt.ts_external()) { nt.ts_set(true); sound_->play(Sfx::Move); }
            else if (is_enter(key) && ts.state != TsInfo::Missing && ts.state != TsInfo::Busy) {
                nt.ts_set(ts.state != TsInfo::On);
                sound_->play(ts.state == TsInfo::On ? Sfx::ToggleOff : Sfx::ToggleOn);
            } else if (is_row_up(key)) net_scroll_ = std::max(0, net_scroll_ - 1);
            else if (is_row_down(key)) net_scroll_ = std::min(std::max(0, static_cast<int>(ts.peers.size()) - kNetPeerRows), net_scroll_ + 1);
            break;
        }
        case NvSpeed:
            if (is_enter(key)) {
                const int ph = nt.speed().phase;
                if (ph >= SpeedInfo::Ping && ph <= SpeedInfo::Up) { nt.speed_cancel(); sound_->play(Sfx::ToggleOff); }
                else { nt.speed_start(); net_needle_ = 0; sound_->play(Sfx::ToggleOn); }
            }
            break;
        case NvTools:
            if (is_up(key)) { net_tool_ = (net_tool_ + kNetToolCount - 1) % kNetToolCount; sound_->play(Sfx::Move); }
            else if (is_down(key)) { net_tool_ = (net_tool_ + 1) % kNetToolCount; sound_->play(Sfx::Move); }
            else if (is_enter(key)) {
                net_target_ = nt.tool_default(kNetTools[net_tool_].t);
                net_editing_ = true;
                sound_->play(Sfx::Move);
            }
            break;
        case NvToolOut:
            if (key == 'r') { nt.tool_run(kNetTools[net_tool_].t, net_target_); net_scroll_ = 0; sound_->play(Sfx::Move); }
            else if (is_row_up(key)) net_scroll_ = std::max(0, net_scroll_ - 1);
            else if (is_row_down(key)) ++net_scroll_;   // clamped when drawn
            break;
    }
    net_hints();
    lv_obj_invalidate(net_canvas_);
    return true;
}

// ---------------------------------------------------------------- terminal
namespace {
#ifdef __EMSCRIPTEN__
constexpr uint32_t kHoldMs = 600;  // must match the long-press time of the input layer
#else
constexpr uint32_t kHoldMs = 700;
#endif

uint32_t blend(uint32_t a, uint32_t b) {  // 50/50 mix, for dim text
    return (((a >> 16 & 0xff) + (b >> 16 & 0xff)) / 2) << 16 | (((a >> 8 & 0xff) + (b >> 8 & 0xff)) / 2) << 8 |
           ((a & 0xff) + (b & 0xff)) / 2;
}

uint32_t term_rgb(int32_t c, bool bold) {
    if (c & Terminal::kRgb) return static_cast<uint32_t>(c) & 0xFFFFFF;
    if (c < 16) return kAnsi[bold && c < 8 ? c + 8 : c];
    if (c < 232) {
        static const uint32_t kLevels[] = {0, 95, 135, 175, 215, 255};
        const int i = c - 16;
        return kLevels[i / 36] << 16 | kLevels[i / 6 % 6] << 8 | kLevels[i % 6];
    }
    const uint32_t g = 8 + (c - 232) * 10;
    return g << 16 | g << 8 | g;
}

bool has_glyph(const lv_font_t* font, uint32_t cp) {
    if (cp >= 0x20 && cp < 0x7F) return true;
    lv_font_glyph_dsc_t g{};
    return lv_font_get_glyph_dsc(font, &g, cp, 0);
}

void utf8_put(std::string& s, uint32_t cp) {
    if (cp < 0x80) s += static_cast<char>(cp);
    else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}
}  // namespace

void VelvetUI::build_terminal(lv_obj_t* p) {
    lv_obj_set_style_bg_color(p, C(kTermBg), 0);
    // normal: 8x14 cells under the header. F1 compact: 6x12 cells, the whole screen, no header.
    term_compact_ = load_setting("term_compact", 0) != 0;
    term_cw_ = term_compact_ ? 6 : 8;
    term_ch_ = term_compact_ ? 12 : 14;
    term_top_ = term_compact_ ? 1 : 16;
    term_font_ = term_compact_ ? &velvet_ter_t12 : &velvet_ter_t14;
    const int cols = 320 / term_cw_, rows = (170 - term_top_) / term_ch_;
    const bool fresh = !term_;
    if (!term_) {
        term_ = std::make_unique<Terminal>(cols, rows);
        term_restart();
    } else if (term_->cols() != cols || term_->rows() != rows) {
        term_->resize(cols, rows);
        if (shell_) shell_->resize(cols, rows);
    }

    term_info_ = nullptr;
    if (!term_compact_) {
        // header: the title in a dark pill, the session info, the F1 hint
        text_pill(p, "Terminal", f_.mono10, kDark, kWhite, 1, 0, 15, 8);
        term_info_ = label(p, "", f_.mono10, kGrey, 92, 0);
        lv_label_set_long_mode(term_info_, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(term_info_, 14 * 8);
        term_info_text_.clear();
        hints_r(p, 319, -1, f_.mono10, {{"F1", "Compact"}});
    }

    term_view_ = plain(p);
    lv_obj_set_pos(term_view_, 0, term_top_);
    lv_obj_set_size(term_view_, cols * term_cw_, rows * term_ch_);
    lv_obj_add_event_cb(term_view_, term_draw_cb, LV_EVENT_DRAW_MAIN, this);

    term_blink_ = true;
    term_blink_at_ = lv_tick_get();
    term_cursor_row_ = -1;
    term_pump();
    if (term_compact_ && (fresh || term_compact_changed_)) {
        term_->clear_above();  // compact = just the prompt and your text, nothing above it
        term_pump();
    }
    term_compact_changed_ = false;
    term_render_info();
    term_timer_ = lv_timer_create(term_tick_cb, 30, this);
}

void VelvetUI::term_restart() {
    term_->reset();
    shell_.reset();
    shell_ = make_shell(term_->cols(), term_->rows());
    term_exited_ = false;
    if (term_view_) lv_obj_invalidate(term_view_);
}

void VelvetUI::term_render_info() {
    if (!term_info_) return;
    char buf[64];
    if (term_->view_offset() > 0) {
        std::snprintf(buf, sizeof buf, "Scroll -%d", term_->view_offset());
    } else if (term_exited_) {
        std::snprintf(buf, sizeof buf, "Ended");
    } else {
        std::snprintf(buf, sizeof buf, "%s %dx%d", shell_->name().c_str(), term_->cols(), term_->rows());
    }
    if (term_info_text_ != buf) {
        term_info_text_ = buf;
        lv_label_set_text(term_info_, buf);
        lv_obj_set_style_text_color(term_info_, C(term_->view_offset() > 0 ? kWhite : kGrey), 0);
    }
}

// Moves shell output into the terminal and redraws only the rows that changed.
void VelvetUI::term_pump() {
    if (!shell_ || !term_) return;
    for (int i = 0; i < 4; ++i) {  // bounded, so a flood of output can't freeze the UI
        const std::string out = shell_->read();
        if (out.empty()) break;
        term_->feed(out);
    }
    if (const std::string reply = term_->take_reply(); !reply.empty()) shell_->write(reply);
    if (!term_exited_ && !shell_->alive()) {
        term_->feed(shell_->read());
        term_exited_ = true;
        sound_->play(Sfx::Error);
        term_->feed("\r\n\x1b[0;1;31m[ SESSION ENDED ]\x1b[0m\r\nENTER: new session  ESC: back\r\n");
    }

    const uint32_t now = lv_tick_get();
    if (now - term_blink_at_ >= 530) {
        term_blink_ = !term_blink_;
        term_blink_at_ = now;
        term_->mark_row(term_->cursor_y());
    }
    if (term_cursor_row_ != term_->cursor_y()) {
        term_->mark_row(term_cursor_row_);
        term_->mark_row(term_->cursor_y());
        term_cursor_row_ = term_->cursor_y();
    }

    const uint64_t dirty = term_->take_dirty();
    if (dirty && term_view_) {
        if (dirty == ~0ull) {
            lv_obj_invalidate(term_view_);
        } else {
            lv_area_t a;
            lv_obj_get_coords(term_view_, &a);
            for (int y = 0; y < term_->rows(); ++y) {
                if (!(dirty >> y & 1)) continue;
                lv_area_t r{a.x1, a.y1 + y * term_ch_, a.x2, a.y1 + (y + 1) * term_ch_ - 1};
                lv_obj_invalidate_area(term_view_, &r);
            }
        }
    }
    term_render_info();
}

void VelvetUI::term_tick_cb(lv_timer_t* t) {
    static_cast<VelvetUI*>(lv_timer_get_user_data(t))->term_pump();
}

void VelvetUI::term_draw_cb(lv_event_t* e) {
    auto* self = static_cast<VelvetUI*>(lv_event_get_user_data(e));
    if (!self->term_) return;
    Terminal& t = *self->term_;
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->term_view_, &a);
    const lv_area_t clip = layer->_clip_area;
    const lv_font_t* font = self->term_font_;
    const int32_t kCellW = self->term_cw_, kCellH = self->term_ch_;
    const bool cursor = t.cursor_visible() && self->term_blink_ && !self->term_exited_;

    struct Look {
        uint32_t fg, bg;
        bool has_bg, under;
        bool operator==(const Look& o) const {
            return fg == o.fg && bg == o.bg && has_bg == o.has_bg && under == o.under;
        }
    };
    auto look = [&](int x, int y) {
        const Cell& c = t.view_cell(x, y);
        const bool bold = c.attr & Terminal::kBold;
        uint32_t fg = c.fg < 0 ? kTermFg : term_rgb(c.fg, bold);
        uint32_t bg = c.bg < 0 ? kTermBg : term_rgb(c.bg, false);
        bool has_bg = c.bg >= 0;
        if (c.attr & Terminal::kReverse) {
            std::swap(fg, bg);
            has_bg = true;
        }
        if (c.attr & Terminal::kDim) fg = blend(fg, bg);
        if (cursor && x == t.cursor_x() && y == t.cursor_y()) {  // white block cursor
            fg = kTermBg;
            bg = kWhite;
            has_bg = true;
        }
        return Look{fg, bg, has_bg, (c.attr & Terminal::kUnder) != 0};
    };

    for (int y = 0; y < t.rows(); ++y) {
        const int32_t y1 = a.y1 + y * kCellH;
        if (y1 > clip.y2 || y1 + kCellH - 1 < clip.y1) continue;
        int x = 0;
        while (x < t.cols()) {
            const Look lk = look(x, y);
            int end = x + 1;
            while (end < t.cols() && look(end, y) == lk) ++end;
            const lv_area_t run{a.x1 + x * kCellW, y1, a.x1 + end * kCellW - 1, y1 + kCellH - 1};
            if (lk.has_bg) {
                lv_draw_rect_dsc_t r;
                lv_draw_rect_dsc_init(&r);
                r.bg_color = C(lk.bg);
                r.bg_opa = LV_OPA_COVER;
                r.radius = 0;
                lv_draw_rect(layer, &r, &run);
            }
            std::string text;
            bool blank = true;
            for (int i = x; i < end; ++i) {
                uint32_t cp = t.view_cell(i, y).ch;
                if (cp != ' ') blank = false;
                if (!has_glyph(font, cp)) cp = '?';
                utf8_put(text, cp);
            }
            if (!blank) {
                lv_draw_label_dsc_t d;
                lv_draw_label_dsc_init(&d);
                d.font = font;
                d.color = C(lk.fg);
                d.text = text.c_str();
                d.text_local = 1;  // the draw task keeps its own copy
                lv_area_t ta = run;
                ta.x2 += kCellW;  // a little slack so the run never wraps
                lv_draw_label(layer, &d, &ta);
            }
            if (lk.under) {
                lv_draw_rect_dsc_t r;
                lv_draw_rect_dsc_init(&r);
                r.bg_color = C(lk.fg);
                r.bg_opa = LV_OPA_COVER;
                lv_area_t ul{run.x1, y1 + kCellH - 1, run.x2, y1 + kCellH - 1};
                lv_draw_rect(layer, &r, &ul);
            }
            x = end;
        }
    }
}

bool VelvetUI::term_key(uint32_t key, bool long_pressed) {
    if (!term_ || !shell_) return false;
    if (long_pressed) {
        if (key == LV_KEY_ESC) {
            term_esc_pending_ = false;  // a hold leaves the page; the shell never sees it
            hold_end();
            close_page();
        }
        return true;
    }
    if (term_exited_) {
        if (key == LV_KEY_ENTER) {
            term_restart();
            term_pump();
            if (term_compact_) term_->clear_above();
        } else if (key == LV_KEY_ESC) {
            close_page();
        }
        return true;
    }
    if (key == LV_KEY_ESC) {
        term_esc_pending_ = true;  // sent on release, unless it turns into a hold
        hold_begin("Back");
        return true;
    }
    if (term_esc_pending_) {  // Esc still down + another key = Alt+key, like a real terminal
        term_esc_pending_ = false;
        hold_end();
        shell_->write("\x1b");
    }
    if (key == kKeyF1) {  // compact / normal (F1 is Velvet's, not the program's)
        term_set_compact(!term_compact_);
        return true;
    }
    if (!term_->alt_screen() && (key == kKeyPgUp || key == kKeyPgDn)) {
        term_->scroll_view(key == kKeyPgUp ? term_->rows() - 1 : -(term_->rows() - 1));
        term_pump();
        return true;
    }
    const std::string bytes = term_->encode_key(key);
    if (bytes.empty()) return true;
    if (term_->view_offset() > 0) term_->scroll_view(-term_->view_offset());
    shell_->write(bytes);
    sound_->play(Sfx::Key);
    term_blink_ = true;
    term_blink_at_ = lv_tick_get();
    term_pump();
    return true;
}

namespace {
// The CardputerZero has FN and SYM LEDs under those keys (M5IOE1 I/O expander at I2C 0x4F,
// PYG9 / PYG11). If the kernel exposes them as LEDs, light the one for the active layer.
// The names aren't known until the hardware ships, so match loosely and do nothing if absent.
void key_layer_leds(int layer) {
#ifndef __EMSCRIPTEN__
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory("/sys/class/leds", ec)) return;
    for (const auto& e : fs::directory_iterator("/sys/class/leds", ec)) {
        std::string n = e.path().filename().string();
        for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        int want = -1;
        if (n.find("sym") != std::string::npos) want = layer == 2;
        else if (n.find("fn") != std::string::npos) want = layer == 1;
        if (want < 0) continue;
        if (FILE* f = std::fopen((e.path() / "brightness").c_str(), "w")) {
            std::fputs(want ? "1" : "0", f);
            std::fclose(f);
        }
    }
#else
    (void)layer;
#endif
}
}  // namespace

void VelvetUI::set_key_layer(int layer) {
    if (layer < 0 || layer > 2) layer = 0;
    if (layer == key_layer_ && (fn_badge_ != nullptr) == (layer != 0)) return;
    key_layer_ = layer;
    key_layer_leds(layer);
    if (fn_badge_) {
        lv_obj_delete(fn_badge_);
        fn_badge_ = nullptr;
    }
    if (sound_) sound_->play(layer ? Sfx::ToggleOn : Sfx::ToggleOff);
    if (!layer) return;
    // a white pill, bottom right, with a dot in the key legend's colour (orange fn layer,
    // blue symbol layer). On the top layer so it stays put across pages.
    const bool fn = layer == 1;
    const int32_t w = fn ? 38 : 46;
    fn_badge_ = pill(lv_layer_top(), 318 - w - 6, 147, w + 6, 22, kBg);  // a black rim so it reads over hints
    lv_obj_t* b = pill(fn_badge_, 3, 3, w, 16, kWhite);
    pill(b, 6, 5, 6, 6, fn ? 0xff6a2b : 0x3d8bd9);
    label(b, fn ? "FN" : "SYM", f_.bold11, kInk, 16, 1);
}

void VelvetUI::term_set_compact(bool on) {
    save_setting("term_compact", on ? 1 : 0);
    term_compact_changed_ = true;
    sound_->play(on ? Sfx::ToggleOn : Sfx::ToggleOff);
    // rebuild just the terminal page at the new size (the session carries on)
    clear_page_timers();
    lv_obj_clean(page_);
    page_kind_ = Page::Terminal;
    build_terminal(page_);
}

// ---- hold-ESC popup (like the FN badge, bottom right): a bar fills while ESC is held and the
// hold action (quit / back) happens when it's full. A quick tap never shows it.
void VelvetUI::hold_begin(const char* what) {
    hold_end();
    hold_what_ = what;
    hold_t0_ = lv_tick_get();
    hold_timer_ = lv_timer_create(hold_tick_cb, 16, this);
}

void VelvetUI::hold_end() {
    if (hold_timer_) {
        lv_timer_delete(hold_timer_);
        hold_timer_ = nullptr;
    }
    if (hold_pop_) {
        lv_obj_delete(hold_pop_);
        hold_pop_ = hold_fill_ = nullptr;
    }
}

void VelvetUI::hold_tick_cb(lv_timer_t* t) {
    auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
    const uint32_t el = lv_tick_elaps(self->hold_t0_);
    if (el < 120) return;  // quick taps stay invisible
    constexpr int32_t kW = 112;
    if (!self->hold_pop_) {
        // a dark card, bottom right: what the hold does, and a pill bar that fills; full = go
        lv_obj_t* pop = pill(lv_layer_top(), 316 - kW, 134, kW, 32, kDark, 10);
        char text[24];
        std::snprintf(text, sizeof text, "%s", self->hold_what_);
        label(pop, "ESC", self->f_.bold11, kGrey, 10, 3);
        label(pop, text, self->f_.bold11, kWhite, 42, 3);
        pill(pop, 10, 21, kW - 20, 5, kBg);
        self->hold_fill_ = pill(pop, 10, 21, 5, 5, kWhite);
        self->hold_pop_ = pop;
    }
    const float p = std::min(1.0f, static_cast<float>(el) / kHoldMs);
    lv_obj_set_width(self->hold_fill_, std::max<int32_t>(5, static_cast<int32_t>((kW - 20) * p)));
}

void VelvetUI::rep_stop() {
    if (rep_timer_) lv_timer_delete(rep_timer_);
    rep_timer_ = nullptr;
    rep_key_ = 0;
}

void VelvetUI::handle_key_release(uint32_t key) {
    if (key == rep_key_) rep_stop();
    if (key == LV_KEY_ESC) hold_end();
    if (key == bpan_key_) bpan_key_ = 0;  // browser: stop sliding
    if (key == LV_KEY_ESC && mus_esc_pending_) {  // music: a short ESC leaves Now Playing for the list
        mus_esc_pending_ = false;
        if (page_kind_ == Page::Music) music_full(false);
    }
    if (key == LV_KEY_ESC && b_esc_pending_) {  // browser: a short ESC goes back
        b_esc_pending_ = false;
        if (page_kind_ == Page::Browser) browser_back();
    }
    if (key == LV_KEY_ESC && term_esc_pending_) {
        term_esc_pending_ = false;
        if (page_kind_ == Page::Terminal && shell_ && !term_exited_) {
            shell_->write("\x1b");
            term_pump();
        }
    }
}

// ---------------------------------------------------------------- browser
// Maximum view: the page is the whole screen (Terminus 6x12, 53 x 14). TAB shows the toolbar.
namespace {
constexpr int kBCols = 53, kBRows = 14;
// Home page: DuckDuckGo. Search engines block a headless Chromium (robot checks, empty pages),
// but answer a plain request; so every search, from any engine's page, is done the plain way on
// DuckDuckGo lite (FAST: it's what FAST does anyway; FULL: shown in Chromium).
const char* const kBrowserHome = "https://duckduckgo.com/";
constexpr const char* kDdgLiteHome = "https://lite.duckduckgo.com/lite/";  // FAST can't run the JS one
constexpr float kZooms[] = {1.0f, 0.75f, 0.5f};                          // SYM+E (+) / SYM+R (-)

std::string google_query(const std::string& url);
// The URL-encoded words of a search on Google, Bing or DuckDuckGo, else "".
std::string engine_query(const std::string& url) {
    std::string q = google_query(url);
    if (!q.empty()) return q;
    for (const char* host : {"bing.com/search?", "duckduckgo.com/?", "duckduckgo.com/html/?", "duckduckgo.com/lite/?",
                             "duckduckgo.com/lite?"}) {
        const size_t h = url.find(host);
        if (h == std::string::npos || h > 24) continue;
        for (const char* key : {"?q=", "&q="}) {
            const size_t k = url.find(key, h);
            if (k == std::string::npos) continue;
            const size_t start = k + 3;
            const size_t end = url.find('&', start);
            q = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!q.empty()) return q;
        }
    }
    return {};
}

// "q" of a Google search (or the URL it was blocked on), else "".
std::string google_query(const std::string& url) {
    const size_t g = url.find("google.");
    if (g == std::string::npos || g > 20) return {};
    size_t qi = url.find("/search?");
    if (qi == std::string::npos) {
        qi = url.find("/sorry/");  // the robot check keeps the search in ?continue=
        if (qi == std::string::npos) return {};
    }
    const bool sorry = url.compare(qi, 7, "/sorry/") == 0;
    if (sorry) {  // ...&continue=https://www.google.com/search%3Fq%3Dpersona%2B5%26hl...
        for (const char* key : {"%3Fq%3D", "%26q%3D"}) {
            const size_t k = url.find(key, qi);
            if (k == std::string::npos) continue;
            const size_t start = k + std::strlen(key);
            const size_t end = url.find_first_of("&%", start) == std::string::npos ? url.size() : url.find("%26", start);
            std::string enc = url.substr(start, (end == std::string::npos ? url.size() : end) - start);
            std::string q;  // undo the extra layer of %-encoding (%2B -> +)
            for (size_t i = 0; i < enc.size(); ++i) {
                if (enc[i] == '%' && i + 2 < enc.size()) {
                    q += static_cast<char>(std::strtol(enc.substr(i + 1, 2).c_str(), nullptr, 16));
                    i += 2;
                } else {
                    q += enc[i];
                }
            }
            return q;
        }
        return {};
    }
    for (const char* key : {"?q=", "&q="}) {
        const size_t k = url.find(key, qi);
        if (k == std::string::npos) continue;
        const size_t start = k + std::strlen(key);
        const size_t end = url.find('&', start);
        return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
    return {};
}
constexpr int32_t kBCw = 6, kBCh = 12, kBTop = 1;
const char* const kBButtons[] = {"Back", "Reload", "Tabs", "New", "Close"};

std::string tail_chars(const std::string& s, size_t n) {  // last n characters (UTF-8 safe)
    if (utf8_len(s) <= n) return s;
    size_t skip = utf8_len(s) - n, i = 0;
    while (skip && i < s.size()) {
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
        --skip;
    }
    return s.substr(i);
}
}  // namespace

void VelvetUI::build_browser(lv_obj_t* p) {
    lv_obj_set_style_bg_color(p, C(kTermBg), 0);
    if (!fetcher_) fetcher_ = make_fetcher();
    if (btabs_.empty()) browser_new_tab(kBrowserHome);

    bview_ = plain(p);
    breader_ = load_setting("browser_reader", 0) != 0;
    lv_obj_set_pos(bview_, 0, breader_ ? kBTop : 0);
    lv_obj_set_size(bview_, 320, breader_ ? kBRows * kBCh : 170);
    lv_obj_add_event_cb(bview_, browser_draw_cb, LV_EVENT_DRAW_MAIN, this);

    // loading: a white pill that sweeps along the top edge
    bload_ = pill(p, -60, 0, 60, 3, kWhite);
    lv_obj_add_flag(bload_, LV_OBJ_FLAG_HIDDEN);

    // toolbar (hidden until TAB): tab count, address, buttons
    btool_ = plain(p);
    lv_obj_set_size(btool_, 320, 40);
    lv_obj_set_style_bg_color(btool_, C(kBg), 0);
    lv_obj_set_style_bg_opa(btool_, LV_OPA_COVER, 0);
    lv_obj_t* cnt = pill(btool_, 3, 3, 40, 16, kDark);
    bcount_ = label(cnt, "", f_.bold11, kWhite, 8, 1);
    pill(btool_, 47, 3, 228, 16, kWhite);
    burl_ = label(btool_, "", ter(velvet_ter_t12), kInk, 55, 5);
    int32_t x = 3;
    for (int i = 0; i < 5; ++i) {
        const int32_t w = static_cast<int32_t>(std::strlen(kBButtons[i])) * 8 + 14;
        bbtns_[i] = pill(btool_, x, 22, w, 16, kDark);
        label(bbtns_[i], kBButtons[i], f_.mono10, kWhite, 7, 1);
        x += w + 4;
    }
    bengine_ = label(btool_, "", f_.bold11, kWhite, 282, 4);  // Fast / Full (F2 switches)
    hints(btool_, 278, 23, f_.mono10, {{"F2", ""}});

    // typing into a form field on the page: a strip along the bottom
    bfbox_ = plain(p);
    lv_obj_set_pos(bfbox_, 0, 148);
    lv_obj_set_size(bfbox_, 320, 22);

    // tab list (T, or TABS in the toolbar)
    blist_ = plain(p);
    lv_obj_set_pos(blist_, 10, 40);
    lv_obj_set_size(blist_, 300, 126);

    btool_open_ = blist_open_ = false;
    b_esc_pending_ = false;
    bfield_ = -1;
    bpan_key_ = 0;
    breader_ = load_setting("browser_reader", 0) != 0;
#ifdef __EMSCRIPTEN__
    const int engine_default = 1;  // the demo tries the PC bridge first (real pages inside Velvet)
#else
    const int engine_default = 0;
#endif
    if (load_setting("browser_engine", engine_default) == 1) browser_set_engine(true);
#if !defined(__EMSCRIPTEN__)
    // OPTIC_BROWSER_URL=...: open this page straight away (for testing a site on the device over SSH)
    if (const char* u = std::getenv("OPTIC_BROWSER_URL"); u && *u && !bfull_) browser_go(u, true);
#endif
    browser_render();
    btimer_ = lv_timer_create(browser_tick_cb, 30, this);
}

void VelvetUI::browser_new_tab(const std::string& url) {
    BTab t;
    t.id = bnext_id_++;
    btabs_.push_back(t);
    bcur_ = static_cast<int>(btabs_.size()) - 1;
    browser_go(url, false);
}

void VelvetUI::browser_free(BTab& tab) {
    for (auto*& b : tab.imgs) {
        if (b) lv_draw_buf_destroy(b);
        b = nullptr;
    }
    tab.img_pixels = 0;
}

void VelvetUI::browser_relayout(BTab& tab) {
    const PageFonts pf{ter(velvet_ter_b16), ter(velvet_ter_b14), f_.big, f_.big, f_.mono12};
    tab.page = layout_page(tab.doc, 512, pf, tab.isz);
}

// Fetches and decodes the page's pictures (PNG / JPEG / GIF), a few at a time, re-laying the
// page out as each one arrives. Stops at about 2.5 million pixels per tab.
void VelvetUI::browser_load_images(BTab& tab) {
    const int id = tab.id;
    const uint32_t gen = tab.gen;
    const size_t n = std::min<size_t>(tab.doc.images.size(), 40);
    for (size_t i = 0; i < n; ++i) {
        fetcher_->get(tab.doc.images[i].url, [this, id, gen, i](bool ok, const std::string&, const std::string&,
                                                                const std::string& body, const std::string&) {
            auto it = std::find_if(btabs_.begin(), btabs_.end(), [id](const BTab& t) { return t.id == id; });
            if (it == btabs_.end() || it->gen != gen || !ok || i >= it->imgs.size()) return;
            BTab& t = *it;
            if (t.img_pixels > 2500000) return;
            lv_draw_buf_t* buf = decode_image(body, 492, 700);
            if (!buf) return;
            t.imgs[i] = buf;
            t.isz[i] = {static_cast<int>(buf->header.w), static_cast<int>(buf->header.h)};
            t.img_pixels += static_cast<size_t>(buf->header.w) * buf->header.h;
            browser_relayout(t);
            if (page_kind_ == Page::Browser && &t == &btab() && bview_) lv_obj_invalidate(bview_);
        });
    }
}

void VelvetUI::browser_close_tab() {
    if (fetcher_) fetcher_->cancel_all();
    browser_free(btab());
    btabs_.erase(btabs_.begin() + bcur_);
    if (btabs_.empty()) {
        browser_new_tab(kBrowserHome);
        return;
    }
    bcur_ = std::min(bcur_, static_cast<int>(btabs_.size()) - 1);
    browser_render();
}

void VelvetUI::browser_show(BTab& tab, const Doc& in) {
    Doc doc = in;
    // no heading near the top: show the page title as one, so you know where you are
    bool heading = false;
    for (size_t i = 0; i < std::min<size_t>(3, doc.blocks.size()); ++i) {
        for (const Span& s : doc.blocks[i].spans) heading |= s.style == kHeading;
    }
    if (!heading && !doc.title.empty() && doc.title != doc.url) {
        Block b;
        b.spans.push_back(Span{doc.title, kHeading, -1});
        doc.blocks.insert(doc.blocks.begin(), b);
    }
    tab.doc = doc;
    tab.title = doc.title;
    tab.lines = layout_doc(doc, kBCols);
    tab.link_line.assign(doc.links.size(), -1);
    for (int y = 0; y < static_cast<int>(tab.lines.size()); ++y) {
        for (const Span& s : tab.lines[y].spans) {
            if (s.link >= 0 && tab.link_line[s.link] < 0) tab.link_line[s.link] = y;
        }
    }
    tab.scroll = 0;
    tab.sel = -1;
    tab.loading = false;
    // rendered view
    browser_free(tab);
    tab.imgs.assign(doc.images.size(), nullptr);
    tab.isz.assign(doc.images.size(), {0, 0});
    tab.values.clear();
    for (const Field& f : doc.fields) tab.values.push_back(f.type == "text" ? f.value : "");
    browser_relayout(tab);
    tab.vx = 0;
    tab.vy = 0;
    browser_load_images(tab);
}

void VelvetUI::browser_go(const std::string& url_raw, bool remember) {
    const std::string url_in = unwrap_redirect(url_raw);  // skip search engines' tracking hops
    if (bfull_ && chrome_) {
        const std::string q = engine_query(url_in);
        if (!q.empty()) {  // any search engine's search: the plain-request search above
            browser_full_search(q);
            return;
        }
        if (url_in.rfind("optic:", 0) == 0) {
            btab().loading = true;
            chrome_->home();
            browser_render();
            return;
        }
    }
    if (bfull_ && chrome_ && url_in.rfind("optic:", 0) != 0) {  // Chromium keeps its own history
        BTab& tab = btab();
        tab.url = url_in;
        tab.loading = true;
        bfield_ = -1;
        chrome_->navigate(url_in);
        browser_render();
        return;
    }
    std::string url = url_in;
#ifndef __EMSCRIPTEN__
    // Wikipedia's mobile site puts the article first (the desktop one starts with menus)
    // (the language sites only: en.wikipedia.org -> en.m.wikipedia.org; www.wikipedia.org has no such twin)
    const size_t w = url.find(".wikipedia.org/");
    const size_t sch = url.find("://");
    if (w != std::string::npos && url.find(".m.wikipedia.org") == std::string::npos && sch != std::string::npos && sch < w) {
        const std::string sub = url.substr(sch + 3, w - (sch + 3));   // "en", "www", "zh"...
        if (sub != "www" && sub.find('.') == std::string::npos && sub.find('/') == std::string::npos) url.insert(w, ".m");
    }
#endif
    BTab& tab = btab();
    if (remember && !tab.url.empty() && tab.url != url) tab.back.push_back(tab.url);
    tab.url = url;
    tab.error.clear();
    ++tab.gen;
    fetcher_->cancel_all();  // a new page: drop anything still loading
    bfield_ = -1;
    std::string html;
    if (builtin_page(url, html)) {
        browser_show(tab, parse_html(html, url));
        browser_render();
        return;
    }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        browser_show(tab, parse_html("<title>Can't open</title><h1>CAN'T OPEN</h1><p>Only web pages (http / https) "
                                     "open here:</p><pre>" + url + "</pre><p><a href='https://duckduckgo.com/'>DuckDuckGo</a></p>",
                                     "optic:error"));
        browser_render();
        return;
    }
    {
        // FAST: searches (Google, Bing, DuckDuckGo's JavaScript page) go to DuckDuckGo lite
        const std::string q = engine_query(url);
        if (!q.empty() && url.rfind(kDdgLiteSearch, 0) != 0 && url.find("wikipedia.org") == std::string::npos) {
            browser_go(search_url("") + q, false);
            return;
        }
        if (url == kBrowserHome) {
#ifdef __EMSCRIPTEN__
            // the demo can't read DuckDuckGo: falls through to "opened in your browser"
#else
            browser_go(kDdgLiteHome, false);
            return;
#endif
        }
    }
    tab.loading = true;
    browser_render();
    const int id = tab.id;
    fetcher_->get(url, [this, id](bool ok, const std::string& final_url, const std::string& type,
                                  const std::string& body, const std::string& err) {
        auto it = std::find_if(btabs_.begin(), btabs_.end(), [id](const BTab& t) { return t.id == id; });
        if (it == btabs_.end()) return;
        BTab& t = *it;
        if (ok && !final_url.empty()) t.url = final_url;
        std::string results;
        if (ok && search_results_html(t.url, body, results)) {
            browser_show(t, parse_html(results, t.url));
        } else if (!ok) {
            sound_->play(Sfx::Error);
            browser_show(t, parse_html("<title>Can't load</title><h1>CAN'T LOAD</h1><p>" + err + "</p><pre>" + t.url +
                                           "</pre><p>R reloads. Or search:</p><form action='optic:search'>"
                                           "<input name='q' placeholder='Search'><input type='submit' value='SEARCH'>"
                                           "</form><p><a href='https://en.wikipedia.org/wiki/Special:Random'>Wikipedia: "
                                           "random article</a></p>",
                                       t.url));
        } else if (type.empty() || type.find("html") != std::string::npos || type.find("xml") != std::string::npos) {
            // a page that is only "go there" (meta refresh, or a one-line script): go there, a few times at most
            if (body.size() < 2000 && t.hops < 4) {
                std::string to = redirect_target(body);
                if (!to.empty()) {
                    if (to.rfind("//", 0) == 0) to = "https:" + to;
                    if (to.rfind("http", 0) == 0 && to != t.url) {
                        ++t.hops;
                        if (page_kind_ == Page::Browser) browser_go(to, false);
                        return;
                    }
                }
            }
            t.hops = 0;
            browser_show(t, parse_html(body, t.url));
        } else if (type.rfind("image/", 0) == 0) {  // a picture on its own: show it like a browser does
            const std::string name = t.url.substr(t.url.rfind('/') + 1);
            browser_show(t, parse_html("<title>" + name + "</title><img src='" + t.url + "' alt='" + name + "'>", t.url));
        } else if (type.rfind("text/", 0) == 0 || type.find("json") != std::string::npos) {
            browser_show(t, parse_text(body, t.url));
        } else {
            browser_show(t, parse_html("<title>File</title><h1>CAN'T SHOW THIS</h1><p>It's a " + type +
                                           " file. This browser shows pages and text.</p>",
                                       t.url));
        }
        if (page_kind_ == Page::Browser) browser_render();
    });
}

void VelvetUI::browser_back() {
    if (bfull_ && chrome_) {
        sound_->play(Sfx::Back);
        chrome_->back();
        return;
    }
    BTab& tab = btab();
    if (tab.back.empty()) {
        sound_->play(Sfx::Error);
        return;
    }
    const std::string url = tab.back.back();
    tab.back.pop_back();
    sound_->play(Sfx::Back);
    browser_go(url, false);
}

void VelvetUI::browser_select(int link) {
    BTab& t = btab();
    if (link < 0 || link >= static_cast<int>(t.link_line.size())) return;
    t.sel = link;
    const int y = t.link_line[link];
    if (y >= 0) {
        if (y < t.scroll) t.scroll = y;
        if (y >= t.scroll + kBRows) t.scroll = y - kBRows + 1;
    }
    sound_->play(Sfx::Move);
    browser_render();
}

void VelvetUI::browser_scroll(int delta) {
    BTab& t = btab();
    const int max = std::max(0, static_cast<int>(t.lines.size()) - kBRows);
    const int before = t.scroll;
    t.scroll = std::clamp(t.scroll + delta, 0, max);
    // keep the selected link on screen, else drop the selection
    if (t.sel >= 0) {
        const int y = t.link_line[t.sel];
        if (y < t.scroll || y >= t.scroll + kBRows) t.sel = -1;
    }
    if (t.scroll != before) browser_render();
}

void VelvetUI::browser_toolbar(bool open, bool edit_url) {
    btool_open_ = open;
    blist_open_ = false;
    if (open) {
        bfocus_ = edit_url ? 0 : bfocus_;
        bedit_ = btab().url;
        bedit_fresh_ = !bedit_.empty();
    }
    sound_->play(open ? Sfx::ToggleOn : Sfx::ToggleOff);
    browser_render();
}

void VelvetUI::browser_render() {
    if (!bview_) return;
    BTab& t = btab();
    lv_obj_invalidate(bview_);
    // toolbar
    if (btool_open_) {
        lv_obj_remove_flag(btool_, LV_OBJ_FLAG_HIDDEN);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d/%d", bcur_ + 1, static_cast<int>(btabs_.size()));
        lv_label_set_text(bcount_, buf);
        lv_label_set_text(bengine_, bfull_ ? "Full" : "Fast");
        const bool editing = bfocus_ == 0;
        const std::string shown = editing ? tail_chars(bedit_, 35) + (bedit_fresh_ ? "" : "_")
                                          : tail_chars(t.loading ? "loading " + t.url : t.title, 36);
        lv_label_set_text(burl_, shown.c_str());
        // a freshly shown address is "selected" (grey): typing replaces it
        lv_obj_set_style_text_color(burl_, C(editing ? (bedit_fresh_ ? kFaint : kInk) : kInk), 0);
        for (int i = 0; i < 5; ++i) {
            const bool sel = bfocus_ == 1 && i == bbtn_sel_;
            pill_set_color(bbtns_[i], sel ? kWhite : kDark);
            lv_obj_set_style_text_color(lv_obj_get_child(bbtns_[i], 0), C(sel ? kInk : kWhite), 0);
        }
    } else {
        lv_obj_add_flag(btool_, LV_OBJ_FLAG_HIDDEN);
    }
    // tab list
    lv_obj_clean(blist_);
    if (blist_open_) {
        lv_obj_remove_flag(blist_, LV_OBJ_FLAG_HIDDEN);
        pill(blist_, 0, 0, 300, 126, kDark, 12);
        label(blist_, "Tabs", f_.bold11, kWhite, 12, 4);
        hints_r(blist_, 290, 3, f_.mono10, {{"OK", "Open"}, {"DEL", "Close"}, {"N", "New"}});
        const int n = static_cast<int>(btabs_.size());
        const int first = std::clamp(blist_sel_ - 3, 0, std::max(0, n - 6));
        for (int i = first; i < std::min(n, first + 6); ++i) {
            const int32_t y = 22 + (i - first) * 17;
            const bool sel = i == blist_sel_;
            lv_obj_t* row = pill(blist_, 6, y, 288, 16, kWhite);
            if (!sel) lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            std::string name = btabs_[i].loading ? "loading..." : btabs_[i].title;
            if (utf8_len(name) > 31) name = name.substr(0, 31);
            char lab[64];
            std::snprintf(lab, sizeof lab, "%d %s", i + 1, name.c_str());
            lv_obj_t* l = label(row, lab, f_.mono10, sel ? kInk : kWhite, 8, 1);
            lv_obj_set_width(l, 256);
            lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
            if (i == bcur_) label(row, "\xE2\x80\xA2", f_.mono10, sel ? kInk : kWhite, 270, 1);
        }
    } else {
        lv_obj_add_flag(blist_, LV_OBJ_FLAG_HIDDEN);
    }
    if (t.loading) lv_obj_remove_flag(bload_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(bload_, LV_OBJ_FLAG_HIDDEN);
    // form field strip
    lv_obj_clean(bfbox_);
    if (bfield_ != -1) {
        lv_obj_remove_flag(bfbox_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t* bar = plain(bfbox_);
        lv_obj_set_size(bar, 320, 22);
        lv_obj_set_style_bg_color(bar, C(kBg), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_t* tag = pill(bfbox_, 3, 3, 46, 16, kDark);
        label(tag, "Type", f_.bold11, kWhite, 7, 1);
        pill(bfbox_, 53, 3, 264, 16, kWhite);
        label(bfbox_, (tail_chars(bedit_, 40) + "_").c_str(), ter(velvet_ter_t12), kInk, 61, 5);
    } else {
        lv_obj_add_flag(bfbox_, LV_OBJ_FLAG_HIDDEN);
    }
}

void VelvetUI::browser_tick_cb(lv_timer_t* tm) {
    auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(tm));
    if (!self->bload_ || self->btabs_.empty()) return;
    if (self->bfull_ && self->chrome_) self->browser_full_tick();
    if (self->bpan_key_ && (!self->breader_ || self->bfull_)) {  // held: glide steadily and slowly, for aiming
        const uint32_t held = lv_tick_elaps(self->bpan_t0_);
        if (held > 250) {
            // 70 screen pixels a second, by the clock (not by ticks), so it never speeds up
            const int32_t due = static_cast<int32_t>((held - 250) * 70 / 1000);
            const int32_t step = due - self->bpan_done_;
            if (step > 0) {
                self->bpan_done_ = due;
                const uint32_t k = self->bpan_key_;
                const int32_t dx = (k == 'z' || k == LV_KEY_LEFT) ? -step : (k == 'c' || k == LV_KEY_RIGHT) ? step : 0;
                const int32_t dy = (k == 'f' || k == LV_KEY_UP) ? -step : (k == 'x' || k == LV_KEY_DOWN) ? step : 0;
                self->browser_pan(dx, dy);
            }
        }
    }
    if (self->btab().loading) {
        lv_obj_remove_flag(self->bload_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(self->bload_, static_cast<int32_t>(lv_tick_get() / 3 % 400) - 60);
    } else {
        lv_obj_add_flag(self->bload_, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---- rendered view: pan, hover, click
namespace {
void br_rect(lv_layer_t* l, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t col);
void br_text(lv_layer_t* l, const lv_font_t* f, uint32_t col, const std::string& s, int32_t x, int32_t y, int32_t w);
void browser_cursor(lv_layer_t* l, int32_t cx, int32_t cy, bool on);
void browser_note(lv_layer_t* l, const lv_area_t& a, const std::string& text, bool toast);
}  // namespace

void VelvetUI::browser_pan(int32_t dx, int32_t dy) {
    BTab& t = btab();
    // the cursor sits in the middle of the screen, so let the page slide half a screen past
    // every edge: that way the cursor reaches every corner
    const float z = bz();  // zoomed out, one screen pixel covers more of the page
    const int32_t hw = static_cast<int32_t>(160 / z), hh = static_cast<int32_t>(85 / z);
    const int32_t x = std::clamp(t.vx + static_cast<int32_t>(dx / z), -hw, std::max<int32_t>(-hw, browser_page_w() - hw));
    const int32_t y = std::clamp(t.vy + static_cast<int32_t>(dy / z), -hh, std::max<int32_t>(-hh, browser_page_h() - hh));
    if (x == t.vx && y == t.vy) return;
    t.vx = x;
    t.vy = y;
    bmoved_at_ = lv_tick_get();
    if (bview_) lv_obj_invalidate(bview_);
}

// FULL: search engines flag headless Chromium as a bot, so the search itself is a plain
// request (like the FAST engine) to DuckDuckGo lite, and the result page is shown in Chromium.
void VelvetUI::browser_full_search(const std::string& q_enc) {
    BTab& t = btab();
    t.loading = true;
    bfield_ = -1;
#ifdef __EMSCRIPTEN__
    chrome_->search(q_enc);  // the PC bridge does the same
#else
    fetcher_->get(kDdgLiteSearch + q_enc, [this](bool ok, const std::string&, const std::string&, const std::string& body,
                                                  const std::string& err) {
        if (!chrome_ || !bfull_) return;
        if (ok) chrome_->show_html(ddg_lite_for_chrome(body));
        else browser_toast("Search failed: " + err);
    });
#endif
    browser_render();
}

void VelvetUI::browser_toast(const std::string& text) {
    btoast_ = text;
    btoast_until_ = lv_tick_get() + 4000;
    if (bview_) lv_obj_invalidate(bview_);
}

int32_t VelvetUI::bcur_x() const { return btabs_[bcur_].vx + static_cast<int32_t>(160 / bz()); }
int32_t VelvetUI::bcur_y() const { return btabs_[bcur_].vy + static_cast<int32_t>(85 / bz()); }

// SYM+E (+) zooms in, SYM+R (-) out: the page keeps its 512 px layout, Chromium just draws it
// smaller, so more of the same page fits on the screen. The point under the cursor stays put.
void VelvetUI::browser_zoom(int dir) {
    if (!bfull_ || !chrome_) {
        browser_toast("Zoom works in the FULL engine: press F2");
        return;
    }
    int i = 0;
    while (i < 2 && kZooms[i] > bzoom_ + 0.01f) ++i;
    const int ni = std::clamp(i + dir, 0, 2);
    if (ni == i) return;
    BTab& t = btab();
    const int32_t cx = bcur_x(), cy = bcur_y();
    bzoom_ = kZooms[ni];
    t.vx = cx - static_cast<int32_t>(160 / bzoom_);
    t.vy = cy - static_cast<int32_t>(85 / bzoom_);
    chrome_->set_scale(bzoom_);
    const auto& st = chrome_->state();
    chrome_->capture(std::clamp(cy - ChromeEngine::kH / 2, 0, std::max(0, st.height - ChromeEngine::kH)));
    bshot_at_ = lv_tick_get();
    save_setting("browser_zoom", ni);
    sound_->play(Sfx::Move);
    char buf[32];
    std::snprintf(buf, sizeof buf, "Zoom %d%%", static_cast<int>(bzoom_ * 100 + 0.5f));
    browser_toast(buf);
    browser_pan(0, 0);  // re-apply the limits for the new size
}

int32_t VelvetUI::browser_page_w() const {
    return bfull_ ? ChromeEngine::kW : btabs_[bcur_].page.width;
}
int32_t VelvetUI::browser_page_h() const {
    return bfull_ && chrome_ ? chrome_->state().height : btabs_[bcur_].page.height;
}

// F2: FAST (Velvet lays the page out itself) <-> FULL (Chromium, with JavaScript)
void VelvetUI::browser_set_engine(bool full) {
#if !defined(__EMSCRIPTEN__)
    if (full) {
        // Chromium wants about 1 GB: on less (a CardputerZero has 361 MB) it takes over a minute
        // for the simplest page and never shows a real one, so FULL isn't offered there.
        long mem_kb = 0;
        if (FILE* f = std::fopen("/proc/meminfo", "r")) {
            if (std::fscanf(f, "MemTotal: %ld", &mem_kb) != 1) mem_kb = 0;
            std::fclose(f);
        }
        if (mem_kb > 0 && mem_kb < 900 * 1024) {
            bfull_ = false;
            if (chrome_) chrome_->stop();
            save_setting("browser_engine", 0);
            sound_->play(Sfx::Error);
            BTab& t = btab();
            browser_show(t, parse_html("<title>FULL engine</title><h1>FULL NEEDS MORE MEMORY</h1><p>The FULL engine is "
                                       "Chromium, which needs about 1 GB of memory. This device has " +
                                           std::to_string(mem_kb / 1024) +
                                           " MB.</p><p>The FAST engine shows pages here. Press ESC to go back.</p>",
                                       "optic:full"));
            browser_render();
            return;
        }
    }
#endif
    if (full) {
        if (!chrome_) chrome_ = std::make_unique<ChromeEngine>();
        std::string err;
        if (!chrome_->start(err)) {
            bfull_ = false;
            sound_->play(Sfx::Error);
            BTab& t = btab();
            browser_show(t, parse_html("<title>FULL engine</title><h1>FULL NEEDS CHROMIUM</h1><p>" + err +
                                           "</p><p>Then press F2 again. Until then the FAST engine shows pages.</p>",
                                       "optic:full"));
            browser_render();
            return;
        }
        bfull_ = true;
        bzoom_ = kZooms[std::clamp(load_setting("browser_zoom", 0), 0, 2)];
        chrome_->set_scale(bzoom_);
        bfull_loads_ = chrome_->state().loads;
        bfull_serial_ = 0;
        bprobe_x_ = bprobe_y_ = -1;
        btab().loading = true;
        // navigate once Chromium is attached (browser_full_tick)
        bcap_at_[0] = 1;
    } else {
        bfull_ = false;
        if (chrome_) chrome_->stop();
        browser_go(btab().url.rfind("optic:", 0) == 0 ? std::string(kBrowserHome) : btab().url, false);
    }
    save_setting("browser_engine", bfull_ ? 1 : 0);
    if (bview_) lv_obj_invalidate(bview_);
}

// Runs every tick while FULL is on: Chromium's answers, new pages, keeping a fresh screenshot
// under the screen, and asking what is under the cursor once it stops moving.
void VelvetUI::browser_full_tick() {
    ChromeEngine& c = *chrome_;
    c.poll();
    const auto& st = c.state();
    BTab& t = btab();
    const uint32_t now = lv_tick_get();
    if (!st.ready) {
        if (!c.running() && !st.error.empty()) {  // Chromium went away: back to FAST
            bfull_ = false;
            browser_show(t, parse_html("<title>FULL engine</title><h1>NO FULL ENGINE</h1><p>" + st.error +
                                           "</p><p>Press F2 to try again. Meanwhile pages open in the FAST engine.</p>"
                                           "<form action='optic:search'><input name='q' placeholder='Search'>"
                                           "<input type='submit' value='SEARCH'></form>",
                                       "optic:full"));
            browser_render();
        }
        return;
    }
    if (bcap_at_[0] == 1) {  // first frame after starting: open the tab's page
        bcap_at_[0] = 0;
        const std::string url = t.url.empty() || t.url.rfind("optic:", 0) == 0 ? std::string(kBrowserHome) : t.url;
        browser_go(url, false);
    }
    if (st.loads != bfull_loads_) {  // a page finished loading
        bfull_loads_ = st.loads;
        t.vx = 0;
        t.vy = 0;
        c.capture(0);
        bcap_at_[1] = now + 1500;  // again once late pictures and scripts have run
    }
    for (auto& at : bcap_at_) {
        if (at > 1 && now >= at) {
            at = 0;
            c.capture(std::max(0, bcur_y() - ChromeEngine::kH / 2));
            bshot_at_ = now;
        }
    }
    // keep the screenshot covering the screen window
    const int32_t win_h = static_cast<int32_t>(170 / bz());  // page height the screen shows
    if (st.tile && (t.vy < st.tile_y || t.vy + win_h > st.tile_y + ChromeEngine::kH) && st.height > ChromeEngine::kH) {
        c.capture(std::clamp(bcur_y() - ChromeEngine::kH / 2, 0, st.height - ChromeEngine::kH));
        bshot_at_ = now;
    }
    // pages change by themselves (pop-ups, robot checks, live results): keep refreshing,
    // about once a second for a while after you do something, then every few seconds
    const uint32_t every = now - std::max(bacted_at_, bmoved_at_) < 15000 ? 1000 : 4000;
    if (st.tile && now - bshot_at_ > every) {
        bshot_at_ = now;
        c.capture(std::clamp(st.tile_y, 0, std::max(0, st.height - ChromeEngine::kH)));
    }
    {
        // Chromium reached a search engine's search by itself (a form, a link): search
        // DuckDuckGo lite the plain way instead, those pages block headless browsers
        const std::string q = engine_query(st.url);
        if (!q.empty() && st.url != bgoogle_seen_) {
            bgoogle_seen_ = st.url;
            browser_full_search(q);
        }
    }
    const bool was_loading = t.loading;
    t.loading = st.loading;
    if (!st.url.empty()) t.url = st.url;
    if (!st.title.empty()) t.title = st.title;
    if (st.tile_serial != bfull_serial_) {
        bfull_serial_ = st.tile_serial;
        if (bview_) lv_obj_invalidate(bview_);
    }
    // what's under the cursor, once it has been still for a moment
    if (now - bmoved_at_ > 120 && (bprobe_x_ != t.vx || bprobe_y_ != t.vy)) {
        bprobe_x_ = t.vx;
        bprobe_y_ = t.vy;
        c.probe(bcur_x(), bcur_y());
    }
    if (was_loading != t.loading || btool_open_) browser_render();
}

void VelvetUI::browser_draw_full(lv_layer_t* layer, const lv_area_t& a) {
    const BTab& t = btab();
    const auto& st = chrome_->state();
    const float z = bz();
    const int32_t ox = a.x1 - static_cast<int32_t>(t.vx * z), oy = a.y1 - static_cast<int32_t>(t.vy * z);
    br_rect(layer, a.x1, a.y1, 320, 170, kBg);
    {
        const int32_t x0 = std::max(a.x1, ox), y0 = std::max(a.y1, oy);
        const int32_t x1 = std::min(a.x1 + 320, ox + static_cast<int32_t>(ChromeEngine::kW * z));
        const int32_t y1 = std::min(a.y1 + 170, oy + static_cast<int32_t>(st.height * z));
        br_rect(layer, x0, y0, x1 - x0, y1 - y0, 0xffffff);
    }
    if (st.tile && std::fabs(st.tile_scale - z) < 0.01f) {  // (a tile at the old zoom waits for the new one)
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = st.tile;
        const int32_t ty = oy + static_cast<int32_t>(st.tile_y * z);
        lv_area_t ia{ox, ty, ox + static_cast<int32_t>(st.tile->header.w) - 1, ty + static_cast<int32_t>(st.tile->header.h) - 1};
        lv_draw_image(layer, &d, &ia);
    }
    const bool on = !st.hover.empty();
    browser_cursor(layer, a.x1 + 160, a.y1 + 85, on);
    if (lv_tick_get() < btoast_until_) {
        browser_note(layer, a, btoast_, true);
    } else if (on && !btool_open_ && bfield_ == -1) {
        std::string what = st.hover == "INPUT" ? "OK: type here" : st.hover == "BUTTON" ? "OK: press" : st.hover.substr(5);
        if (utf8_len(what) > 50) what = what.substr(0, 49) + "~";
        browser_note(layer, a, what, false);
    }
}

int VelvetUI::browser_hover() const {
    if (btabs_.empty()) return -1;
    const BTab& t = btabs_[bcur_];
    const int32_t px = t.vx + 160, py = t.vy + 85;
    int best = -1;
    int32_t best_d = 5;  // a little slack around small links
    for (int i = 0; i < static_cast<int>(t.page.boxes.size()); ++i) {
        const PBox& b = t.page.boxes[i];
        const bool clickable = b.link >= 0 || b.kind == kBoxInput || b.kind == kBoxButton;
        if (!clickable) continue;
        const int32_t dx = px < b.x ? b.x - px : px >= b.x + b.w ? px - (b.x + b.w - 1) : 0;
        const int32_t dy = py < b.y ? b.y - py : py >= b.y + b.h ? py - (b.y + b.h - 1) : 0;
        const int32_t d = std::max(dx, dy);
        if (d < best_d) {
            best_d = d;
            best = i;
            if (d == 0) break;
        }
    }
    return best;
}

void VelvetUI::browser_submit(int field) {
    BTab& t = btab();
    if (field < 0 || field >= static_cast<int>(t.doc.fields.size())) return;
    const std::string url = form_url(t.doc, t.doc.fields[field].form, t.values);
    sound_->play(Sfx::Open);
    browser_go(url, true);
}

void VelvetUI::browser_click() {
    BTab& t = btab();
    const int h = browser_hover();
    if (h < 0) return;
    const PBox& b = t.page.boxes[h];
    if (b.kind == kBoxInput) {  // type into the field (bottom strip), OK sends the form
        bfield_ = b.field;
        bedit_ = t.values[b.field];
        bedit_fresh_ = false;
        sound_->play(Sfx::ToggleOn);
        browser_render();
    } else if (b.kind == kBoxButton) {
        browser_submit(b.field);
    } else if (b.link >= 0 && b.link < static_cast<int>(t.doc.links.size())) {
        const std::string url = t.doc.links[b.link];
        const size_t hash = url.find('#');
        if (hash != std::string::npos && url.substr(0, hash) == t.url.substr(0, t.url.find('#'))) return;
        sound_->play(Sfx::Open);
        browser_go(url, true);
    }
}

namespace {
void br_rect(lv_layer_t* l, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(col);
    r.bg_opa = LV_OPA_COVER;
    lv_area_t a{x, y, x + w - 1, y + h - 1};
    lv_draw_rect(l, &r, &a);
}
void br_text(lv_layer_t* l, const lv_font_t* f, uint32_t col, const std::string& s, int32_t x, int32_t y, int32_t w) {
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = lv_color_hex(col);
    d.text = s.c_str();
    d.text_local = 1;
    lv_area_t a{x, y, x + std::max<int32_t>(w, 8) + 16, y + f->line_height - 1};
    lv_draw_label(l, &d, &a);
}
// The page cursor: a white plus with a black outline; over something clickable, an inverted,
// bigger plus.
void browser_cursor(lv_layer_t* l, int32_t cx, int32_t cy, bool on) {
    const int32_t k = on ? 3 : 2;
    br_rect(l, cx - 1, cy - k - 1, 3, 2 * k + 3, on ? kWhite : kInk);
    br_rect(l, cx - k - 1, cy - 1, 2 * k + 3, 3, on ? kWhite : kInk);
    br_rect(l, cx, cy - k, 1, 2 * k + 1, on ? kInk : kWhite);
    br_rect(l, cx - k, cy, 2 * k + 1, 1, on ? kInk : kWhite);
}
// A note along the bottom (a toast in a white pill, or where a link goes in a dark one).
void browser_note(lv_layer_t* l, const lv_area_t& a, const std::string& text, bool toast) {
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(toast ? kWhite : kDark);
    r.radius = LV_RADIUS_CIRCLE;
    const int32_t w = std::min<int32_t>(316, static_cast<int32_t>(utf8_len(text)) * 6 + 14);
    const lv_area_t b{a.x1 + 2, a.y1 + 153, a.x1 + 2 + w - 1, a.y1 + 168};
    lv_draw_rect(l, &r, &b);
    br_text(l, &velvet_ter_t12, toast ? kInk : kWhite, text, a.x1 + 9, a.y1 + 155, w - 14);
}
}  // namespace

// The page drawn like a (small) real browser would: white paper, dark text, blue links,
// pictures; the Velvet cursor sits on top in the middle of the screen.
void VelvetUI::browser_draw_page(lv_layer_t* layer, const lv_area_t& a) {
    const BTab& t = btab();
    const int32_t ox = a.x1 - t.vx, oy = a.y1 - t.vy;  // page (0,0) on screen
    br_rect(layer, a.x1, a.y1, 320, 170, kBg);  // outside the page
    {
        const int32_t x0 = std::max(a.x1, ox), y0 = std::max(a.y1, oy);
        const int32_t x1 = std::min(a.x1 + 320, ox + t.page.width), y1 = std::min(a.y1 + 170, oy + t.page.height);
        br_rect(layer, x0, y0, x1 - x0, y1 - y0, 0xffffff);
    }
    constexpr uint32_t kInkText = 0x15161c, kLinkBlue = 0x1f3fd6, kPGrey = 0x6b6f80, kLine = 0xc4c8d6;
    const int hover = browser_hover();
    for (int i = 0; i < static_cast<int>(t.page.boxes.size()); ++i) {
        const PBox& b = t.page.boxes[i];
        const int32_t x = ox + b.x, y = oy + b.y;
        if (y > a.y1 + 170 || y + b.h < a.y1 || x > a.x1 + 320 || x + b.w < a.x1) continue;
        const bool hot = i == hover;
        switch (b.kind) {
            case kBoxText: {
                uint32_t col = kInkText;
                if (b.style == kLink) col = kLinkBlue;
                else if (b.style == kDim || b.style == kQuote) col = kPGrey;
                else if (b.style == kHeading && b.level >= 3) col = 0x8a1029;
                if (hot) br_rect(layer, x - 1, y, b.w + 2, b.h, 0xe6e6e6);
                br_text(layer, b.font, col, b.text, x, y + (b.h - b.font->line_height) / 2, b.w);
                if (b.style == kLink) br_rect(layer, x, y + b.h - 2, b.w, 1, hot ? kInk : kLinkBlue);
                break;
            }
            case kBoxImage: {
                if (b.img >= 0 && b.img < static_cast<int>(t.imgs.size()) && t.imgs[b.img]) {
                    lv_draw_image_dsc_t d;
                    lv_draw_image_dsc_init(&d);
                    d.src = t.imgs[b.img];
                    lv_area_t ia{x, y, x + b.w - 1, y + b.h - 1};
                    lv_draw_image(layer, &d, &ia);
                } else {  // not loaded (yet), or a format we can't decode
                    br_rect(layer, x, y, b.w, b.h, kLine);
                    br_rect(layer, x + 1, y + 1, b.w - 2, b.h - 2, 0xeef0f6);
                    std::string alt = b.text.empty() ? "image" : b.text;
                    const size_t fit = static_cast<size_t>(std::max<int32_t>(1, (b.w - 8) / 6));
                    if (utf8_len(alt) > fit) alt = alt.substr(0, fit);
                    br_text(layer, &velvet_ter_t12, kPGrey, alt, x + 4, y + 3, b.w - 8);
                }
                if (b.link >= 0 && hot) {
                    br_rect(layer, x, y, b.w, 2, kInk);
                    br_rect(layer, x, y + b.h - 2, b.w, 2, kInk);
                }
                break;
            }
            case kBoxInput:
            case kBoxButton: {
                const bool input = b.kind == kBoxInput;
                br_rect(layer, x, y, b.w, b.h, hot ? kInk : 0x8890a8);
                br_rect(layer, x + 1, y + 1, b.w - 2, b.h - 2, input ? 0xffffff : 0xe6e8f0);
                std::string txt = input && b.field >= 0 && b.field < static_cast<int>(t.values.size()) && !t.values[b.field].empty()
                                      ? t.values[b.field] : b.text;
                const bool placeholder = input && (b.field < 0 || t.values[b.field].empty());
                const size_t fit = static_cast<size_t>(std::max<int32_t>(1, (b.w - 10) / 8));
                if (utf8_len(txt) > fit) txt = txt.substr(txt.size() - fit);
                br_text(layer, b.font, placeholder ? 0x9a9eb0 : kInkText, txt, x + (input ? 5 : 9), y + 3, b.w - 10);
                break;
            }
            case kBoxBullet: br_rect(layer, x, y, b.w, b.h, kInkText); break;
            case kBoxRule: br_rect(layer, x, y, b.w, 1, kLine); break;
            case kBoxPreBg: br_rect(layer, x, y, b.w, b.h, 0xeef0f6); break;
            case kBoxQuoteBar: br_rect(layer, x, y, b.w, b.h, kLine); break;
            default: break;
        }
    }

    // the cursor, fixed in the middle; it grows over something you can click
    const bool on = hover >= 0;
    browser_cursor(layer, a.x1 + 160, a.y1 + 85, on);

    if (lv_tick_get() < btoast_until_) {  // a short note along the bottom
        browser_note(layer, a, btoast_, true);
    } else
    // where a link goes / what OK will do, along the bottom
    if (on && !btool_open_ && bfield_ < 0) {
        const PBox& b = t.page.boxes[hover];
        std::string what = b.kind == kBoxInput ? "OK: type here" : b.kind == kBoxButton ? "OK: send" : t.doc.links[b.link];
        if (utf8_len(what) > 50) what = what.substr(0, 49) + "~";
        browser_note(layer, a, what, false);
    }
}

void VelvetUI::browser_draw_cb(lv_event_t* e) {
    auto* self = static_cast<VelvetUI*>(lv_event_get_user_data(e));
    if (self->btabs_.empty()) return;
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->bview_, &a);
    if (self->bfull_ && self->chrome_) {
        self->browser_draw_full(layer, a);
        return;
    }
    if (!self->breader_) {
        self->browser_draw_page(layer, a);
        return;
    }
    const BTab& t = self->btab();
    const lv_font_t* font = &velvet_ter_t12;
    auto colour = [&](const Span& s, bool sel) -> uint32_t {
        if (sel) return kInk;
        switch (s.style) {
            case kHeading: return kWhite;
            case kLink: return kLight;
            case kPre: return kAnsi[6];
            case kQuote: return kGrey;
            case kDim: return kFaint;
            case kBold: return kWhite;
            case kRule: return kDark;
            default: return kTermFg;
        }
    };
    for (int row = 0; row < kBRows; ++row) {
        const int li = t.scroll + row;
        if (li >= static_cast<int>(t.lines.size())) break;
        const int32_t y1 = a.y1 + row * kBCh;
        int32_t x = a.x1;
        for (const Span& s : t.lines[li].spans) {
            const int32_t w = static_cast<int32_t>(utf8_len(s.text)) * kBCw;
            const bool sel = s.link >= 0 && s.link == t.sel;
            if (sel) {  // selected link: black on white
                lv_draw_rect_dsc_t r;
                lv_draw_rect_dsc_init(&r);
                r.bg_color = C(kWhite);
                r.bg_opa = LV_OPA_COVER;
                lv_area_t ra{x, y1, x + w - 1, y1 + kBCh - 1};
                lv_draw_rect(layer, &r, &ra);
            }
            std::string txt;
            const char* p = s.text.c_str();
            while (*p) {  // characters the font lacks become '?'
                uint32_t cp = static_cast<unsigned char>(*p);
                int len = 1;
                if (cp >= 0xF0) { cp &= 0x07; len = 4; }
                else if (cp >= 0xE0) { cp &= 0x0F; len = 3; }
                else if (cp >= 0xC0) { cp &= 0x1F; len = 2; }
                for (int k = 1; k < len && p[k]; ++k) cp = (cp << 6) | (static_cast<unsigned char>(p[k]) & 0x3F);
                if (!has_glyph(font, cp)) txt += '?';
                else txt.append(p, static_cast<size_t>(len));
                p += len;
            }
            lv_draw_label_dsc_t d;
            lv_draw_label_dsc_init(&d);
            d.font = font;
            d.color = C(colour(s, sel));
            d.text = txt.c_str();
            d.text_local = 1;
            lv_area_t ta{x, y1, x + w + kBCw, y1 + kBCh - 1};
            lv_draw_label(layer, &d, &ta);
            if (s.style == kLink && !sel) {  // links are underlined
                lv_draw_rect_dsc_t r;
                lv_draw_rect_dsc_init(&r);
                r.bg_color = C(kFaint);
                r.bg_opa = LV_OPA_COVER;
                lv_area_t ul{x, y1 + kBCh - 1, x + w - 1, y1 + kBCh - 1};
                lv_draw_rect(layer, &r, &ul);
            }
            x += w;
        }
    }
    // scroll position: a 2 px bar on the right edge
    const int n = static_cast<int>(t.lines.size());
    if (n > kBRows) {
        const int32_t h = kBRows * kBCh, bar = std::max<int32_t>(8, h * kBRows / n);
        const int32_t y = (h - bar) * t.scroll / std::max(1, n - kBRows);
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_color = C(kWhite);
        r.bg_opa = LV_OPA_COVER;
        r.radius = 1;
        lv_area_t ba{a.x1 + 318, a.y1 + y, a.x1 + 319, a.y1 + y + bar - 1};
        lv_draw_rect(layer, &r, &ba);
    }
}

bool VelvetUI::browser_key(uint32_t key, bool long_pressed) {
    if (long_pressed) {
        if (key == LV_KEY_ESC) {  // hold ESC: leave the browser
            b_esc_pending_ = false;
            hold_end();
            close_page();
        }
        return true;
    }
    if (bfield_ != -1) {  // typing into a form field on the page (-2: a field in Chromium)
        BTab& ft = btab();
        if (key == LV_KEY_ESC) {
            bfield_ = -1;
        } else if (key == LV_KEY_ENTER && bfield_ == -2) {
            bfield_ = -1;
            chrome_->type(bedit_, true);
            bacted_at_ = lv_tick_get();
            bcap_at_[0] = lv_tick_get() + 400;
            bcap_at_[1] = lv_tick_get() + 1600;
            browser_render();
            return true;
        } else if (key == LV_KEY_ENTER) {
            const int f = bfield_;
            ft.values[f] = bedit_;
            bfield_ = -1;
            browser_submit(f);
            return true;
        } else if (key == LV_KEY_BACKSPACE) {
            if (!bedit_.empty()) bedit_.pop_back();
            sound_->play(Sfx::Key);
        } else if (key >= 32 && key < 127) {
            bedit_ += static_cast<char>(key);
            sound_->play(Sfx::Key);
        } else {
            return true;
        }
        if (bfield_ >= 0) ft.values[bfield_] = bedit_;
        browser_render();
        if (bview_) lv_obj_invalidate(bview_);
        return true;
    }
    if (key == LV_KEY_ESC) {
        if (blist_open_) {
            blist_open_ = false;
            browser_render();
        } else if (btool_open_) {
            browser_toolbar(false, false);
        } else {
            b_esc_pending_ = true;  // back on release, unless it turns into a hold
            hold_begin("Leave");
        }
        return true;
    }
    b_esc_pending_ = false;
    BTab& t = btab();
    if (key == LV_KEY_NEXT) {  // TAB: toolbar on / off
        browser_toolbar(!btool_open_, false);
        return true;
    }
    if (key == kKeyF1 + 1) {  // F2: FAST <-> FULL engine
        browser_set_engine(!bfull_);
        sound_->play(bfull_ ? Sfx::ToggleOn : Sfx::ToggleOff);
        browser_render();
        return true;
    }
    if (key == kKeyF1 && bfull_) return true;  // the text reader is a FAST-engine view
    if (key == kKeyF1) {  // F1: rendered page <-> text reader
        breader_ = !breader_;
        save_setting("browser_reader", breader_ ? 1 : 0);
        lv_obj_set_pos(bview_, 0, breader_ ? kBTop : 0);
        lv_obj_set_size(bview_, 320, breader_ ? kBRows * kBCh : 170);
        sound_->play(breader_ ? Sfx::ToggleOn : Sfx::ToggleOff);
        browser_render();
        return true;
    }
    if (blist_open_) {
        const int n = static_cast<int>(btabs_.size());
        if (is_row_up(key) || key == LV_KEY_LEFT || key == 'z') blist_sel_ = (blist_sel_ + n - 1) % n;
        else if (is_row_down(key) || key == LV_KEY_RIGHT || key == 'c') blist_sel_ = (blist_sel_ + 1) % n;
        else if (is_enter(key)) {
            bcur_ = blist_sel_;
            blist_open_ = btool_open_ = false;
            if (bfull_ && chrome_) chrome_->navigate(btab().url);
        } else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
            bcur_ = blist_sel_;
            browser_close_tab();
            blist_sel_ = std::min(blist_sel_, static_cast<int>(btabs_.size()) - 1);
        } else if (key == 'n' || key == 'N') {
            blist_open_ = false;
            browser_new_tab(kBrowserHome);
            browser_toolbar(true, true);
            return true;
        } else {
            return true;
        }
        sound_->play(Sfx::Move);
        browser_render();
        return true;
    }
    if (btool_open_) {
        if (bfocus_ == 0) {  // typing an address
            if (key == LV_KEY_ENTER) {
                std::string url = normalize_input(bedit_);
                const std::string search = search_url("");  // words typed -> a search
                if (bfull_ && url.rfind(search, 0) == 0) {
                    btool_open_ = false;
                    browser_full_search(url.substr(search.size()));
                    return true;
                }
                btool_open_ = false;
                browser_go(url, true);
            } else if (key == LV_KEY_BACKSPACE) {
                if (bedit_fresh_) bedit_.clear();
                else if (!bedit_.empty()) bedit_.pop_back();
                bedit_fresh_ = false;
                sound_->play(Sfx::Key);
            } else if (key == LV_KEY_DOWN) {
                bfocus_ = 1;
            } else if (key >= 32 && key < 127) {
                if (bedit_fresh_) bedit_.clear();
                bedit_fresh_ = false;
                bedit_ += static_cast<char>(key);
                sound_->play(Sfx::Key);
            } else {
                return true;
            }
            browser_render();
            return true;
        }
        // buttons row
        if (key == LV_KEY_LEFT || key == 'z') bbtn_sel_ = (bbtn_sel_ + 4) % 5;
        else if (key == LV_KEY_RIGHT || key == 'c') bbtn_sel_ = (bbtn_sel_ + 1) % 5;
        else if (key == LV_KEY_UP || key == 'f') {
            bfocus_ = 0;
            bedit_ = t.url;
            bedit_fresh_ = true;
        } else if (is_enter(key)) {
            switch (bbtn_sel_) {
                case 0: btool_open_ = false; browser_back(); break;
                case 1: btool_open_ = false; browser_go(t.url, false); break;
                case 2: blist_open_ = true; blist_sel_ = bcur_; break;
                case 3: browser_new_tab(kBrowserHome); bfocus_ = 0; bedit_.clear(); return (browser_render(), true);
                case 4: btool_open_ = false; browser_close_tab(); break;
                default: break;
            }
        } else {
            return true;
        }
        sound_->play(Sfx::Move);
        browser_render();
        return true;
    }
    // rendered page (FAST or FULL): the page slides under the fixed cursor
    if (!breader_ || bfull_) {
        // a tap nudges a little (aiming at small things); holding glides (browser_tick_cb)
        const int32_t dx = (key == 'z' || key == LV_KEY_LEFT) ? -8 : (key == 'c' || key == LV_KEY_RIGHT) ? 8 : 0;
        const int32_t dy = (key == 'f' || key == LV_KEY_UP) ? -8 : (key == 'x' || key == LV_KEY_DOWN) ? 8 : 0;
        if (key == '+' || key == '=') {  // SYM+E
            browser_zoom(-1);
            return true;
        }
        if (key == '-' || key == '_') {  // SYM+R
            browser_zoom(1);
            return true;
        }
        if (dx || dy) {
            if (key == bpan_key_) return true;  // a held key's auto-repeat: the glide handles it
            browser_pan(dx, dy);
            bpan_key_ = key;  // keeps sliding while held (browser_tick_cb), stops on release
            bpan_t0_ = lv_tick_get();
            bpan_done_ = 0;
            return true;
        }
        if (is_enter(key) && key != ' ') {
            if (bfull_ && chrome_) {  // click in Chromium; a text box opens Velvet's typing strip
                const bool input = chrome_->state().hover == "INPUT";
                chrome_->click(bcur_x(), bcur_y());
                bacted_at_ = lv_tick_get();
                sound_->play(Sfx::Move);
                bcap_at_[0] = lv_tick_get() + 350;
                bcap_at_[1] = lv_tick_get() + 1500;
                if (input) {
                    bfield_ = -2;
                    bedit_.clear();
                    browser_render();
                }
            } else {
                browser_click();
            }
        }
        else if (key == ' ' || key == kKeyPgDn) browser_pan(0, 150);
        else if (key == kKeyPgUp) browser_pan(0, -150);
        else if (key == LV_KEY_HOME) browser_pan(-100000, -100000);
        else if (key == LV_KEY_END) browser_pan(0, 100000);
        else if (key == LV_KEY_BACKSPACE) browser_back();
        else if (key == 'r' || key == 'R') {
            if (bfull_ && chrome_) chrome_->reload();
            else browser_go(t.url, false);
        } else if (key == 't' || key == 'T') {
            blist_open_ = true;
            blist_sel_ = bcur_;
            sound_->play(Sfx::ToggleOn);
            browser_render();
        } else if (key == 'n' || key == 'N') {
            browser_new_tab(kBrowserHome);
            browser_toolbar(true, true);
        } else if (key == 'g' || key == 'G' || key == '/') {
            browser_toolbar(true, true);
            bedit_.clear();
            browser_render();
        }
        return true;
    }
    // text reader
    const int page = kBRows - 1;
    if (key == 'f' || key == LV_KEY_UP) browser_scroll(-3);
    else if (key == 'x' || key == LV_KEY_DOWN) browser_scroll(3);
    else if (key == ' ' || key == kKeyPgDn) browser_scroll(page);
    else if (key == kKeyPgUp) browser_scroll(-page);
    else if (key == LV_KEY_HOME) browser_scroll(-100000);
    else if (key == LV_KEY_END) browser_scroll(100000);
    else if (key == 'c' || key == LV_KEY_RIGHT || key == 'z' || key == LV_KEY_LEFT) {
        const bool next = key == 'c' || key == LV_KEY_RIGHT;
        const int n = static_cast<int>(t.link_line.size());
        if (n == 0) return true;
        int target = -1;
        if (t.sel < 0) {  // first link on screen (or last, going back)
            for (int i = 0; i < n; ++i) {
                const int y = t.link_line[i];
                if (y >= t.scroll && y < t.scroll + kBRows) {
                    target = i;
                    if (next) break;
                }
            }
            if (target < 0) target = next ? 0 : n - 1;
        } else {
            target = next ? std::min(n - 1, t.sel + 1) : std::max(0, t.sel - 1);
        }
        browser_select(target);
    } else if (is_enter(key) && key != ' ') {
        if (t.sel >= 0 && t.sel < static_cast<int>(t.doc.links.size())) {
            std::string url = t.doc.links[t.sel];
            const size_t hash = url.find('#');
            const std::string page_url = t.url.substr(0, t.url.find('#'));
            if (hash != std::string::npos && url.substr(0, hash) == page_url) return true;  // same-page anchor
            sound_->play(Sfx::Open);
            browser_go(url, true);
        }
    } else if (key == LV_KEY_BACKSPACE) browser_back();
    else if (key == 'r' || key == 'R') browser_go(t.url, false);
    else if (key == 't' || key == 'T') {
        blist_open_ = true;
        blist_sel_ = bcur_;
        sound_->play(Sfx::ToggleOn);
        browser_render();
    } else if (key == 'n' || key == 'N') {
        browser_new_tab(kBrowserHome);
        browser_toolbar(true, true);
    } else if (key == 'g' || key == 'G' || key == '/') {
        browser_toolbar(true, true);
        bedit_.clear();
        browser_render();
    }
    return true;
}

// ---------------------------------------------------------------- sensors
namespace {
std::vector<lv_point_precise_t> circle_pts(float cx, float cy, float r, int n = 48) {
    std::vector<lv_point_precise_t> p;
    for (int i = 0; i < n; ++i) {
        const float a = 6.2831853f * i / n;
        p.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    return p;
}

void draw_text(lv_layer_t* layer, const lv_font_t* font, uint32_t color, const char* text, int32_t cx, int32_t cy) {
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = font;
    d.color = lv_color_hex(color);
    d.text = text;
    d.text_local = 1;
    const int32_t w = static_cast<int32_t>(std::strlen(text)) * 8;
    lv_area_t a{cx - w / 2, cy - 7, cx - w / 2 + w + 8, cy + 8};
    lv_draw_label(layer, &d, &a);
}

float tilt_x(const Motion& m) { return std::atan2(m.ax, m.az) * 57.29578f; }  // right edge up = +
float tilt_y(const Motion& m) { return std::atan2(m.ay, m.az) * 57.29578f; }  // top edge up = +

const char* compass_point(float h) {
    static const char* const kPts[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return kPts[static_cast<int>(std::fmod(h + 22.5f + 360.0f, 360.0f) / 45.0f) % 8];
}
}  // namespace

void VelvetUI::build_sensors(lv_obj_t* p) {
    build_page_chrome(p, "Sensors");
    if (!sensors_) sensors_ = make_sensors();
    if (!sensors_) {
        label(p, "No motion sensor", f_.mono12, kWhite, 20, 80);
        return;
    }
    motion_ = sensors_->read();

    compass_ = plain(p);
    lv_obj_set_pos(compass_, 6, 30);
    lv_obj_set_size(compass_, 104, 104);
    lv_obj_add_event_cb(compass_, compass_draw_cb, LV_EVENT_DRAW_MAIN, this);
    sens_heading_ = label(p, "", f_.mono10, kWhite, 0, 150);
    lv_obj_set_width(sens_heading_, 104);
    lv_obj_set_style_text_align(sens_heading_, LV_TEXT_ALIGN_CENTER, 0);

    level_ = plain(p);
    lv_obj_set_pos(level_, 116, 38);
    lv_obj_set_size(level_, 88, 88);
    lv_obj_add_event_cb(level_, level_draw_cb, LV_EVENT_DRAW_MAIN, this);
    sens_tilt_ = label(p, "", f_.mono10, kGrey, 112, 150);
    lv_obj_set_width(sens_tilt_, 96);
    lv_obj_set_style_text_align(sens_tilt_, LV_TEXT_ALIGN_CENTER, 0);

    pill(p, 210, 30, 106, 136, kDark, 10);
    sens_values_ = label(p, "", f_.mono10, kWhite, 220, 36);
    lv_obj_set_style_text_line_space(sens_values_, 1, 0);

    sens_timer_ = lv_timer_create(sensors_tick_cb, 50, this);
    sensors_tick_cb(sens_timer_);
}

void VelvetUI::sensors_tick_cb(lv_timer_t* t) {
    auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
    if (!self->sensors_ || !self->sens_values_) return;
    const Motion m = self->motion_ = self->sensors_->read();
    char buf[200];
    const float g = std::sqrt(m.ax * m.ax + m.ay * m.ay + m.az * m.az);
    std::snprintf(buf, sizeof buf,
                  "%s\nAX %+6.2f\nAY %+6.2f\nAZ %+6.2f\nGX %+6.1f\nGY %+6.1f\nGZ %+6.1f\n|G|  %5.2f",
                  m.source.c_str(), m.ax, m.ay, m.az, m.gx, m.gy, m.gz, g);
    lv_label_set_text(self->sens_values_, m.valid ? buf : "No data");
    if (m.has_heading) {
        std::snprintf(buf, sizeof buf, "%03d\xC2\xB0 %s", static_cast<int>(m.heading + 0.5f) % 360, compass_point(m.heading));
    } else {
        std::snprintf(buf, sizeof buf, "No compass");
    }
    lv_label_set_text(self->sens_heading_, buf);
    std::snprintf(buf, sizeof buf, "%+5.1f %+5.1f", tilt_x(m), tilt_y(m));
    lv_label_set_text(self->sens_tilt_, buf);
    const bool flat = std::fabs(tilt_x(m)) < 1.0f && std::fabs(tilt_y(m)) < 1.0f;
    lv_obj_set_style_text_color(self->sens_tilt_, C(flat ? kWhite : kGrey), 0);
    lv_obj_invalidate(self->compass_);
    lv_obj_invalidate(self->level_);
}

// Compass: the dial turns so N points north; the marker at the top is where you face.
// Letters move around the dial but stay upright.
void VelvetUI::compass_draw_cb(lv_event_t* e) {
    auto* self = static_cast<VelvetUI*>(lv_event_get_user_data(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->compass_, &a);
    const float cx = 52, cy = 52, r = 48;
    fill_poly(layer, circle_pts(cx, cy, r), C(kWhite), LV_OPA_COVER, a.x1, a.y1);
    fill_poly(layer, circle_pts(cx, cy, r - 2), C(kDark), LV_OPA_COVER, a.x1, a.y1);
    const float h = self->motion_.has_heading ? self->motion_.heading : 0.0f;
    for (int i = 0; i < 12; ++i) {  // ticks every 30 degrees
        const float ang = (i * 30.0f - h - 90.0f) * 0.0174533f;
        const float r0 = r - (i % 3 == 0 ? 11 : 8), r1 = r - 4;
        const float dx = std::cos(ang), dy = std::sin(ang), nx = -dy * 1.2f, ny = dx * 1.2f;
        fill_poly(layer,
                  {{cx + dx * r0 - nx, cy + dy * r0 - ny}, {cx + dx * r1 - nx, cy + dy * r1 - ny},
                   {cx + dx * r1 + nx, cy + dy * r1 + ny}, {cx + dx * r0 + nx, cy + dy * r0 + ny}},
                  C(kFaint), LV_OPA_COVER, a.x1, a.y1);
    }
    static const char* const kCard[] = {"N", "E", "S", "W"};
    for (int i = 0; i < 4; ++i) {
        const float ang = (i * 90.0f - h - 90.0f) * 0.0174533f;
        const int32_t x = a.x1 + static_cast<int32_t>(cx + std::cos(ang) * (r - 20));
        const int32_t y = a.y1 + static_cast<int32_t>(cy + std::sin(ang) * (r - 20));
        draw_text(layer, &velvet_ter_b14, i == 0 ? kWhite : kGrey, kCard[i], x, y);
    }
    // facing marker + centre dot
    fill_poly(layer, {{cx - 6, 0}, {cx + 6, 0}, {cx, 11}}, C(kWhite), LV_OPA_COVER, a.x1, a.y1);
    fill_poly(layer, circle_pts(cx, cy, 3, 16), C(kWhite), LV_OPA_COVER, a.x1, a.y1);
    if (!self->motion_.has_heading) draw_text(layer, &velvet_ter_b14, kGrey, "--", a.x1 + 52, a.y1 + 74);
}

// Bubble level: the bubble floats to the raised side; grey, white when level.
void VelvetUI::level_draw_cb(lv_event_t* e) {
    auto* self = static_cast<VelvetUI*>(lv_event_get_user_data(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->level_, &a);
    const float c = 44, r = 40;
    fill_poly(layer, circle_pts(c, c, r), C(kWhite), LV_OPA_COVER, a.x1, a.y1);
    fill_poly(layer, circle_pts(c, c, r - 2), C(kDark), LV_OPA_COVER, a.x1, a.y1);
    fill_poly(layer, {{c - 30, c - 0.5f}, {c + 30, c - 0.5f}, {c + 30, c + 0.5f}, {c - 30, c + 0.5f}}, C(kFaint),
              LV_OPA_COVER, a.x1, a.y1);
    fill_poly(layer, {{c - 0.5f, c - 30}, {c + 0.5f, c - 30}, {c + 0.5f, c + 30}, {c - 0.5f, c + 30}}, C(kFaint),
              LV_OPA_COVER, a.x1, a.y1);
    fill_poly(layer, circle_pts(c, c, 11, 32), C(kGrey), LV_OPA_COVER, a.x1, a.y1);
    fill_poly(layer, circle_pts(c, c, 10, 32), C(kDark), LV_OPA_COVER, a.x1, a.y1);
    const Motion& m = self->motion_;
    float bx = tilt_x(m) * 1.6f, by = -tilt_y(m) * 1.6f;
    const float d = std::sqrt(bx * bx + by * by), lim = r - 10;
    if (d > lim) {
        bx *= lim / d;
        by *= lim / d;
    }
    const bool flat = std::fabs(tilt_x(m)) < 1.0f && std::fabs(tilt_y(m)) < 1.0f;
    fill_poly(layer, circle_pts(c + bx, c + by, 7, 24), C(flat ? kWhite : kGrey), LV_OPA_COVER, a.x1, a.y1);
}

// ---------------------------------------------------------------- pwn
namespace {
const char* const kPwnMoods[] = {"awake", "happy", "excited", "cool", "bored", "sad", "lonely", "angry", "sleep",
                                 "intense", "grateful", "motivated", "demotivated", "friend", "smart", "broken",
                                 "look_l", "look_r"};
int pwn_mood_i(const std::string& m) {
    for (int i = 0; i < 18; ++i)
        if (m == kPwnMoods[i]) return i;
    if (m.rfind("sleep", 0) == 0) return 8;
    if (m.rfind("look_r", 0) == 0) return 17;
    if (m.rfind("look_l", 0) == 0) return 16;
    return 0;
}

// a rounded-rect outline polygon (clockwise from the top-left corner)
std::vector<lv_point_precise_t> rrect_pts(float x, float y, float w, float h, float r, int seg = 4) {
    std::vector<lv_point_precise_t> p;
    const float cxs[4] = {x + r, x + w - r, x + w - r, x + r};
    const float cys[4] = {y + r, y + r, y + h - r, y + h - r};
    const float a0[4] = {180, 270, 0, 90};
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i <= seg; ++i) {
            const float a = (a0[c] + 90.0f * i / seg) * 0.0174533f;
            p.push_back({cxs[c] + r * std::cos(a), cys[c] + r * std::sin(a)});
        }
    return p;
}

// Iris: an original eye-themed cyber-pet (not from any franchise). A rounded head with a neon rim,
// an antenna, a visor band holding two expressive eyes, cheek LEDs and a little mouth; the mood
// changes the eyes, the tint and the cheeks. Drawn in code so it works in every build.
void draw_iris(lv_layer_t* layer, int32_t ox, int32_t oy, const std::string& mn, bool live) {
    auto poly = [&](const std::vector<lv_point_precise_t>& pts, uint32_t col) {
        fill_poly(layer, pts, C(col), LV_OPA_COVER, ox, oy);
    };
    auto disc = [&](float cx, float cy, float r, uint32_t col) {
        fill_poly(layer, circle_pts(cx, cy, r, 20), C(col), LV_OPA_COVER, ox, oy);
    };
    auto bar = [&](float x0, float y0, float x1, float y1, float t, uint32_t col) {
        const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy) + 0.001f;
        const float nx = -dy / len * t, ny = dx / len * t;
        poly({{x0 - nx, y0 - ny}, {x1 - nx, y1 - ny}, {x1 + nx, y1 + ny}, {x0 + nx, y0 + ny}}, col);
    };

    // palette, tinted by mood
    uint32_t rim = 0x33e1e6, eye = 0x74f0ff, cheek = 0, mouth = 0, visor = 0x080c16;
    bool brow_angry = false, tear = false;
    if (mn == "happy" || mn == "grateful" || mn == "friend") { eye = 0x8bffc0; cheek = 0x35e06a; rim = 0x35e0a0; mouth = 0x8bffc0; }
    else if (mn == "excited" || mn == "motivated") { eye = 0xffe24d; cheek = 0xffae3d; rim = 0xffc24d; mouth = 0xffe24d; }
    else if (mn == "cool") { eye = 0x9fd8ff; rim = 0x7fb0ff; }
    else if (mn == "bored" || mn == "demotivated") { eye = 0x5a7384; rim = 0x3a4a58; }
    else if (mn == "sad" || mn == "lonely") { eye = 0x6aa8ff; rim = 0x4a6fb0; tear = true; }
    else if (mn == "angry" || mn == "intense") { eye = 0xff6a5a; rim = 0xff5a4a; brow_angry = true; mouth = 0xff6a5a; }
    else if (mn == "broken") { eye = 0xff5a5a; rim = 0x8a3a3a; }
    else if (mn == "smart") { eye = 0x74f0ff; cheek = 0x33e1e6; }
    if (!live) { rim = 0x2a3340; eye = 0x3b4a5a; cheek = 0; mouth = 0; }

    // antenna (tilts a touch toward where it looks)
    const float ax = 64 + (mn == "look_l" ? -5 : mn == "look_r" ? 5 : 0);
    bar(64, 26, ax, 12, 1.6f, 0x9aa4b5);
    disc(ax, 10, 5, 0xff4d8d);
    disc(ax, 10, 2.2f, 0xffd0e4);
    // side bolts
    disc(16, 66, 4, 0x2a3340);
    disc(112, 66, 4, 0x2a3340);
    // head: rim then body
    poly(rrect_pts(18, 24, 92, 84, 24), rim);
    poly(rrect_pts(21, 27, 86, 78, 21), 0x1b2233);
    // visor band
    poly(rrect_pts(28, 40, 72, 38, 17), visor);

    const float ex[2] = {48, 80}, ey = 59, er = 11;
    const bool blink = live && std::fmod(lv_tick_get() / 1000.0f, 5.0f) < 0.16f;
    for (int s = 0; s < 2; ++s) {
        const float x = ex[s], y = ey;
        if (mn == "sleep" || (blink && mn != "sleep")) { bar(x - er, y, x + er, y, 2.2f, eye); continue; }
        if (mn == "happy" || mn == "grateful" || mn == "friend") {  // ^ ^
            bar(x - er, y + er * 0.5f, x, y - er * 0.5f, 2.6f, eye);
            bar(x, y - er * 0.5f, x + er, y + er * 0.5f, 2.6f, eye);
            continue;
        }
        if (mn == "cool") { poly({{x - er - 1, y - 4}, {x + er + 1, y - 4}, {x + er + 1, y + 4}, {x - er - 1, y + 4}}, eye); continue; }
        if (mn == "broken") { bar(x - er, y - er, x + er, y + er, 2.2f, eye); bar(x + er, y - er, x - er, y + er, 2.2f, eye); continue; }
        if (mn == "bored" || mn == "demotivated") {
            bar(x - er, y - 3, x + er, y - 3, 2.0f, eye);
            disc(x, y + 2, er * 0.55f, eye);
            continue;
        }
        if (mn == "sad" || mn == "lonely") {
            disc(x, y + 2, er * 0.7f, eye);
            bar(x - er, y - er * (s == 0 ? 0.4f : 1.1f), x + er, y - er * (s == 0 ? 1.1f : 0.4f), 1.8f, rim);
            continue;
        }
        // round eye with a pupil (glow ring + dark + bright pupil); look shifts the pupil
        const float look = mn == "look_l" ? -er * 0.45f : mn == "look_r" ? er * 0.45f : 0;
        disc(x, y, er, eye);
        disc(x, y, er * 0.62f, 0x0a1018);
        disc(x + look, y, er * 0.34f, eye);
        disc(x + look + 2, y - 3, er * 0.14f, 0xffffff);
        if (brow_angry) bar(x - er, y - er * (s == 0 ? 1.3f : 0.4f), x + er, y - er * (s == 0 ? 0.4f : 1.3f), 2.6f, rim);
    }
    if (tear) disc(ex[0] - 3, ey + er + 4, 2.4f, 0x6aa8ff);
    if (cheek) { disc(40, 86, 3.2f, cheek); disc(88, 86, 3.2f, cheek); }
    if (mouth) {
        if (mn == "angry" || mn == "intense") bar(56, 92, 72, 92, 2.0f, mouth);
        else { bar(58, 90, 64, 94, 2.2f, mouth); bar(64, 94, 70, 90, 2.2f, mouth); }  // small smile
    }
}
}  // namespace

void VelvetUI::build_pwn(lv_obj_t* p) {
    build_page_chrome(p, "Pwn");
    if (!pwn_) pwn_ = make_pwn();
    pwn_state_ = pwn_->read();

    // the face card on the left
    pwn_face_ = plain(p);
    lv_obj_set_pos(pwn_face_, 4, 30);
    lv_obj_set_size(pwn_face_, 128, 128);
    lv_obj_set_style_bg_color(pwn_face_, C(kDark), 0);
    lv_obj_set_style_bg_opa(pwn_face_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pwn_face_, 12, 0);
    lv_obj_add_event_cb(pwn_face_, pwn_face_draw_cb, LV_EVENT_DRAW_MAIN, this);
    pwn_face_img_ = lv_image_create(pwn_face_);
    lv_obj_set_pos(pwn_face_img_, 8, 8);
    lv_obj_add_flag(pwn_face_img_, LV_OBJ_FLAG_HIDDEN);

    // name + a Live / Demo / Off tag pill
    pwn_name_ = label(p, "", f_.big, kWhite, 142, 32);
    pwn_tag_ = text_pill(p, "Demo", f_.mono10, kDark, kGrey, 142, 33, 16, 6);
    pwn_notice_ = label(p, "", f_.mono10, kGrey, 142, 54);   // a short fan-content flag; full text via the S overlay

    // the speech line, then the stats
    pwn_line_ = label(p, "", f_.bold11, kWhite, 142, 70);
    lv_obj_set_width(pwn_line_, 174);
    lv_label_set_long_mode(pwn_line_, LV_LABEL_LONG_WRAP);
    pwn_stats_ = label(p, "", f_.mono10, kGrey, 142, 110);
    lv_obj_set_width(pwn_stats_, 174);
    lv_label_set_long_mode(pwn_stats_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(pwn_stats_, 1, 0);

    pwn_about_ = label(p, "", f_.mono10, kWhite, 10, 32);
    lv_obj_set_width(pwn_about_, 300);
    lv_label_set_long_mode(pwn_about_, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(pwn_about_, LV_OBJ_FLAG_HIDDEN);

    hints_r(p, 316, 151, f_.mono10, {{"OK", pwn_state_.live ? "Stop" : "Start"}, {"S", "Skin"}});

    pwn_timer_ = lv_timer_create(pwn_tick_cb, 250, this);
    pwn_tick_cb(pwn_timer_);
}

void VelvetUI::pwn_tick_cb(lv_timer_t* t) {
    auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
    if (!self->pwn_ || !self->pwn_line_) return;
    const PwnState s = self->pwn_state_ = self->pwn_->read();
    const int mi = pwn_mood_i(s.mood);

    if (self->pwn_about_open_) return;  // the notice fills the page

    // the face: a PNG from the skin, else the drawn construct
    std::string fp = self->pwn_->face_file(s.live ? s.mood : std::string("sleep"));
    if (fp != self->pwn_face_path_) {
        self->pwn_face_path_ = fp;
        if (!fp.empty()) {
            lv_image_set_src(self->pwn_face_img_, fp.c_str());
            lv_image_set_inner_align(self->pwn_face_img_, LV_IMAGE_ALIGN_CONTAIN);
            lv_obj_set_size(self->pwn_face_img_, 112, 112);
            lv_obj_remove_flag(self->pwn_face_img_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(self->pwn_face_img_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (mi != self->pwn_shown_mood_) { self->pwn_shown_mood_ = mi; lv_obj_invalidate(self->pwn_face_); }

    const std::string sk = self->pwn_->skin_name();
    const bool iris = (sk == "Iris");
    if (iris != self->pwn_is_iris_) { self->pwn_is_iris_ = iris; lv_obj_invalidate(self->pwn_face_); }
    lv_label_set_text(self->pwn_name_, sk.c_str());
    const char* tag = !s.live ? "Off" : s.demo ? "Demo" : "Live";
    lv_obj_t* tl = lv_obj_get_child(self->pwn_tag_, 0);
    lv_label_set_text(tl, tag);
    lv_obj_set_width(self->pwn_tag_, utf8_cols(tag) * 8 + 12);
    pill_set_color(self->pwn_tag_, s.live && !s.demo ? kWhite : kDark);
    lv_obj_set_style_text_color(tl, C(!s.live ? kGrey : s.demo ? kWhite : kInk), 0);
    lv_obj_set_x(self->pwn_tag_, 142 + 8 + text_width(self->pwn_name_));

    const std::string notice = self->pwn_->notice();
    lv_label_set_text(self->pwn_notice_, notice.empty() ? "" : "(c) fan content");
    const int32_t line_y = notice.empty() ? 58 : 72;
    lv_obj_set_pos(self->pwn_line_, 142, line_y);
    const std::string line = s.live ? s.line : "Sleeping. OK to wake me.";
    if (line != self->pwn_shown_line_) { self->pwn_shown_line_ = line; lv_label_set_text(self->pwn_line_, line.c_str()); }
    lv_obj_set_style_text_color(self->pwn_line_, C(s.live ? kWhite : kGrey), 0);

    char b[200];
    if (s.live)
        std::snprintf(b, sizeof b, "CH %s  AP %s  GOT %s (%s)\nUP %s  PEERS %s  %s",
                      s.ch.c_str(), s.aps.c_str(), s.pwnd.c_str(), s.total.c_str(), s.uptime.c_str(), s.peers.c_str(), s.mode.c_str());
    else
        std::snprintf(b, sizeof b, "%s", "Only ever point it at networks you own or may test.");
    lv_obj_update_layout(self->pwn_line_);
    lv_obj_set_pos(self->pwn_stats_, 142, line_y + lv_obj_get_height(self->pwn_line_) + 6);
    lv_label_set_text(self->pwn_stats_, b);
}

// The construct face: the Optic visor outline with two eyes that show the mood.
void VelvetUI::pwn_face_draw_cb(lv_event_t* e) {
    auto* self = static_cast<VelvetUI*>(lv_event_get_user_data(e));
    if (self->pwn_face_img_ && !lv_obj_has_flag(self->pwn_face_img_, LV_OBJ_FLAG_HIDDEN)) return;  // a PNG is shown
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->pwn_face_, &a);
    const float cx = 64, cy = 64, s = 118;
    const bool live = self->pwn_state_.live;
    const int mi = live ? self->pwn_shown_mood_ : 8;
    const std::string mn = kPwnMoods[mi < 0 ? 0 : mi];
    const uint32_t col = live ? kWhite : kGrey;

    if (self->pwn_is_iris_) { draw_iris(layer, a.x1, a.y1, mn, live); return; }

    // visor hexagon outline
    const float w = s * 0.82f, h = s * 0.46f, t = s * 0.03f;
    const lv_point_precise_t v[6] = {{cx - w / 2, cy},        {cx - w / 2 + h / 2, cy - h / 2}, {cx + w / 2 - h / 2, cy - h / 2},
                                     {cx + w / 2, cy},        {cx + w / 2 - h / 2, cy + h / 2}, {cx - w / 2 + h / 2, cy + h / 2}};
    for (int i = 0; i < 6; ++i) {
        const auto& p0 = v[i];
        const auto& p1 = v[(i + 1) % 6];
        const float dx = p1.x - p0.x, dy = p1.y - p0.y, len = std::sqrt(dx * dx + dy * dy);
        const float nx = -dy / len * t, ny = dx / len * t;
        fill_poly(layer, {{p0.x - nx, p0.y - ny}, {p1.x - nx, p1.y - ny}, {p1.x + nx, p1.y + ny}, {p0.x + nx, p0.y + ny}},
                  C(col), LV_OPA_COVER, a.x1, a.y1);
    }
    // eyes
    const float ex = s * 0.17f, er = s * 0.075f;
    const bool blink = live && std::fmod(lv_tick_get() / 1000.0f, 5.0f) < 0.15f;
    for (int side = -1; side <= 1; side += 2) {
        const float x = cx + side * ex, y = cy;
        if (mn == "sleep" || (blink && mn != "sleep")) {
            fill_poly(layer, {{x - er, y - t}, {x + er, y - t}, {x + er, y + t}, {x - er, y + t}}, C(col), LV_OPA_COVER, a.x1, a.y1);
            continue;
        }
        if (mn == "happy" || mn == "grateful" || mn == "friend") {  // ^ ^
            for (int k = 0; k < 2; ++k) {
                const float x0 = k ? x : x - er, y0 = k ? y - er * 0.7f : y + er * 0.4f;
                const float x1 = k ? x + er : x, y1 = k ? y + er * 0.4f : y - er * 0.7f;
                const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
                const float nx = -dy / len * t, ny = dx / len * t;
                fill_poly(layer, {{x0 - nx, y0 - ny}, {x1 - nx, y1 - ny}, {x1 + nx, y1 + ny}, {x0 + nx, y0 + ny}}, C(col), LV_OPA_COVER, a.x1, a.y1);
            }
        } else if (mn == "excited" || mn == "motivated") {
            fill_poly(layer, circle_pts(x, y, er * 1.1f, 20), C(col), LV_OPA_COVER, a.x1, a.y1);
            fill_poly(layer, circle_pts(x, y, er * 0.55f, 16), C(kDark), LV_OPA_COVER, a.x1, a.y1);
        } else if (mn == "cool") {
            fill_poly(layer, {{x - er * 1.4f, y - er * 0.5f}, {x + er * 1.4f, y - er * 0.5f}, {x + er * 1.4f, y + er * 0.5f}, {x - er * 1.4f, y + er * 0.5f}},
                      C(col), LV_OPA_COVER, a.x1, a.y1);
        } else if (mn == "broken") {
            for (int k = 0; k < 2; ++k) {
                const float x0 = x - er, x1 = x + er, y0 = k ? y + er : y - er, y1 = k ? y - er : y + er;
                const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
                const float nx = -dy / len * t, ny = dx / len * t;
                fill_poly(layer, {{x0 - nx, y0 - ny}, {x1 - nx, y1 - ny}, {x1 + nx, y1 + ny}, {x0 + nx, y0 + ny}}, C(col), LV_OPA_COVER, a.x1, a.y1);
            }
        } else if (mn == "sad" || mn == "lonely" || mn == "angry" || mn == "bored" || mn == "demotivated") {
            fill_poly(layer, circle_pts(x, y + er * 0.3f, er * 0.6f, 18), C(col), LV_OPA_COVER, a.x1, a.y1);
        } else {  // awake / look / smart / intense / grateful fallback: ring + pupil
            const float look = mn == "look_l" ? -er * 0.6f : mn == "look_r" ? er * 0.6f : 0;
            fill_poly(layer, circle_pts(x, y, er, 20), C(col), LV_OPA_COVER, a.x1, a.y1);
            fill_poly(layer, circle_pts(x, y, er * 0.6f, 16), C(kDark), LV_OPA_COVER, a.x1, a.y1);
            fill_poly(layer, circle_pts(x + look, y, er * 0.35f, 12), C(col), LV_OPA_COVER, a.x1, a.y1);
        }
    }
}

bool VelvetUI::pwn_key(uint32_t key, bool long_pressed) {
    if (long_pressed) return false;
    if (pwn_about_open_) {  // any key closes the notice
        pwn_about_open_ = false;
        lv_obj_add_flag(pwn_about_, LV_OBJ_FLAG_HIDDEN);
        for (lv_obj_t* o : {pwn_face_, pwn_name_, pwn_tag_, pwn_notice_, pwn_line_, pwn_stats_})
            if (o) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        pwn_tick_cb(pwn_timer_);
        return true;
    }
    if (is_enter(key)) {
        pwn_->toggle();
        sound_->play(pwn_state_.live ? Sfx::ToggleOff : Sfx::ToggleOn);
        pwn_tick_cb(pwn_timer_);
        return true;
    }
    if (key == 's' || key == 'S' || is_right(key)) {
        pwn_->next_skin();
        if (pwn_face_) lv_obj_invalidate(pwn_face_);
        sound_->play(Sfx::Move);
        const std::string notice = pwn_->notice();
        if (!notice.empty()) {  // show the full ABOUT when a credited skin is picked
            std::string full = notice;
            lv_label_set_text(pwn_about_,
                              (full + "\n\nOptic Pwn shows a pwnagotchi (github.com/jayofelony/pwnagotchi, GPL-3.0). "
                                      "Only use it on networks you own or are allowed to test.").c_str());
            pwn_about_open_ = true;
            for (lv_obj_t* o : {pwn_face_, pwn_name_, pwn_tag_, pwn_notice_, pwn_line_, pwn_stats_})
                if (o) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(pwn_about_, LV_OBJ_FLAG_HIDDEN);
        }
        pwn_tick_cb(pwn_timer_);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- camera
void VelvetUI::build_camera(lv_obj_t* p) {
    lv_obj_set_style_bg_color(p, C(kBg), 0);
    cam_msg_ = label(p, "", f_.mono12, kGrey, 0, 76);
    lv_obj_set_width(cam_msg_, 320);
    lv_obj_set_style_text_align(cam_msg_, LV_TEXT_ALIGN_CENTER, 0);

    if (!camera_) camera_ = make_camera();
    if (!cam_buf_) cam_buf_ = lv_draw_buf_create(Camera::kW, Camera::kH, LV_COLOR_FORMAT_XRGB8888, 0);
    cam_img_ = lv_image_create(p);
    lv_obj_set_pos(cam_img_, 0, 0);
    lv_image_set_src(cam_img_, cam_buf_);
    lv_obj_add_flag(cam_img_, LV_OBJ_FLAG_HIDDEN);

    // viewfinder corners (white with a 1 px black edge so they show on bright scenes)
    const int32_t L = 16, T = 2, m = 10;
    const int32_t corners[4][2] = {{m, 30}, {320 - m - L, 30}, {m, 170 - 26 - L}, {320 - m - L, 170 - 26 - L}};
    for (int i = 0; i < 4; ++i) {
        const int32_t x = corners[i][0], y = corners[i][1];
        const bool right = i % 2, bottom = i / 2;
        for (const auto& [dx, col] : {std::pair<int32_t, uint32_t>{1, kInk}, {0, kWhite}}) {
            lv_obj_t* hz = plain(p);
            lv_obj_set_style_bg_opa(hz, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(hz, C(col), 0);
            lv_obj_set_pos(hz, x + dx, (bottom ? y + L - T : y) + dx);
            lv_obj_set_size(hz, L, T);
            lv_obj_t* vt = plain(p);
            lv_obj_set_style_bg_opa(vt, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(vt, C(col), 0);
            lv_obj_set_pos(vt, (right ? x + L - T : x) + dx, y + dx);
            lv_obj_set_size(vt, T, L);
        }
    }

    // title pill + status pill, hints in a dark pill (all opaque: they sit over the picture)
    lv_obj_t* tab = text_pill(p, "Camera", f_.mono10, kDark, kWhite, 4, 4, 20, 10);
    lv_obj_t* st_bg = pill(p, 8 + lv_obj_get_style_width(tab, LV_PART_MAIN), 4, 148, 20, kDark);
    cam_status_ = label(st_bg, "", f_.mono10, kGrey, 10, 3);
    lv_obj_t* hint_bg = pill(p, 0, 146, 10, 20, kDark);
    lv_obj_t* hb = hints(hint_bg, 8, 2, f_.mono10, {{"OK", "Snap"}, {"ESC", "Back"}});
    const int32_t hw = lv_obj_get_style_width(hb, LV_PART_MAIN) + 16;
    lv_obj_set_width(hint_bg, hw);
    lv_obj_set_x(hint_bg, 316 - hw);

    cam_banner_ = pill(p, 4, 124, 220, 20, kWhite);
    label(cam_banner_, "", f_.bold11, kInk, 10, 3);
    lv_obj_add_flag(cam_banner_, LV_OBJ_FLAG_HIDDEN);

    cam_flash_ = plain(p);
    lv_obj_set_size(cam_flash_, 320, 170);
    lv_obj_set_style_bg_color(cam_flash_, C(kWhite), 0);
    lv_obj_set_style_bg_opa(cam_flash_, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(cam_flash_, LV_OPA_TRANSP, 0);

    cam_busy_ = false;
    camera_->start();
    cam_timer_ = lv_timer_create(camera_tick_cb, 33, this);
    camera_tick_cb(cam_timer_);
}

void VelvetUI::camera_tick_cb(lv_timer_t* t) {
    auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
    if (!self->camera_ || !self->cam_img_) return;
    Camera& cam = *self->camera_;
    if (cam.frame(self->cam_frame_) && self->cam_frame_.size() == static_cast<size_t>(Camera::kW * Camera::kH)) {
        auto* px = reinterpret_cast<uint32_t*>(self->cam_buf_->data);
        const uint32_t stride = self->cam_buf_->header.stride / 4;
        for (int y = 0; y < Camera::kH; ++y) {
            for (int x = 0; x < Camera::kW; ++x) px[y * stride + x] = 0xFF000000u | self->cam_frame_[y * Camera::kW + x];
        }
        lv_image_cache_drop(self->cam_buf_);
        lv_obj_remove_flag(self->cam_img_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(self->cam_img_);
    }
    const bool live = cam.live();
    const std::string st = cam.status();
    lv_label_set_text(self->cam_status_, st.c_str());
    lv_obj_set_width(lv_obj_get_parent(self->cam_status_), text_width(self->cam_status_) + 20);
    lv_obj_set_style_text_color(self->cam_status_, C(live ? kWhite : kGrey), 0);
    if (!live) {
        lv_obj_add_flag(self->cam_img_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(self->cam_msg_, st == "STARTING" ? "Starting camera..."
                                         : st == "NO CAMERA ACCESS" ? "Allow camera access\nin the browser"
                                                                     : "No camera found");
    } else {
        lv_label_set_text(self->cam_msg_, "");
    }
    bool ok = false;
    std::string msg;
    if (cam.snap_result(ok, msg)) {
        self->cam_busy_ = false;
        if (!ok) self->sound_->play(Sfx::Error);
        lv_obj_t* bl = lv_obj_get_child(self->cam_banner_, 0);
        lv_label_set_text(bl, msg.c_str());
        lv_obj_set_width(self->cam_banner_, std::min<int32_t>(312, text_width(bl) + 20));
        pill_set_color(self->cam_banner_, ok ? kWhite : kDark);
        lv_obj_set_style_text_color(bl, C(ok ? kInk : kWhite), 0);
        lv_obj_remove_flag(self->cam_banner_, LV_OBJ_FLAG_HIDDEN);
        self->cam_banner_until_ = lv_tick_get() + 2500;
    }
    if (self->cam_banner_until_ && lv_tick_get() > self->cam_banner_until_) {
        self->cam_banner_until_ = 0;
        lv_obj_add_flag(self->cam_banner_, LV_OBJ_FLAG_HIDDEN);
    }
}

void VelvetUI::camera_snap() {
    if (!camera_ || !camera_->live() || cam_busy_) {
        sound_->play(Sfx::Error);
        return;
    }
    cam_busy_ = true;
    sound_->play(Sfx::Shutter);
    anim(cam_flash_, set_opa, 255, 0, 350, lv_anim_path_ease_out);
    camera_->snap();
}

// ---------------------------------------------------------------- boot splash
// Plays on start (and from SYSTEM → BOOT): the Optic Suite loading animation (one cycle) in the
// middle and one long thin bar along the bottom that fills meanwhile. Any key skips to the end.
namespace {
constexpr float kSplashExit = wordmark::kCycle * 0.96f;  // the lids have faded: dip to the screen
constexpr int kLogoY = 26, kLogoH = 96;                  // the band of the screen the logo uses
constexpr float kLogoS = 0.85f;                           // logo units → pixels
constexpr float kLogoOX = 160 - 165 * kLogoS, kLogoOY = 75 - 67 * kLogoS;

float sp_prog(float t, float a, float b) { return std::clamp((t - a) / (b - a), 0.0f, 1.0f); }
void sp_pill(lv_layer_t* l, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t col) {
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = C(col);
    r.radius = LV_RADIUS_CIRCLE;
    const lv_area_t a{x, y, x + w - 1, y + h - 1};
    lv_draw_rect(l, &r, &a);
}
}  // namespace

void VelvetUI::splash_start() {
    if (splash_) return;
    splash_exiting_ = false;
    splash_cues_ = 0;
    splash_ = plain(root_);
    lv_obj_set_size(splash_, 320, 170);
    lv_obj_set_style_bg_color(splash_, C(kBg), 0);
    lv_obj_set_style_bg_opa(splash_, LV_OPA_COVER, 0);
    // the logo: rasterised into an image each frame
    logo_cov_.assign(320 * kLogoH, 0.f);
    if (!logo_buf_) logo_buf_ = lv_draw_buf_create(320, kLogoH, LV_COLOR_FORMAT_ARGB8888, 0);
    lv_draw_buf_clear(logo_buf_, nullptr);
    logo_img_ = lv_image_create(splash_);
    lv_image_set_src(logo_img_, logo_buf_);
    lv_obj_set_pos(logo_img_, 0, kLogoY);
    // "suite" and the bar are drawn by one callback on top
    lv_obj_t* fx = plain(splash_);
    lv_obj_set_size(fx, 320, 170);
    lv_obj_add_event_cb(fx, splash_draw_cb, LV_EVENT_DRAW_MAIN, this);
    splash_fx_ = fx;
    lv_obj_move_foreground(splash_);
    splash_t0_ = lv_tick_get();
    sound_->play(Sfx::Swoosh);
    splash_timer_ = lv_timer_create(splash_tick_cb, 16, this);
}

float VelvetUI::splash_time() const { return lv_tick_elaps(splash_t0_) / 1000.0f; }

void VelvetUI::splash_skip() {
    if (!splash_ || splash_exiting_) return;
    splash_t0_ = lv_tick_get() - static_cast<uint32_t>(kSplashExit * 1000);
}

void VelvetUI::splash_tick_cb(lv_timer_t* tm) {
    auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(tm));
    if (!self->splash_) return;
    const float t = self->splash_time();
    if (!self->splash_exiting_) {
        auto& cov = self->logo_cov_;
        std::fill(cov.begin(), cov.end(), 0.f);
        wordmark::render(std::min(t / wordmark::kCycle, 1.f), kLogoS, kLogoOX, kLogoOY - kLogoY, cov.data(), 320, kLogoH);
        auto* px = reinterpret_cast<uint32_t*>(self->logo_buf_->data);
        const uint32_t stride = self->logo_buf_->header.stride / 4;
        for (int y = 0; y < kLogoH; ++y)
            for (int x = 0; x < 320; ++x) {
                const float c = cov[y * 320 + x];
                px[y * stride + x] = c <= 0.f ? 0u : (static_cast<uint32_t>(std::min(c, 1.f) * 255.f + .5f) << 24) | (kWhite & 0xFFFFFFu);
            }
        lv_image_cache_drop(self->logo_buf_);
        lv_obj_invalidate(self->logo_img_);
    }
    lv_obj_invalidate(self->splash_fx_);
    if (t >= kSplashExit && !self->splash_exiting_) {
        self->splash_exiting_ = true;
        self->sound_->play(Sfx::Swoosh);
        self->transition(true, [](VelvetUI* s) {
            if (s->splash_timer_) {
                lv_timer_delete(s->splash_timer_);
                s->splash_timer_ = nullptr;
            }
            lv_obj_delete(s->splash_);
            s->splash_ = s->splash_fx_ = s->logo_img_ = nullptr;
        });
    }
}

void VelvetUI::splash_draw_cb(lv_event_t* e) {
    auto* self = static_cast<VelvetUI*>(lv_event_get_user_data(e));
    lv_layer_t* l = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->splash_fx_, &a);
    const int32_t ox = a.x1, oy = a.y1;
    const float t = std::min(self->splash_time(), kSplashExit);

    // "suite": slides out from behind the eye with the letters
    const auto su = wordmark::suite(t / wordmark::kCycle);
    const lv_font_t* f = self->f_.mono10;
    const int32_t clip_x = ox + static_cast<int32_t>(std::lround(su.clip_x * kLogoS + kLogoOX));
    if (clip_x < ox + 320) {
        const lv_area_t keep = l->_clip_area;
        lv_area_t c = keep;
        c.x1 = std::max(c.x1, clip_x);
        if (c.x1 <= c.x2) {
            l->_clip_area = c;
            const int32_t top = oy + static_cast<int32_t>(std::lround(su.base * kLogoS + kLogoOY)) - (f->line_height - f->base_line);
            lv_draw_label_dsc_t d;
            lv_draw_label_dsc_init(&d);
            d.font = f;
            d.color = C(kWhite);
            d.text_local = 1;
            char ch[2] = {0, 0};
            for (int i = 0; i < 5; ++i) {
                ch[0] = "suite"[i];
                d.text = ch;
                const int32_t x = ox + static_cast<int32_t>(std::lround((su.x + i * su.advance) * kLogoS + kLogoOX));
                lv_area_t ta{x, top, x + 16, top + f->line_height};
                lv_draw_label(l, &d, &ta);
            }
            l->_clip_area = keep;
        }
    }

    // the bar: one long thin pill along the bottom, nothing else
    const float lp = sp_prog(t, 0.2f, kSplashExit - 0.4f);
    const float e2 = 1 - (1 - lp) * (1 - lp);
    const int32_t bx = ox + 24, by = oy + 150, bw = 272;
    sp_pill(l, bx, by, bw, 3, kDark);
    if (e2 > 0) sp_pill(l, bx, by, std::max<int32_t>(3, static_cast<int32_t>(bw * e2)), 3, kWhite);
}

// ---------------------------------------------------------------- credits (Easter egg)
// SYM Aa fn ctrl alt, in a row on the System page. Full screen, no hints: the QR code to the
// GitHub page with 2B beside it; right = 2B full screen; left = the QR slides right and the bio
// takes its place; a short ESC goes back to System.
namespace {
constexpr int kQrScale = 3;   // px per module (level-H codes are 33 / 37 modules: they fit at 3)
int32_t egg_qr_size() { return 132; }   // the white card the code sits in
float egg_qr_target(int view) { return view == 2 ? 320 - 8 - egg_qr_size() : 18; }
}  // namespace

void VelvetUI::mod_tap(int which) {
    if (page_kind_ != Page::System || egg_ || transitioning_) { egg_pos_ = 0; return; }
    static const int kSeq[5] = {1, 2, 3, 4, 5};
    egg_pos_ = which == kSeq[egg_pos_] ? egg_pos_ + 1 : (which == kSeq[0] ? 1 : 0);
    if (egg_pos_ == 5) {
        egg_pos_ = 0;
        egg_open();
    }
}

void VelvetUI::egg_open() {
    if (egg_) return;
    egg_frame_at_ = lv_tick_get();   // the full-screen logo animation's start
    egg_ = plain(root_);
    lv_obj_set_size(egg_, 320, 170);
    lv_obj_set_style_bg_color(egg_, C(kBg), 0);
    lv_obj_set_style_bg_opa(egg_, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(egg_, egg_draw_cb, LV_EVENT_DRAW_MAIN, this);
    lv_obj_move_foreground(egg_);
    egg_view_ = 0;
    egg_btc_ = false;
    egg_qr_x_ = egg_qr_target(0);
    egg_timer_ = lv_timer_create([](lv_timer_t* t) {   // the QR's slide, the 2B card's frames
        auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
        if (self->egg_view_ == 1 && self->egg_) {   // full screen: the Optic Suite loading animation, looping
            if (!self->egg_full_) self->egg_full_ = lv_draw_buf_create(320, kLogoH, LV_COLOR_FORMAT_ARGB8888, 0);
            self->egg_cov_.assign(320 * kLogoH, 0.f);
            const float ph = std::fmod(lv_tick_elaps(self->egg_frame_at_) / 1000.0f, wordmark::kCycle) / wordmark::kCycle;
            wordmark::render(ph, kLogoS, kLogoOX, kLogoOY - kLogoY, self->egg_cov_.data(), 320, kLogoH);
            auto* px = reinterpret_cast<uint32_t*>(self->egg_full_->data);
            const uint32_t stride = self->egg_full_->header.stride / 4;
            for (int y = 0; y < kLogoH; ++y)
                for (int x = 0; x < 320; ++x) {
                    const float c = self->egg_cov_[y * 320 + x];
                    px[y * stride + x] = c <= 0.f ? 0u : (static_cast<uint32_t>(std::min(c, 1.f) * 255.f + .5f) << 24) | (kWhite & 0xFFFFFFu);
                }
            lv_image_cache_drop(self->egg_full_);
            lv_obj_invalidate(self->egg_);
        }
        const float want = egg_qr_target(self->egg_view_);
        if (std::fabs(want - self->egg_qr_x_) > 0.5f) {
            self->egg_qr_x_ += (want - self->egg_qr_x_) * 0.22f;
            if (self->egg_) lv_obj_invalidate(self->egg_);
        } else if (self->egg_qr_x_ != want) {
            self->egg_qr_x_ = want;
            if (self->egg_) lv_obj_invalidate(self->egg_);
        }
    }, 16, this);
    set_key_layer(0);
    sound_->play(Sfx::Swoosh);
}

void VelvetUI::egg_close() {
    if (!egg_) return;
    if (egg_timer_) { lv_timer_delete(egg_timer_); egg_timer_ = nullptr; }
    lv_obj_delete(egg_);
    egg_ = nullptr;
    sound_->play(Sfx::Swoosh);
}

bool VelvetUI::egg_key(uint32_t key, bool long_pressed) {
    if (long_pressed) return true;
    if (key == LV_KEY_ESC) { egg_close(); return true; }
    if (egg_view_ == 2 && (is_row_up(key) || is_row_down(key))) {   // the bio: GitHub <-> Bitcoin QR
        egg_btc_ = !egg_btc_;
        sound_->play(Sfx::Move);
        lv_obj_invalidate(egg_);
        return true;
    }
    if (is_right(key)) egg_view_ = egg_view_ == 0 ? 1 : egg_view_ == 2 ? 0 : 1;
    else if (is_left(key)) egg_view_ = egg_view_ == 0 ? 2 : egg_view_ == 1 ? 0 : 2;
    else return true;
    sound_->play(Sfx::Move);
    lv_obj_invalidate(egg_);
    return true;
}

void VelvetUI::egg_draw_cb(lv_event_t* e) {
    auto* self = static_cast<VelvetUI*>(lv_event_get_user_data(e));
    lv_layer_t* l = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->egg_, &a);
    const int32_t ox = a.x1, oy = a.y1;
    auto image = [&](lv_draw_buf_t* b, int32_t x, int32_t y, int32_t radius = 0) {
        if (!b) return;
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = b;
        d.clip_radius = radius;
        const lv_area_t r{ox + x, oy + y, ox + x + static_cast<int32_t>(b->header.w) - 1, oy + y + static_cast<int32_t>(b->header.h) - 1};
        lv_draw_image(l, &d, &r);
    };
    if (self->egg_view_ == 1) {   // the whole screen: the Optic Suite loading animation, looping
        image(self->egg_full_, 0, kLogoY);
        const float ph = std::fmod(lv_tick_elaps(self->egg_frame_at_) / 1000.0f, wordmark::kCycle) / wordmark::kCycle;
        const auto su = wordmark::suite(ph);
        const lv_font_t* f = self->f_.mono10;
        const int32_t clip_x = ox + static_cast<int32_t>(std::lround(su.clip_x * kLogoS + kLogoOX));
        const lv_area_t keep = l->_clip_area;
        lv_area_t c = keep;
        c.x1 = std::max(c.x1, clip_x);
        if (c.x1 <= c.x2) {
            l->_clip_area = c;
            const int32_t top = oy + static_cast<int32_t>(std::lround(su.base * kLogoS + kLogoOY)) - (f->line_height - f->base_line);
            for (int i = 0; i < 5; ++i) {
                const char ch[2] = {"suite"[i], 0};
                const int32_t x = ox + static_cast<int32_t>(std::lround((su.x + i * su.advance) * kLogoS + kLogoOX));
                nt_text(l, f, kWhite, ch, x, top);
            }
            l->_clip_area = keep;
        }
        return;
    }
    // the Optic card beside the QR (it slides out of the way for the bio)
    const float qx = self->egg_qr_x_;
    const float t = std::clamp((qx - egg_qr_target(0)) / (egg_qr_target(2) - egg_qr_target(0)), 0.f, 1.f);
    if (t < 1) {
        // a framed card with rounded corners, centred in the space right of the QR: the eye, OPTIC, suite
        const int32_t qr_right = static_cast<int32_t>(egg_qr_target(0)) + egg_qr_size();
        const int32_t cw = 96, ch = 140, pad = 3;
        const int32_t cx = qr_right + (320 - qr_right - cw) / 2 + static_cast<int32_t>(t * 170), cy = (170 - ch) / 2 - 2;
        lv_draw_rect_dsc_t fr;
        lv_draw_rect_dsc_init(&fr);
        fr.radius = 14;
        fr.bg_color = C(kDark);
        fr.border_color = C(kWhite);
        fr.border_width = 2;
        const lv_area_t fa{ox + cx - pad, oy + cy - pad, ox + cx + cw + pad - 1, oy + cy + ch + pad - 1};
        lv_draw_rect(l, &fr, &fa);
        draw_optic_eye(l, 0, 0, 74, ox + cx + (cw - 74) / 2, oy + cy + 30, C(kWhite));
        nt_text(l, self->f_.big, kWhite, "OPTIC", ox + cx + (cw - 50) / 2, oy + cy + 82);
        nt_text(l, self->f_.mono10, kGrey, "suite", ox + cx + (cw - 40) / 2, oy + cy + 104);
    }
    // the QR: the GitHub page, or (in the bio, up / down) the Bitcoin address for donations
    const bool btc = self->egg_view_ == 2 && self->egg_btc_;
    const char* const* qr = btc ? kEggBtc : kEggQr;
    const int qn = btc ? kEggBtcN : kEggQrN;
    const int32_t qs = egg_qr_size(), qy = (170 - qs) / 2 - 8;
    nt_box(l, ox + static_cast<int32_t>(qx), oy + qy, qs, qs, 0xffffff, 6);
    // the code centred on the card, its middle left blank for the logo (level H covers for it)
    const int cut = qn <= 33 ? 9 : 11, c0 = (qn - cut) / 2;
    const int32_t mx = ox + static_cast<int32_t>(qx) + (qs - qn * kQrScale) / 2, my = oy + qy + (qs - qn * kQrScale) / 2;
    for (int y = 0; y < qn; ++y)
        for (int x = 0; x < qn; ++x)
            if (qr[y][x] == '1' && !(x >= c0 && x < c0 + cut && y >= c0 && y < c0 + cut))
                nt_box(l, mx + x * kQrScale, my + y * kQrScale, kQrScale, kQrScale, 0x000000, 0);
    const int32_t lc = cut * kQrScale, lx = mx + c0 * kQrScale, ly = my + c0 * kQrScale;   // the logo's square
    if (btc) {   // Bitcoin: the orange coin with a white B and its two strokes
        nt_box(l, lx + 1, ly + 1, lc - 2, lc - 2, 0xF7931A);
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.font = self->f_.big;
        d.color = lv_color_hex(0xffffff);
        d.text = "B";
        const int32_t bx = lx + (lc - 10) / 2 + 1, by = ly + (lc - self->f_.big->line_height) / 2;
        const lv_area_t ba{bx, by, bx + 12, by + self->f_.big->line_height};
        lv_draw_label(l, &d, &ba);
        nt_box(l, bx + 3, by + 1, 2, 3, 0xffffff, 0);
        nt_box(l, bx + 3, by + self->f_.big->line_height - 4, 2, 3, 0xffffff, 0);
    } else {     // GitHub: the Optic eye
        draw_optic_eye(l, 0, 0, static_cast<float>(lc - 2), lx + 1, ly + (lc - (lc - 2) * 4 / 7) / 2, lv_color_hex(0x000000));
    }
    const char* cap = btc ? "Bitcoin" : "zedward856-spec";
    nt_text(l, self->f_.mono10, kGrey, cap, ox + static_cast<int32_t>(qx) + (qs - static_cast<int32_t>(std::strlen(cap)) * 8) / 2, oy + qy + qs + 3);
    // the bio, coming in from the left as the QR leaves
    if (t > 0) {
        const int32_t bx = ox + 8 - static_cast<int32_t>((1 - t) * 180);
        // the name, and the Optic eye big in the space right of it
        nt_text(l, self->f_.big, kWhite, "sl01220", bx, oy + 8);
        draw_optic_eye(l, 0, 0, 46, bx + 82, oy + 4, C(kWhite));
        // (21 columns: the QR stands at x 180)
        const char* const kBio[] = {
            "Solo developer.", "A Taiwanese high-", "school freshman in", "love with technology,", "IoT and 2B.",
            "Instagram @sl01220", "Reddit    u/sl01220", self->egg_btc_ ? "Donate    Bitcoin <" : "Donate    Bitcoin v"};
        for (int i = 0; i < 8; ++i)
            nt_text(l, self->f_.mono10, i < 5 ? kWhite : kGrey, kBio[i], bx, oy + 34 + i * 15 + (i >= 5 ? 6 : 0));
    }
}

// ---------------------------------------------------------------- tutorial
namespace {
struct TutPage {
    const char* title;
    const char* body;
};
const TutPage kTut[] = {
    {"Welcome",
     "Your CardputerZero is a pocket OS: terminal, browser, music, camera, sensors, Wi-Fi and "
     "settings in one app. This tour shows how to use it. It takes a minute."},
    {"Move",
     "F Z X C are the arrows: F up, X down, Z left, C right. OK opens. ESC goes back. On the home "
     "list, hold ESC to quit."},
    {"FN + SYM",
     "fn turns on the orange keys: arrows, F1-F12, PgUp/PgDn. SYM turns on the blue symbols. A "
     "badge shows in the corner. Tap again, or press a key that isn't on it, to turn it off."},
    {"Apps",
     "Terminal: real shell (hold ESC: leave, F1: compact). Browser: full-screen pages, TAB: "
     "toolbar. Camera: OK = photo. Network: Wi-Fi, Ethernet, Tor, Tailscale, speed test, tools."},
    {"System",
     "Brightness, volume, sound, theme and power. Tutorial: OK opens this tour, left/right turns "
     "it on or off at start."},
    {"Ready?", "That's the tour. Show it again when your CardputerZero starts?"},
};
constexpr int kTutPages = sizeof(kTut) / sizeof(kTut[0]);
}  // namespace

void VelvetUI::tutorial_open() {
    if (tut_ || transitioning_) return;
    tutorial_pending_ = false;
    if (hub_prompt_) lv_obj_add_flag(hub_prompt_, LV_OBJ_FLAG_HIDDEN);
    if (hub_hints_) lv_obj_remove_flag(hub_hints_, LV_OBJ_FLAG_HIDDEN);
    sound_->play(Sfx::Swoosh);  // just the slide: nothing layered on top
    transition(true, [](VelvetUI* self) {
        self->tut_ = plain(self->root_);
        lv_obj_set_size(self->tut_, 320, 170);
        lv_obj_set_style_bg_color(self->tut_, C(kBg), 0);
        lv_obj_set_style_bg_opa(self->tut_, LV_OPA_COVER, 0);
        self->tut_page_ = 0;
        self->tut_sel_ = 0;
        self->tutorial_render(0);
    });
}

void VelvetUI::tutorial_close(bool never_again) {
    if (!tut_ || transitioning_) return;
    if (never_again) {
        tutorial_at_start_ = false;
        save_setting("tutorial", 0);
    }
    sound_->play(Sfx::Swoosh);  // just the slide: nothing layered on top
    transition(false, [](VelvetUI* self) {
        lv_obj_delete(self->tut_);
        self->tut_ = nullptr;
        self->tut_body_ = nullptr;
        if (self->page_kind_ == Page::System) self->system_render();
    });
}

// Rebuilds the overlay for tut_page_. dir: +1 came from the left page, -1 from the right.
void VelvetUI::tutorial_render(int dir) {
    lv_obj_clean(tut_);
    lv_obj_t* t = tut_;
    const TutPage& pg = kTut[tut_page_];
    const bool last = tut_page_ == kTutPages - 1;

    text_pill(t, pg.title, f_.mono10, kDark, kWhite, 4, 4, 20, 10);
    char count[24];
    std::snprintf(count, sizeof count, "Tutorial %d/%d", tut_page_ + 1, kTutPages);
    lv_obj_t* cnt = text_pill(t, count, f_.mono10, kDark, kGrey, 0, 4, 20, 10);
    lv_obj_set_x(cnt, 316 - lv_obj_get_style_width(cnt, LV_PART_MAIN));

    tut_body_ = label(t, pg.body, f_.mono12, kWhite, 12, 32);
    lv_label_set_long_mode(tut_body_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(tut_body_, 296);
    LV_UNUSED(dir);

    if (last) {
        static const char* const kOpts[] = {"Got it", "Never show again"};
        for (int i = 0; i < 2; ++i) {
            const bool sel = i == tut_sel_;
            lv_obj_t* plate = pill(t, 4, 76 + i * 26, 312, 22, kWhite);
            if (!sel) lv_obj_set_style_bg_opa(plate, LV_OPA_TRANSP, 0);
            label(plate, kOpts[i], f_.mono12, sel ? kInk : kWhite, 12, 3);
        }
    }

    // page dots: the current one a wide white pill
    int32_t x = 10;
    for (int i = 0; i < kTutPages; ++i) {
        const bool cur = i == tut_page_;
        pill(t, x, 156, cur ? 16 : 6, 6, cur ? kWhite : kDark);
        x += (cur ? 16 : 6) + 5;
    }
    if (last) hints_r(t, 316, 151, f_.mono10, {{"\xE2\x86\x91\xE2\x86\x93", "Pick"}, {"OK", "Choose"}});
    else hints_r(t, 316, 151, f_.mono10, {{"\xE2\x86\x90\xE2\x86\x92", "Page"}, {"ESC", "Close"}});
}

bool VelvetUI::tutorial_key(uint32_t key, bool long_pressed) {
    if (transitioning_ || long_pressed) return true;
    const bool last = tut_page_ == kTutPages - 1;
    if (key == LV_KEY_ESC) {
        tutorial_close(false);
    } else if (last && (is_row_up(key) || is_row_down(key))) {
        tut_sel_ ^= 1;
        sound_->play(Sfx::Move);
        tutorial_render(0);
    } else if (last && is_enter(key)) {
        tutorial_close(tut_sel_ == 1);
    } else if (!last && (is_right(key) || is_enter(key) || is_row_down(key))) {
        ++tut_page_;
        tut_sel_ = 0;
        sound_->play(Sfx::Move);
        tutorial_render(1);
    } else if (tut_page_ > 0 && (is_left(key) || (!last && is_row_up(key)))) {
        --tut_page_;
        sound_->play(Sfx::Move);
        tutorial_render(-1);
    }
    return true;  // the tour owns every key while it's open
}

// ---------------------------------------------------------------- input
// The device's one volume (the sound server's, shared with M5's launcher). Nothing is restarted,
// so music keeps playing. Without a mixer (the PC demo) the player's own volume is used.
void VelvetUI::global_volume(int delta) {
    const int v = device_->volume();
    if (v < 0) { if (music_) music_->volume(delta); return; }
    if (delta == 0) { gvol_ = v; return; }                       // only read it
    gvol_ = std::max(0, std::min(100, (v + delta + 5) / 10 * 10));   // in steps of 10, like the launcher
    device_->set_volume(gvol_);
}
void VelvetUI::global_mute() {
    const int v = device_->volume();
    if (v < 0) return;
    if (v > 0) { gvol_unmuted_ = v; gvol_ = 0; } else gvol_ = gvol_unmuted_;
    device_->set_volume(gvol_);
}

bool VelvetUI::handle_key(uint32_t key, bool long_pressed) {
    // Hold Backspace / Delete: after 400 ms it repeats every 45 ms until let go.
    // (Not in the browser's tab list, where DEL closes a tab.)
    if (!long_pressed && !rep_firing_ && (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) &&
        !(page_kind_ == Page::Browser && blist_open_)) {
        rep_stop();
        rep_key_ = key;
        rep_timer_ = lv_timer_create(
            [](lv_timer_t* t) {
                auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
                lv_timer_set_period(t, 45);
                self->rep_firing_ = true;
                self->handle_key(self->rep_key_, false);
                self->rep_firing_ = false;
            },
            400, this);
    } else if (!long_pressed && !rep_firing_ && rep_key_ && key != rep_key_) {
        rep_stop();  // another key: the held one is no longer the last pressed
    }
    if (!long_pressed && eco_wake()) return true;   // ECO: the screen was off, this key only wakes it
    if (key == kKeyVolUp || key == kKeyVolDown || key == kKeyMute) {   // fn+D / fn+S / fn+A, on every page
        // On M5's system the desktop under the launcher (labwc) acts on these keys itself, 5 % a
        // press: then this only shows the new value (and writes it where the launcher keeps it).
        // With no such desktop, the key is acted on here.
        if (!long_pressed) {
            static const bool desktop_does_it = std::system("pgrep -x labwc >/dev/null 2>&1") == 0;
            volkey_ = key;
            if (!desktop_does_it) {
                if (key == kKeyMute) global_mute(); else global_volume(key == kKeyVolUp ? 10 : -10);
            }
            if (volkey_timer_) lv_timer_delete(volkey_timer_);
            volkey_timer_ = lv_timer_create(
                [](lv_timer_t* t) {
                    auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
                    lv_timer_delete(t);
                    self->volkey_timer_ = nullptr;
                    const int v = self->device_->volume();
                    if (v >= 0) { self->gvol_ = v; self->device_->set_volume(v); }
                    if (self->page_kind_ == Page::Music && !self->mus_vid_) self->music_render();
                    else if (self->page_kind_ == Page::System) self->system_render();
                },
                desktop_does_it ? 500 : 30, this);
        }
        return true;
    }
    if (splash_) {  // any key skips the boot animation
        if (!long_pressed) splash_skip();
        return true;
    }
    if (egg_) return egg_key(key, long_pressed);
    if (tut_) return tutorial_key(key, long_pressed);
    if (!long_pressed) egg_pos_ = 0;   // any ordinary key breaks the sequence
    if (tutorial_pending_ && page_kind_ == Page::Hub && !transitioning_ && !long_pressed) {
        tutorial_open();  // first key press after start: the tour
        return true;
    }
    if (page_kind_ == Page::Browser && !transitioning_) return browser_key(key, long_pressed);
    if (page_kind_ == Page::Gallery && !transitioning_ && gallery_key(key, long_pressed)) return true;
    if (page_kind_ == Page::Music && !transitioning_ && music_key(key, long_pressed)) return true;
    if (page_kind_ == Page::Pwn && !transitioning_ && pwn_key(key, long_pressed)) return true;
    // The terminal gets every key (Esc too: hold Esc to leave it).
    if (page_kind_ == Page::Terminal && !transitioning_ && term_key(key, long_pressed)) return true;
    // The Wi-Fi page owns its keys first (text entry, overlays, long Backspace).
    if (page_kind_ == Page::Wifi && !transitioning_ && wifi_key(key, long_pressed)) return true;
    if (page_kind_ == Page::Net && !transitioning_ && net_key(key, long_pressed)) return true;

    if (key == LV_KEY_ESC && long_pressed) {
        hold_end();
        if (esc_from_hub_ && page_kind_ == Page::Hub) quit_(quit_user_);
        return true;
    }
    if (key == LV_KEY_ESC) esc_from_hub_ = page_kind_ == Page::Hub && !transitioning_;
    if (long_pressed) return true;
    if (transitioning_) return true;

    if (page_kind_ == Page::Hub) {
        if (is_up(key)) hub_select(hub_index_ - 1, true);
        else if (is_down(key)) hub_select(hub_index_ + 1, true);
        else if (is_enter(key)) open_page(kHubPages[hub_index_]);
        else if (key == LV_KEY_ESC) {
            sound_->play(Sfx::ToggleOff);
            hold_begin("Quit");
        }
        else return false;
        return true;
    }

    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
        close_page();
        return true;
    }
    switch (page_kind_) {
        case Page::Stats:
            if (is_up(key)) stats_select(list_index_ - 1);
            else if (is_down(key)) stats_select(list_index_ + 1);
            break;
        case Page::System:
            if (is_left(key)) system_adjust(-10);  // z / c here; f / x pick the row
            else if (is_right(key)) system_adjust(10);
            else if (is_enter(key) && sys_sel_ >= 2) system_adjust(0);
            else if (is_row_up(key)) {
                sys_step(-1);
                sound_->play(Sfx::Move);
                system_render();
            } else if (is_row_down(key)) {
                sys_step(1);
                sound_->play(Sfx::Move);
                system_render();
            }
            break;
        case Page::Camera:
            if (is_enter(key)) camera_snap();
            break;
        case Page::Hdmi:
            if (is_enter(key) || is_left(key) || is_right(key)) hdmi_toggle();
            break;
        case Page::Bluetooth:
            bt_key(key);
            break;
        default: break;
    }
    return true;
}


// ---------------------------------------------------------------- gallery
namespace {
constexpr int kGalCols = 4, kGalRows = 2, kGalTW = 72, kGalTH = 54;
constexpr int32_t kGalX0 = 9, kGalY0 = 33, kGalDX = 78, kGalDY = 60;

std::string read_file(const std::string& path) {
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[1 << 16];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

// A camera JPEG carries a small preview in its EXIF block (rpicam-still writes one): decoding
// that is ~100x faster than the 8 MP photo. Returns "" if there isn't one.
std::string exif_thumb(const std::string& j) {
    if (j.size() < 8 || (unsigned char)j[0] != 0xFF || (unsigned char)j[1] != 0xD8) return {};
    size_t i = 2;
    while (i + 4 < j.size() && (unsigned char)j[i] == 0xFF) {
        const unsigned m = (unsigned char)j[i + 1];
        const size_t len = ((unsigned char)j[i + 2] << 8) | (unsigned char)j[i + 3];
        if (m == 0xE1) {  // APP1 (EXIF): look for an embedded JPEG inside it
            const size_t end = std::min(j.size(), i + 2 + len);
            for (size_t k = i + 4; k + 1 < end; ++k) {
                if ((unsigned char)j[k] == 0xFF && (unsigned char)j[k + 1] == 0xD8) {
                    for (size_t e = k + 2; e + 1 < end; ++e)
                        if ((unsigned char)j[e] == 0xFF && (unsigned char)j[e + 1] == 0xD9) return j.substr(k, e + 2 - k);
                }
            }
            return {};
        }
        if (m == 0xDA) break;  // image data starts: no EXIF
        i += 2 + len;
    }
    return {};
}

bool is_photo(const std::string& n) {
    std::string e = n.substr(n.find_last_of('.') == std::string::npos ? n.size() : n.find_last_of('.'));
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == ".jpg" || e == ".jpeg" || e == ".png";
}
}  // namespace

void VelvetUI::build_gallery(lv_obj_t* p) {
    build_page_chrome(p, "Gallery");
    // newest first
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, std::string>> found;
    for (const auto& e : fs::directory_iterator(photos_dir(), ec)) {
        if (e.is_regular_file(ec) && is_photo(e.path().filename().string())) found.push_back({e.last_write_time(ec), e.path().string()});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    gal_files_.clear();
    for (auto& f : found) gal_files_.push_back(f.second);
    gal_sel_ = std::min(gal_sel_, std::max(0, static_cast<int>(gal_files_.size()) - 1));
    gal_top_ = 0;

    gal_count_ = label(p, "", f_.mono10, kFaint, 8, 152);
    // the selection: a white rounded frame around the thumbnail
    gal_sel_box_ = pill(p, 0, 0, kGalTW + 8, kGalTH + 8, kWhite, 6);
    for (int i = 0; i < kGalCols * kGalRows; ++i) {
        lv_obj_t* cell = plain(p);
        lv_obj_set_size(cell, kGalTW, kGalTH);
        lv_obj_set_style_bg_color(cell, C(kDark), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(cell, C(kWhite), 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_t* img = lv_image_create(cell);
        lv_obj_center(img);
        gal_cells_.push_back(cell);
        gal_imgs_.push_back(img);
    }
    if (gal_files_.empty()) {
        lv_obj_add_flag(gal_sel_box_, LV_OBJ_FLAG_HIDDEN);
        for (auto* c : gal_cells_) lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        gal_empty_ = label(p, "No photos yet", f_.big, kWhite, 16, 64);
        label(p, "Take one in Camera (OK = snap)", f_.mono10, kGrey, 16, 88);
    } else {
        hints_r(p, 316, 151, f_.mono10, {{"OK", "View"}, {"\xE2\x86\x90\xE2\x86\x91\xE2\x86\x92\xE2\x86\x93", "Move"}});
    }
    gallery_layout();
    gal_timer_ = lv_timer_create(gallery_tick_cb, 30, this);
}

void VelvetUI::gallery_layout() {
    if (gal_cells_.empty()) return;
    const int n = static_cast<int>(gal_files_.size()), per = kGalCols * kGalRows;
    if (gal_sel_ < gal_top_) gal_top_ = gal_sel_ / kGalCols * kGalCols;
    if (gal_sel_ >= gal_top_ + per) gal_top_ = (gal_sel_ / kGalCols - kGalRows + 1) * kGalCols;
    for (int i = 0; i < per; ++i) {
        const int k = gal_top_ + i;
        lv_obj_t* cell = gal_cells_[i];
        const int32_t x = kGalX0 + (i % kGalCols) * kGalDX, y = kGalY0 + (i / kGalCols) * kGalDY;
        lv_obj_set_pos(cell, x, y);
        if (k >= n) { lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_width(cell, 0, 0);
        auto it = gal_thumbs_.find(gal_files_[k]);
        if (it != gal_thumbs_.end() && it->second) {
            lv_image_set_src(gal_imgs_[i], it->second);
            lv_obj_remove_flag(gal_imgs_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(gal_imgs_[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (k == gal_sel_) lv_obj_set_pos(gal_sel_box_, x - 4, y - 4);
    }
    if (gal_count_) {
        char t[32] = "";
        if (n) std::snprintf(t, sizeof t, "%d/%d", gal_sel_ + 1, n);
        lv_label_set_text(gal_count_, t);
    }
}

void VelvetUI::gallery_tick_cb(lv_timer_t* t) {
    auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
    // the viewer: decode the photo a frame after "LOADING" went up
    if (self->gal_loading_ && self->gal_open_ >= 0) {
        self->gal_loading_ = false;
        const std::string bytes = read_file(self->gal_files_[self->gal_open_]);
        if (self->gal_src_) lv_draw_buf_destroy(self->gal_src_);
        self->gal_src_ = decode_image(bytes, 1600, 1200);
        self->gallery_render();
        return;
    }
    // one missing thumbnail per tick, visible ones first
    const int n = static_cast<int>(self->gal_files_.size()), per = kGalCols * kGalRows;
    int want = -1;
    for (int k = self->gal_top_; k < std::min(n, self->gal_top_ + per) && want < 0; ++k)
        if (!self->gal_thumbs_.count(self->gal_files_[k])) want = k;
    for (int k = 0; k < n && want < 0; ++k)
        if (!self->gal_thumbs_.count(self->gal_files_[k])) want = k;
    if (want < 0) return;
    const std::string bytes = read_file(self->gal_files_[want]);
    std::string small = exif_thumb(bytes);
    lv_draw_buf_t* th = small.empty() ? nullptr : decode_image(small, kGalTW, kGalTH);
    if (!th) th = decode_image(bytes, kGalTW, kGalTH);
    self->gal_thumbs_[self->gal_files_[want]] = th;
    if (!self->gal_view_) self->gallery_layout();
}

void VelvetUI::gallery_open(int index) {
    if (index < 0 || index >= static_cast<int>(gal_files_.size())) return;
    gal_open_ = index;
    gal_zoom_ = 1.0f;
    gal_cx_ = gal_cy_ = 0.5f;
    if (!gal_view_) {
        gal_view_ = plain(page_);
        lv_obj_set_size(gal_view_, 320, 170);
        lv_obj_set_style_bg_color(gal_view_, C(kInk), 0);
        lv_obj_set_style_bg_opa(gal_view_, LV_OPA_COVER, 0);
        if (!gal_out_) gal_out_ = lv_draw_buf_create(320, 170, LV_COLOR_FORMAT_ARGB8888, 0);
        gal_view_img_ = lv_image_create(gal_view_);
        lv_obj_set_pos(gal_view_img_, 0, 0);
        lv_obj_t* bar = pill(gal_view_, 2, 151, 316, 18, kDark);
        gal_info_ = label(bar, "", f_.mono10, kWhite, 8, 2);
    }
    if (gal_src_) { lv_draw_buf_destroy(gal_src_); gal_src_ = nullptr; }
    lv_label_set_text(gal_info_, "Loading...");
    lv_obj_add_flag(gal_view_img_, LV_OBJ_FLAG_HIDDEN);
    gal_loading_ = true;
}

void VelvetUI::gallery_close_view() {
    if (gal_view_) lv_obj_delete(gal_view_);
    gal_view_ = gal_view_img_ = gal_info_ = nullptr;
    if (gal_src_) { lv_draw_buf_destroy(gal_src_); gal_src_ = nullptr; }
    gal_open_ = -1;
    gal_loading_ = false;
    gallery_layout();
}

// The open photo, fitted to the screen times the zoom, around (cx, cy); averaged when shrunk.
void VelvetUI::gallery_render() {
    if (!gal_view_ || !gal_out_) return;
    const std::string name = gal_files_[gal_open_].substr(gal_files_[gal_open_].find_last_of('/') + 1);
    char info[160];
    if (!gal_src_) {
        std::snprintf(info, sizeof info, "Can't open %.60s", name.c_str());
        lv_label_set_text(gal_info_, info);
        return;
    }
    const int w = static_cast<int>(gal_src_->header.w), h = static_cast<int>(gal_src_->header.h);
    const uint32_t sstride = gal_src_->header.stride / 4, ostride = gal_out_->header.stride / 4;
    const auto* src = reinterpret_cast<const uint32_t*>(gal_src_->data);
    auto* out = reinterpret_cast<uint32_t*>(gal_out_->data);
    const float fit = std::min(320.0f / w, 170.0f / h), s = fit * gal_zoom_;
    // keep the picture on screen: no panning past its edges
    const float hw = 160.0f / (s * w), hh = 85.0f / (s * h);
    gal_cx_ = hw >= 0.5f ? 0.5f : std::clamp(gal_cx_, hw, 1.0f - hw);
    gal_cy_ = hh >= 0.5f ? 0.5f : std::clamp(gal_cy_, hh, 1.0f - hh);
    const float ox = gal_cx_ * w, oy = gal_cy_ * h;
    const int box = s < 1.0f ? std::min(6, static_cast<int>(1.0f / s + 0.5f)) : 1;
    for (int y = 0; y < 170; ++y) {
        for (int x = 0; x < 320; ++x) {
            const float fx = (x + 0.5f - 160.0f) / s + ox, fy = (y + 0.5f - 85.0f) / s + oy;
            const int sx = static_cast<int>(fx), sy = static_cast<int>(fy);
            if (fx < 0 || fy < 0 || sx >= w || sy >= h) { out[y * ostride + x] = 0xFF000000u | kInk; continue; }
            if (box == 1) { out[y * ostride + x] = src[sy * sstride + sx] | 0xFF000000u; continue; }
            uint32_t r = 0, g = 0, b = 0, cnt = 0;
            for (int dy = 0; dy < box; ++dy) {
                const int yy = sy + dy - box / 2;
                if (yy < 0 || yy >= h) continue;
                for (int dx = 0; dx < box; ++dx) {
                    const int xx = sx + dx - box / 2;
                    if (xx < 0 || xx >= w) continue;
                    const uint32_t c = src[yy * sstride + xx];
                    r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255; ++cnt;
                }
            }
            if (!cnt) cnt = 1;
            out[y * ostride + x] = 0xFF000000u | ((r / cnt) << 16) | ((g / cnt) << 8) | (b / cnt);
        }
    }
    lv_image_cache_drop(gal_out_);
    lv_image_set_src(gal_view_img_, gal_out_);
    lv_obj_remove_flag(gal_view_img_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(gal_view_img_);
    // 53 columns: count, a short name, zoom, what the keys do
    std::string shortname = name.size() > 18 ? name.substr(0, 8) + "~" + name.substr(name.size() - 9) : name;
    std::snprintf(info, sizeof info, "%d/%d %s %d%% %s", gal_open_ + 1, static_cast<int>(gal_files_.size()),
                  shortname.c_str(), static_cast<int>(gal_zoom_ * 100 + 0.5f), gal_zoom_ > 1.01f ? "FZXC move" : "Z/C next");
    lv_label_set_text(gal_info_, info);
}

bool VelvetUI::gallery_key(uint32_t key, bool long_pressed) {
    if (long_pressed) return false;
    const int n = static_cast<int>(gal_files_.size());
    if (gal_view_) {
        if (key == LV_KEY_ESC) { gallery_close_view(); sound_->play(Sfx::Swoosh); return true; }
        if (gal_loading_) return true;
        // zoom: SYM+E / SYM+R ('+' / '-'), also '=' / '_'; OK flips between fitted and 2.5x
        if (key == '+' || key == '=' || key == '-' || key == '_' || is_enter(key)) {
            if (key == '+' || key == '=') gal_zoom_ = std::min(8.0f, gal_zoom_ * 1.5f);
            else if (key == '-' || key == '_') gal_zoom_ = std::max(1.0f, gal_zoom_ / 1.5f);
            else gal_zoom_ = gal_zoom_ > 1.01f ? 1.0f : 2.5f;
            sound_->play(Sfx::Move);
            gallery_render();
            return true;
        }
        const bool zoomed = gal_zoom_ > 1.01f;
        const float step = 60.0f / (std::min(320.0f / gal_src_w(), 170.0f / gal_src_h()) * gal_zoom_);
        if (is_left(key) || is_right(key)) {
            if (zoomed) gal_cx_ += (is_left(key) ? -step : step) / gal_src_w();
            else if (n > 1) { gallery_open((gal_open_ + (is_left(key) ? n - 1 : 1)) % n); gal_sel_ = gal_open_; sound_->play(Sfx::Move); return true; }
            gallery_render();
            return true;
        }
        if (zoomed && (is_row_up(key) || is_row_down(key))) {
            gal_cy_ += (is_row_up(key) ? -step : step) / gal_src_h();
            gallery_render();
            return true;
        }
        return true;  // the viewer keeps every other key
    }
    if (!n) return false;
    int sel = gal_sel_;
    if (is_left(key)) sel -= 1;
    else if (is_right(key)) sel += 1;
    else if (is_row_up(key)) sel -= kGalCols;
    else if (is_row_down(key)) sel += kGalCols;
    else if (is_enter(key)) { gallery_open(gal_sel_); sound_->play(Sfx::Swoosh); return true; }
    else return false;
    sel = std::clamp(sel, 0, n - 1);
    if (sel != gal_sel_) { gal_sel_ = sel; sound_->play(Sfx::Move); gallery_layout(); }
    return true;
}


// ---------------------------------------------------------------- music
namespace {
constexpr int kMusRows = 4;
constexpr int32_t kMusRowY = 52, kMusRowH = 17;
}  // namespace

void VelvetUI::build_music(lv_obj_t* p) {
    build_page_chrome(p, "Music");
    if (!music_) music_ = make_music();
    std::string why;
    if (!music_ || !music_->available(why)) {
        label(p, music_ ? why.c_str() : "Music plays on the CardputerZero", f_.mono12, kWhite, 14, 70);
        label(p, music_ ? "Then open Music again" : "(yt-dlp + mpv, YouTube audio)", f_.mono10, kGrey, 14, 92);
        return;
    }
    // search box: a white pill while typing, dark otherwise
    mus_box_ = pill(p, 4, 29, 312, 20, kWhite);
    mus_query_lbl_ = label(mus_box_, "", f_.mono10, kInk, 10, 3);
    // results
    mus_sel_bg_ = pill(p, 4, 0, 312, kMusRowH - 1, kWhite);
    for (int i = 0; i < kMusRows; ++i) {
        mus_rows_.push_back(label(p, "", f_.mono10, kWhite, 12, kMusRowY + i * kMusRowH + 1));
        mus_durs_.push_back(label(p, "", f_.mono10, kGrey, 266, kMusRowY + i * kMusRowH + 1));
    }
    mus_msg_ = label(p, "", f_.mono10, kGrey, 12, kMusRowY + 2);
    // now playing: title, then a pill track with a white pill fill
    mus_now_ = label(p, "", f_.bold11, kWhite, 8, 121);
    pill(p, 8, 140, 214, 6, kDark);
    mus_fill_ = pill(p, 8, 140, 6, 6, kWhite);
    mus_time_ = label(p, "", f_.mono10, kGrey, 228, 136);
    label(p, "OK play V video D save M mine DEL stop", f_.mono10, kFaint, 8, 153);
    if (mus_results_.empty() || mus_lib_) { mus_results_ = music_->library(); mus_lib_ = true; }   // your files, offline
    mus_typing_ = mus_results_.empty() && music_->online();
    mus_timer_ = lv_timer_create(music_tick_cb, 40, this);
    global_volume(0);   // read the device's volume once, for the display
    music_render();
}

void VelvetUI::music_render() {
    if (!mus_box_ || !music_) return;
    const Music::Status st = mus_status();
    std::string q = mus_query_;
    if (q.size() > 44) q = q.substr(q.size() - 44);
    if (mus_typing_) q += "_";
    if (!mus_typing_ && mus_lib_) q = "My music (" + std::to_string(mus_results_.size()) + ")  TAB: search YouTube";
    else if (mus_query_.empty() && !mus_typing_) q = "TAB: search YouTube";
    else if (mus_query_.empty()) q = "Type to search YouTube_";
    if (!mus_typing_ && !st.note.empty()) {
        q = font_safe_text(f_.mono10, st.note);   // ASCII after this, so a byte cut is safe
        if (q.size() > 36) q = q.substr(0, 35) + "~";
    }
    if (utf8_len(q) > 37) q = tail_chars(q, 37);
    lv_label_set_text(mus_query_lbl_, q.c_str());
    lv_obj_set_style_bg_color(mus_box_, C(mus_typing_ ? kWhite : kDark), 0);
    lv_obj_set_style_text_color(mus_query_lbl_, C(mus_typing_ ? kInk : kWhite), 0);

    const int n = static_cast<int>(mus_results_.size());
    if (mus_sel_ < mus_top_) mus_top_ = mus_sel_;
    if (mus_sel_ >= mus_top_ + kMusRows) mus_top_ = mus_sel_ - kMusRows + 1;
    for (int i = 0; i < kMusRows; ++i) {
        const int k = mus_top_ + i;
        if (k >= n) { lv_label_set_text(mus_rows_[i], ""); lv_label_set_text(mus_durs_[i], ""); continue; }
        const Track& t = mus_results_[k];
        std::string title = (t.id == mus_playing_id_ && st.playing ? "> " : "") + t.title;
        if (utf8_len(title) > 31) {   // keep whole characters (the duration sits at x 266)
            size_t cut = 0, chars = 0;
            while (cut < title.size() && chars < 30) { ++chars; ++cut; while (cut < title.size() && (title[cut] & 0xC0) == 0x80) ++cut; }
            title = title.substr(0, cut) + "~";
        }
        lv_label_set_text(mus_rows_[i], font_safe_text(f_.mono10, title).c_str());
        std::string d = t.duration > 0 ? fmt_time(t.duration) : t.local() ? t.channel : "LIVE";
        if (t.local() && t.duration <= 0) for (auto& ch : d) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        lv_label_set_text(mus_durs_[i], d.substr(0, 5).c_str());
        const bool sel = k == mus_sel_ && !mus_typing_;
        lv_obj_set_style_text_color(mus_rows_[i], C(sel ? kInk : (t.id == mus_playing_id_ ? kWhite : kLight)), 0);
        lv_obj_set_style_text_color(mus_durs_[i], C(sel ? kInk : kGrey), 0);
    }
    if (n && !mus_typing_) {
        lv_obj_remove_flag(mus_sel_bg_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(mus_sel_bg_, kMusRowY + (mus_sel_ - mus_top_) * kMusRowH);
    } else {
        lv_obj_add_flag(mus_sel_bg_, LV_OBJ_FLAG_HIDDEN);
    }
    std::string msg;
    if (st.busy) msg = st.playing || !mus_playing_id_.empty() ? "" : "Searching...";
    if (st.busy && st.note.rfind("Getting", 0) == 0) msg = st.note;   // the first time: yt-dlp is being fetched
    if (!st.error.empty()) msg = st.error;
    if (!n && msg.empty()) msg = mus_typing_ ? "ENTER = search  (empty = my music)"
                                 : mus_lib_ ? "No files in ~/Music or SD card/Music yet" : "";
    lv_label_set_text(mus_msg_, msg.c_str());

    std::string now = st.title.empty() ? "Nothing playing" : font_safe_text(f_.bold11, st.title);
    if (st.busy && !st.title.empty() && !st.playing) now = "Loading  " + now;
    else if (st.paused) now = "Paused  " + now;
    if (now.size() > 38) now = now.substr(0, 37) + "~";   // already ASCII (font_safe_text)
    lv_label_set_text(mus_now_, now.c_str());
    char tm[48];
    std::snprintf(tm, sizeof tm, "%s/%s %d%%", fmt_time(st.pos).c_str(), st.dur > 0 ? fmt_time(st.dur).c_str() : "--", st.volume);
    lv_label_set_text(mus_time_, st.playing ? tm : "");
    const int32_t fw = st.playing && st.dur > 0 ? 6 + static_cast<int32_t>(208 * std::min(1.0, st.pos / st.dur)) : 6;
    lv_obj_set_width(mus_fill_, fw);
    if (mus_full_) lv_obj_invalidate(mus_full_);
}

void VelvetUI::music_video_show(bool on) {
    if (on == (mus_vid_ != nullptr) || !page_) return;
    if (!on) {
        lv_obj_delete(mus_vid_);
        mus_vid_ = mus_vid_img_ = mus_vid_bar_ = mus_vid_info_ = nullptr;
        return;
    }
    if (!mus_vid_buf_) mus_vid_buf_ = lv_draw_buf_create(Music::kVW, Music::kVH, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!mus_vid_buf_) return;
    std::memset(mus_vid_buf_->data, 0, mus_vid_buf_->data_size);
    mus_vid_ = plain(page_);
    lv_obj_set_size(mus_vid_, 320, 170);
    lv_obj_set_style_bg_color(mus_vid_, C(kInk), 0);
    lv_obj_set_style_bg_opa(mus_vid_, LV_OPA_COVER, 0);
    mus_vid_img_ = lv_image_create(mus_vid_);
    lv_image_set_src(mus_vid_img_, mus_vid_buf_);
    mus_vid_bar_ = pill(mus_vid_, 2, 2, 316, 18, kDark);
    mus_vid_info_ = label(mus_vid_bar_, "", f_.mono10, kWhite, 8, 2);
    mus_vid_info_at_ = lv_tick_get();
}

// ---- Now Playing: the cover big on the left; title, artist, state and the progress on the right
namespace { constexpr int32_t kArt = 104; }

void VelvetUI::music_full(bool on) {
    if (on == (mus_full_ != nullptr) || !page_) return;
    if (!on) {
        lv_obj_delete(mus_full_);
        mus_full_ = mus_art_img_ = nullptr;
        mus_esc_pending_ = false;
        music_render();
        return;
    }
    mus_full_ = plain(page_);
    lv_obj_set_pos(mus_full_, 0, 26);
    lv_obj_set_size(mus_full_, 320, 144);
    lv_obj_set_style_bg_color(mus_full_, C(kBg), 0);
    lv_obj_set_style_bg_opa(mus_full_, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(mus_full_, music_full_draw_cb, LV_EVENT_DRAW_MAIN, this);
    // the cover in a rounded frame
    lv_obj_t* frame = plain(mus_full_);
    lv_obj_set_pos(frame, 8, 6);
    lv_obj_set_size(frame, kArt, kArt);
    lv_obj_set_style_radius(frame, 10, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_style_bg_color(frame, C(kDark), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    if (!mus_art_buf_) mus_art_buf_ = lv_draw_buf_create(kArt, kArt, LV_COLOR_FORMAT_ARGB8888, 0);
    mus_art_img_ = lv_image_create(frame);
    lv_image_set_src(mus_art_img_, mus_art_buf_);
    lv_obj_set_pos(mus_art_img_, 0, 0);
    if (!mus_has_art_) lv_obj_add_flag(mus_art_img_, LV_OBJ_FLAG_HIDDEN);
}

// the cover: cropped to a square from its middle, then scaled into kArt x kArt
void VelvetUI::music_set_art(const std::string& bytes) {
    mus_has_art_ = false;
    lv_draw_buf_t* src = bytes.empty() ? nullptr : decode_image(bytes, 640, 640);
    if (src) {
        if (!mus_art_buf_) mus_art_buf_ = lv_draw_buf_create(kArt, kArt, LV_COLOR_FORMAT_ARGB8888, 0);
        const int w = src->header.w, h = src->header.h, side = std::min(w, h);
        const int x0 = (w - side) / 2, y0 = (h - side) / 2;
        const auto* in = reinterpret_cast<const uint32_t*>(src->data);
        auto* out = reinterpret_cast<uint32_t*>(mus_art_buf_->data);
        const uint32_t is = src->header.stride / 4, os = mus_art_buf_->header.stride / 4;
        for (int y = 0; y < kArt; ++y)
            for (int x = 0; x < kArt; ++x) {
                // a small box average (the cover is bigger than the frame)
                const int sx0 = x0 + x * side / kArt, sx1 = std::max(sx0 + 1, x0 + (x + 1) * side / kArt);
                const int sy0 = y0 + y * side / kArt, sy1 = std::max(sy0 + 1, y0 + (y + 1) * side / kArt);
                uint32_t r = 0, g = 0, b = 0, n = 0;
                for (int sy = sy0; sy < sy1; ++sy)
                    for (int sx = sx0; sx < sx1; ++sx) {
                        const uint32_t p = in[sy * is + sx];
                        r += (p >> 16) & 0xFF; g += (p >> 8) & 0xFF; b += p & 0xFF; ++n;
                    }
                out[y * os + x] = 0xFF000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
            }
        lv_draw_buf_destroy(src);
        lv_image_cache_drop(mus_art_buf_);
        mus_has_art_ = true;
    }
    if (mus_art_img_) {
        if (mus_has_art_) lv_obj_remove_flag(mus_art_img_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(mus_art_img_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(mus_art_img_);
    }
}

void VelvetUI::music_full_draw_cb(lv_event_t* e) {
    auto* self = static_cast<VelvetUI*>(lv_event_get_user_data(e));
    if (!self->music_ || !self->mus_full_) return;
    lv_layer_t* l = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->mus_full_, &a);
    const int32_t ox = a.x1, oy = a.y1 - 26;   // page coordinates
    const Fonts& f = self->f_;
    const Music::Status st = self->mus_status();
    // no cover: a big note in the frame
    if (!self->mus_has_art_) {
        nt_box(l, ox + 66, oy + 52, 4, 46, kGrey, 0);
        nt_box(l, ox + 66, oy + 52, 20, 4, kGrey, 0);
        nt_box(l, ox + 46, oy + 88, 24, 20, kGrey);
    }
    // the track: which one in the list, its title (two lines), the artist
    const Track* t = nullptr;
    int idx = -1;
    for (size_t i = 0; i < self->mus_results_.size(); ++i)
        if (self->mus_results_[i].id == self->mus_playing_id_) { t = &self->mus_results_[i]; idx = static_cast<int>(i); }
    const char* state = (st.busy && !st.playing) || st.loading ? "LOADING" : st.paused ? "PAUSED" : st.playing ? "NOW PLAYING" : "STOPPED";
    nt_text(l, f.mono10, kGrey, state, ox + 124, oy + 32);
    if (idx >= 0) {
        char b[24];
        std::snprintf(b, sizeof b, "%d/%d", idx + 1, static_cast<int>(self->mus_results_.size()));
        nt_text_r(l, f.mono10, kFaint, b, ox + 312, oy + 32);
    }
    std::string title = font_safe_text(f.mono12, st.title.empty() && t ? t->title : st.title);
    constexpr size_t kCols = 23;
    std::string l1 = title, l2;
    if (title.size() > kCols) {   // wrap at a space, the rest on line two
        size_t cut = title.rfind(' ', kCols);
        if (cut == std::string::npos || cut < 8) cut = kCols;
        l1 = title.substr(0, cut);
        l2 = title.substr(cut + (title[cut] == ' ' ? 1 : 0));
        if (l2.size() > kCols) l2 = l2.substr(0, kCols - 1) + "~";
    }
    nt_text(l, f.mono12, kWhite, l1, ox + 124, oy + 48);
    nt_text(l, f.mono12, kWhite, l2, ox + 124, oy + 65);
    if (t) nt_text(l, f.mono10, kGrey, nt_fit(font_safe_text(f.mono10, t->local() ? t->channel : t->channel), 23), ox + 124, oy + 86);
    char vb[24];
    std::snprintf(vb, sizeof vb, "Volume %d%%", st.volume);
    nt_text(l, f.mono10, kGrey, vb, ox + 124, oy + 104);
    // the progress: times above, the bar under everything
    nt_text(l, f.mono10, kWhite, fmt_time(st.pos), ox + 124, oy + 122);
    nt_text_r(l, f.mono10, kGrey, st.dur > 0 ? fmt_time(st.dur) : "--:--", ox + 312, oy + 122);
    nt_box(l, ox + 8, oy + 142, 304, 4, kDark);
    if (st.dur > 0) nt_box(l, ox + 8, oy + 142, std::max<int32_t>(4, static_cast<int32_t>(304 * std::min(1.0, st.pos / st.dur))), 4, kWhite);
    nt_text(l, f.mono10, kFaint, "OK pause  Z/C seek  F/X track  ESC list", ox + 4, oy + 152);
}

// The player's status. "ended" is reported once, and the status is read every tick, so it's
// kept here until the timer's next-track check sees it.
Music::Status VelvetUI::mus_status() {
    Music::Status st = music_->status();
    if (st.ended) mus_ended_ = true;
    if (gvol_ >= 0) st.volume = gvol_;   // shown: the device volume
    return st;
}

void VelvetUI::music_tick_cb(lv_timer_t* t) {
    auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
    if (!self->music_) return;
    // video: the newest frame onto the screen every tick; the rest a few times a second
    const bool vid = self->mus_status().video;
    if (vid != (self->mus_vid_ != nullptr)) self->music_video_show(vid);
    if (self->mus_vid_ && self->music_->frame(self->mus_frame_) && self->mus_vid_buf_) {
        auto* px = reinterpret_cast<uint32_t*>(self->mus_vid_buf_->data);
        const uint32_t stride = self->mus_vid_buf_->header.stride / 4;
        for (int y = 0; y < Music::kVH; ++y)
            for (int x = 0; x < Music::kVW; ++x) px[y * stride + x] = self->mus_frame_[y * Music::kVW + x] | 0xFF000000u;
        lv_image_cache_drop(self->mus_vid_buf_);
        lv_obj_invalidate(self->mus_vid_img_);
    }
    if (self->mus_vid_) {   // the title strip shows for a moment after a key, then hides
        const Music::Status st = self->mus_status();
        const bool show = lv_tick_elaps(self->mus_vid_info_at_) < 2500 || st.paused || st.busy;
        if (show) {
            lv_obj_remove_flag(self->mus_vid_bar_, LV_OBJ_FLAG_HIDDEN);
            char b[96];
            std::snprintf(b, sizeof b, "%s %s/%s  %.24s", st.paused ? "Paused" : ">", fmt_time(st.pos).c_str(),
                          st.dur > 0 ? fmt_time(st.dur).c_str() : "--", font_safe_text(self->f_.mono10, st.title).c_str());
            lv_label_set_text(self->mus_vid_info_, b);
        } else {
            lv_obj_add_flag(self->mus_vid_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    std::string art;
    if (self->music_->art(art)) self->music_set_art(art);
    if (++self->mus_tick_n_ % 6) return;
    // the highlighted track, once it has been highlighted for a moment: find its stream ahead of OK
    if (!self->mus_typing_ && self->mus_sel_ >= 0 && self->mus_sel_ < static_cast<int>(self->mus_results_.size())) {
        const Track& sel = self->mus_results_[self->mus_sel_];
        const Music::Status ps = self->mus_status();
        if (sel.id != self->mus_prep_id_) { self->mus_prep_id_ = sel.id; self->mus_prep_n_ = 0; }
        else if (!ps.busy && !ps.loading && ++self->mus_prep_n_ >= 3) self->music_->prepare(sel);
    }
    std::vector<Track> r;
    if (self->music_->take_results(r)) {
        self->mus_results_ = r;
        self->mus_lib_ = false;
        self->mus_sel_ = self->mus_top_ = 0;
        if (!r.empty()) self->mus_typing_ = false;
    }
    // a track finished: the next one in the list
    self->mus_status();
    const bool ended = self->mus_ended_;
    self->mus_ended_ = false;
    if (ended && !self->mus_playing_id_.empty()) {
        for (size_t i = 0; i + 1 < self->mus_results_.size(); ++i) {
            if (self->mus_results_[i].id == self->mus_playing_id_) {
                self->mus_playing_id_ = self->mus_results_[i + 1].id;
                self->music_->play(self->mus_results_[i + 1]);
                break;
            }
        }
    }
    self->music_render();
}

bool VelvetUI::music_key(uint32_t key, bool long_pressed) {
    if (mus_full_ && key == LV_KEY_ESC) {   // Now Playing: a short ESC = the list, held = home
        if (long_pressed) { mus_esc_pending_ = false; hold_end(); music_full(false); close_page(); }
        else { mus_esc_pending_ = true; hold_begin("Home"); }
        return true;
    }
    if (long_pressed || !mus_box_ || !music_) return false;
    if (mus_full_ && !mus_vid_) {
        const int n = static_cast<int>(mus_results_.size());
        int idx = -1;
        for (int i = 0; i < n; ++i) if (mus_results_[i].id == mus_playing_id_) idx = i;
        auto go = [&](int i) {   // another track in the list
            if (i < 0 || i >= n) return;
            mus_sel_ = i;
            mus_playing_id_ = mus_results_[i].id;
            music_->play(mus_results_[i]);
            sound_->play(Sfx::Move);
        };
        if (is_enter(key)) music_->toggle_pause();
        else if (is_left(key)) music_->seek(-10);
        else if (is_right(key)) music_->seek(10);
        else if (is_row_up(key)) go(idx - 1);
        else if (is_row_down(key)) go(idx + 1);
        else if (key == LV_KEY_DEL || key == LV_KEY_BACKSPACE) { music_->stop(); mus_playing_id_.clear(); music_full(false); }
        music_render();
        return true;
    }
    if (mus_vid_) {   // watching: OK pause, Z C 10 s, fn+S/D volume, ESC / DEL stop
        mus_vid_info_at_ = lv_tick_get();
        if (key == LV_KEY_ESC || key == LV_KEY_DEL || key == LV_KEY_BACKSPACE) { music_->stop(); mus_playing_id_.clear(); music_video_show(false); music_render(); }
        else if (is_enter(key)) music_->toggle_pause();
        else if (is_left(key)) music_->seek(-10);
        else if (is_right(key)) music_->seek(10);
        return true;
    }
    if (key == LV_KEY_ESC) return false;          // back to the hub (the music keeps playing)
    if (key == '\t' || key == LV_KEY_NEXT) { mus_typing_ = !mus_typing_ || mus_results_.empty(); music_render(); return true; }
    if (mus_typing_) {
        if (key == LV_KEY_ENTER) {
            if (!mus_query_.empty()) { music_->search(mus_query_); sound_->play(Sfx::Open); }
            else { mus_results_ = music_->library(); mus_lib_ = true; mus_sel_ = mus_top_ = 0; mus_typing_ = false; }
        } else if (key == LV_KEY_BACKSPACE) {
            while (!mus_query_.empty() && (mus_query_.back() & 0xC0) == 0x80) mus_query_.pop_back();
            if (!mus_query_.empty()) mus_query_.pop_back();
        } else if (key == LV_KEY_DOWN && !mus_results_.empty()) {
            mus_typing_ = false;
        } else if (key >= 32 && key < 0x110000 && key != LV_KEY_DEL) {
            // UTF-8
            if (key < 0x80) mus_query_ += static_cast<char>(key);
            else if (key < 0x800) { mus_query_ += static_cast<char>(0xC0 | (key >> 6)); mus_query_ += static_cast<char>(0x80 | (key & 0x3F)); }
            else if (key < 0x10000) { mus_query_ += static_cast<char>(0xE0 | (key >> 12)); mus_query_ += static_cast<char>(0x80 | ((key >> 6) & 0x3F)); mus_query_ += static_cast<char>(0x80 | (key & 0x3F)); }
        }
        music_render();
        return true;
    }
    const int n = static_cast<int>(mus_results_.size());
    if (key == LV_KEY_DEL || key == LV_KEY_BACKSPACE) { music_->stop(); mus_playing_id_.clear(); music_render(); return true; }
    if (is_row_up(key) && n) { mus_sel_ = (mus_sel_ + n - 1) % n; sound_->play(Sfx::Move); music_render(); return true; }
    if (is_row_down(key) && n) { mus_sel_ = (mus_sel_ + 1) % n; sound_->play(Sfx::Move); music_render(); return true; }
    if ((key == 'v' || key == 'V') && n) {
        const Track& t = mus_results_[mus_sel_];
        mus_playing_id_ = t.id;
        if (t.local() && !t.video_file) music_->play(t);   // a song file has no picture
        else music_->play_video(t);
        music_render();
        return true;
    }
    if ((key == 'd' || key == 'D') && n && !mus_results_[mus_sel_].local()) { music_->download(mus_results_[mus_sel_]); sound_->play(Sfx::Open); music_render(); return true; }
    if (key == 'm' || key == 'M') { mus_results_ = music_->library(); mus_lib_ = true; mus_sel_ = mus_top_ = 0; sound_->play(Sfx::Move); music_render(); return true; }
    if (is_left(key)) { music_->seek(-10); music_render(); return true; }
    if (is_right(key)) { music_->seek(10); music_render(); return true; }
    if (is_enter(key) && n) {
        const Track& t = mus_results_[mus_sel_];
        if (!(t.id == mus_playing_id_ && mus_status().playing)) { mus_playing_id_ = t.id; music_->play(t); }
        music_full(true);   // the track fills the page until a short ESC
        music_render();
        return true;
    }
    return true;
}

// ---------------------------------------------------------------- autoshot
namespace {
struct ShotStep {
    uint32_t key;      // 0 = no key
    uint32_t wait_ms;  // wait after the key before the (optional) shot
    const char* shot;  // file stem or nullptr
    const char* text;  // typed one character at a time ('\n' = Enter), or nullptr
};
constexpr uint32_t kGoto = 0x7E000000;     // | page: select that page's hub item
constexpr uint32_t P(Page p) { return static_cast<uint32_t>(p); }
constexpr uint32_t kHoldEsc = 0x7D000001;  // Esc held long enough to count as a hold
constexpr uint32_t kEscDown = 0x7D000002;  // Esc pressed and still held (no release yet)
constexpr uint32_t kEscUp = 0x7D000003;    // ... and let go
constexpr uint32_t kFnLayer = 0x7D000004;  // fn / SYM layer on, layer off (the badge)
constexpr uint32_t kSymLayer = 0x7D000005;
constexpr uint32_t kNoLayer = 0x7D000006;
const ShotStep kScript[] = {
    {0, 1500, "44-boot-title", nullptr},   // boot splash: the eye
    {0, 1700, "45-boot-loading", nullptr}, // OPTIC out, crosshair on the O
    {0, 3000, "01-hub", nullptr},  // first start: PRESS ANY KEY
    {LV_KEY_ENTER, 900, "38-tutorial-welcome", nullptr},
    {'c', 500, "39-tutorial-move", nullptr},
    {'c', 500, "40-tutorial-layers", nullptr},
    {'c', 300, nullptr, nullptr},
    {'c', 300, nullptr, nullptr},
    {'c', 500, "41-tutorial-ready", nullptr},
    {'x', 400, "42-tutorial-never", nullptr},
    {'f', 200, nullptr, nullptr},    // back to GOT IT (doesn't change the setting)
    {LV_KEY_ENTER, 900, "43-hub-after-tour", nullptr},
    {kGoto | P(Page::Sensors), 500, nullptr, nullptr},
    {LV_KEY_DOWN, 90, "02-hub-moving", nullptr},
    {0, 400, "03-hub-stats", nullptr},
    {LV_KEY_ENTER, 150, "04-transition", nullptr},
    {0, 1250, "08-stats", nullptr},
    {LV_KEY_DOWN, 400, "09-stats-sel", nullptr},
    {LV_KEY_ESC, 700, nullptr, nullptr},
    {kGoto | P(Page::System), 400, nullptr, nullptr},  // SYSTEM
    {LV_KEY_ENTER, 2600, "12-system", nullptr},
    {LV_KEY_DOWN, 200, nullptr, nullptr},
    {LV_KEY_LEFT, 500, "13-system-volume", nullptr},
    {LV_KEY_ESC, 700, nullptr, nullptr},
    {kGoto | P(Page::Net), 400, nullptr, nullptr},  // NETWORK
    {LV_KEY_ENTER, 900, "80-net-menu", nullptr},
    {LV_KEY_ENTER, 1900, "14-wifi-list", nullptr},  // Wi-Fi
    {LV_KEY_DOWN, 300, nullptr, nullptr},  // CafeNet: secured, not saved
    {LV_KEY_ENTER, 500, nullptr, nullptr},
    {0, 400, "15-wifi-password", "sunnyDay#4"},
    {LV_KEY_NEXT, 400, "16-wifi-password-shown", nullptr},
    {LV_KEY_ENTER, 700, "17-wifi-connecting", nullptr},
    {0, 1500, "18-wifi-connected", nullptr},
    {0, 2600, "19-wifi-after", nullptr},
    {LV_KEY_ENTER, 500, "20-wifi-actions", nullptr},
    {LV_KEY_ESC, 300, nullptr, nullptr},
    {LV_KEY_DOWN, 400, "20a-wifi-eap-list", nullptr},   // CampusNet: WPA-Enterprise, asks for a login
    {LV_KEY_ENTER, 500, "20b-wifi-eap-form", nullptr},
    {0, 200, nullptr, "s1012345@campus.example"},
    {LV_KEY_DOWN, 200, nullptr, "anonymous@campus.example"},
    {LV_KEY_DOWN, 400, "20c-wifi-eap-filled", "notMyRealPw#7"},
    {LV_KEY_NEXT, 400, "20d-wifi-eap-shown", nullptr},
    {LV_KEY_NEXT, 200, nullptr, nullptr},
    {LV_KEY_ENTER, 300, nullptr, nullptr},              // on to the method row
    {LV_KEY_RIGHT, 400, "20e-wifi-eap-method", nullptr},   // TTLS / PAP
    {LV_KEY_RIGHT, 300, nullptr, nullptr},              // back to PEAP / MSCHAPv2
    {LV_KEY_ENTER, 700, "20f-wifi-eap-connecting", nullptr},
    {0, 1500, "20g-wifi-eap-connected", nullptr},
    {0, 2600, "20h-wifi-eap-after", nullptr},
    {LV_KEY_ESC, 900, "81-net-back", nullptr},   // back in NETWORK
    {LV_KEY_DOWN, 150, nullptr, nullptr},
    {LV_KEY_ENTER, 700, "82-net-eth", nullptr},
    {LV_KEY_ESC, 300, nullptr, nullptr},
    {LV_KEY_DOWN, 150, nullptr, nullptr},
    {LV_KEY_ENTER, 500, "83-net-tor", nullptr},
    {LV_KEY_ESC, 300, nullptr, nullptr},
    {LV_KEY_DOWN, 150, nullptr, nullptr},
    {LV_KEY_ENTER, 600, "84-net-tailscale", nullptr},
    {LV_KEY_ESC, 300, nullptr, nullptr},
    {LV_KEY_DOWN, 150, nullptr, nullptr},
    {LV_KEY_ENTER, 500, "85-net-speed", nullptr},
    {LV_KEY_ESC, 300, nullptr, nullptr},
    {LV_KEY_DOWN, 150, nullptr, nullptr},
    {LV_KEY_ENTER, 500, "86-net-tools", nullptr},
    {LV_KEY_ENTER, 400, "87-net-tool-edit", nullptr},
    {LV_KEY_ENTER, 2600, "88-net-tool-out", nullptr},
    {LV_KEY_ESC, 300, nullptr, nullptr},
    {LV_KEY_ESC, 300, nullptr, nullptr},
    {LV_KEY_ESC, 800, nullptr, nullptr},
    {kGoto | P(Page::Terminal), 300, nullptr, nullptr},
    {kEscDown, 420, "21-quit-hint", nullptr},
    {kEscUp, 200, nullptr, nullptr},
    {0, 1700, "27-hub-terminal", nullptr},
    {LV_KEY_ENTER, 900, "28-terminal", nullptr},
    {0, 600, "29-terminal-neofetch", "neofetch\n"},
    {0, 600, "30-terminal-files", "cd notes\nls -l\ncat lora.txt\n"},
    {0, 400, "31-terminal-complete", "cd ..\nclear\nls\ncat sch\t"},
    {0, 200, nullptr, "\n"},
    {kKeyF1, 400, nullptr, "clear\nneofetch\nls -l\n"},
    {0, 500, "49-terminal-compact", nullptr},
    {kKeyF1, 400, nullptr, nullptr},
    {kEscDown, 420, "50-terminal-hold-esc", nullptr},
    {kHoldEsc, 900, nullptr, nullptr},
    {kGoto | P(Page::Browser), 400, nullptr, nullptr},
    {LV_KEY_ENTER, 1200, "51-browser-start", nullptr},
    {'x', 400, "52-browser-cursor", nullptr},
    {LV_KEY_NEXT, 300, nullptr, "raspberry pi"},          // search from the address bar
    {0, 300, "58-browser-typing", nullptr},
    {LV_KEY_ENTER, 7000, "59-browser-search", nullptr},
    {LV_KEY_NEXT, 400, "53-browser-toolbar", nullptr},
    {0, 200, nullptr, "en.wikipedia.org/wiki/Raspberry_Pi"},
    {LV_KEY_ENTER, 9000, "54-browser-page", nullptr},
    {LV_KEY_NEXT, 300, nullptr, "upload.wikimedia.org/wikipedia/commons/a/a7/Camponotus_flavomarginatus_ant.jpg"},
    {LV_KEY_ENTER, 7000, "61-browser-image", nullptr},
    {LV_KEY_BACKSPACE, 6000, nullptr, nullptr},
    {' ', 300, nullptr, nullptr},
    {'c', 400, "60-browser-panned", nullptr},
    {kKeyF1, 500, "56-browser-reading", nullptr},
    {kKeyF1, 300, nullptr, nullptr},
    {'t', 400, "57-browser-tabs", nullptr},
    {LV_KEY_ESC, 200, nullptr, nullptr},
    {kKeyF1 + 1, 9000, "62-browser-full", nullptr},     // F2: FULL engine (Chromium) on the same page
    {LV_KEY_NEXT, 300, nullptr, "r6 siege tracker"},       // words: DuckDuckGo search, shown in Chromium
    {LV_KEY_ENTER, 8000, "64-browser-full-search", nullptr},
    {'-', 2500, "68-browser-full-zoom75", nullptr},       // SYM+R: zoom out
    {'-', 2500, "69-browser-full-zoom50", nullptr},
    {'+', 300, nullptr, nullptr},                         // SYM+E: back in
    {'+', 2000, nullptr, nullptr},
    {0, 100, nullptr, "zzzzxxxxxx"},                       // cursor onto the first result
    {0, 900, "63-browser-full-cursor", nullptr},
    {LV_KEY_ENTER, 9000, "67-browser-full-site", nullptr},
    {kKeyF1 + 1, 1500, nullptr, nullptr},                 // back to FAST
    {kHoldEsc, 900, nullptr, nullptr},
    {kGoto | P(Page::Sensors), 400, nullptr, nullptr},
    {LV_KEY_ENTER, 1400, "34-sensors", nullptr},
    {LV_KEY_ESC, 800, nullptr, nullptr},
    {kGoto | P(Page::Camera), 400, nullptr, nullptr},
    {LV_KEY_ENTER, 1000, "35-camera", nullptr},
    {LV_KEY_ENTER, 60, "36-camera-flash", nullptr},
    {0, 500, "37-camera-saved", nullptr},
    {LV_KEY_ESC, 800, nullptr, nullptr},
    {kGoto | P(Page::System), 500, "32-hub-scrolled", nullptr},
    {LV_KEY_ENTER, 1200, nullptr, nullptr},  // SYSTEM
    {LV_KEY_DOWN, 150, nullptr, nullptr},
    {LV_KEY_DOWN, 150, nullptr, nullptr},
    {LV_KEY_DOWN, 150, nullptr, nullptr},
    {LV_KEY_DOWN, 150, nullptr, nullptr},
    {LV_KEY_DOWN, 150, nullptr, nullptr},
    {LV_KEY_DOWN, 150, nullptr, nullptr},   // BOOT row
    {0, 300, "48-system-boot-row", nullptr},
    {LV_KEY_ENTER, 3300, "46-boot-replay", nullptr},
    {0, 3000, nullptr, nullptr},
    {LV_KEY_UP, 150, nullptr, nullptr},
    {LV_KEY_UP, 150, nullptr, nullptr},
    {LV_KEY_UP, 150, nullptr, nullptr},
    {LV_KEY_UP, 150, nullptr, nullptr},
    {LV_KEY_UP, 150, nullptr, nullptr},
    {LV_KEY_UP, 150, nullptr, nullptr},
    {kFnLayer, 300, "26-system-fn-badge", nullptr},
    {kSymLayer, 300, nullptr, nullptr},
    {kNoLayer, 100, nullptr, nullptr},
    {LV_KEY_ESC, 900, nullptr, nullptr},
    {kGoto | P(Page::Gallery), 300, nullptr, nullptr},
    {LV_KEY_ENTER, 1500, "70-gallery", nullptr},
    {LV_KEY_ENTER, 900, "71-gallery-view", nullptr},
    {LV_KEY_ESC, 300, nullptr, nullptr},
    {LV_KEY_ESC, 800, nullptr, nullptr},
    {kGoto | P(Page::Music), 300, nullptr, nullptr},
    {LV_KEY_ENTER, 1500, "72-music", nullptr},
    {LV_KEY_ESC, 800, "73-hub-end", nullptr},
    {kGoto | P(Page::Bluetooth), 500, nullptr, nullptr},
    {LV_KEY_ENTER, 3200, "73a-bluetooth", nullptr},
    {LV_KEY_DOWN, 500, nullptr, nullptr},
    {LV_KEY_DOWN, 500, nullptr, nullptr},
    {LV_KEY_RIGHT, 700, "73b-bluetooth-stream", nullptr},
    {LV_KEY_DOWN, 300, nullptr, nullptr},
    {LV_KEY_DOWN, 300, nullptr, nullptr},
    {LV_KEY_DOWN, 300, nullptr, nullptr},
    {LV_KEY_DOWN, 900, "73c-bluetooth-device", nullptr},
    {LV_KEY_ESC, 800, nullptr, nullptr},
    {kGoto | P(Page::Hdmi), 500, "74-hub-hdmi", nullptr},
    {LV_KEY_ENTER, 1500, "75-hdmi-off", nullptr},   // HDMI: a screen is plugged in, the desktop off
    {LV_KEY_ENTER, 500, "76-hdmi-starting", nullptr},
    {0, 3500, "77-hdmi-on", nullptr},
    {LV_KEY_ESC, 800, "78-hub-hdmi-on", nullptr},
};
}  // namespace

void VelvetUI::start_autoshot(const std::string& dir) {
    tutorial_pending_ = true;  // always show the tour in scripted runs
    if (hub_prompt_) lv_obj_remove_flag(hub_prompt_, LV_OBJ_FLAG_HIDDEN);
    sound_->set_gain(0.0f);  // scripted runs stay silent
    shot_dir_ = dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    shot_step_ = 0;
    shot_timer_ = lv_timer_create(autoshot_cb, kScript[0].wait_ms, this);
}

void VelvetUI::autoshot_cb(lv_timer_t* t) {
    auto* self = static_cast<VelvetUI*>(lv_timer_get_user_data(t));
    const size_t n = sizeof(kScript) / sizeof(kScript[0]);
    const ShotStep& cur = kScript[self->shot_step_];
    if (cur.shot) {
        lv_draw_buf_t* snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888);
        // badges and popups live on the top layer: paint it over the screen shot
        lv_draw_buf_t* top = snap ? lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888) : nullptr;
        if (top && top->header.w == snap->header.w && top->header.h == snap->header.h) {
            for (uint32_t y = 0; y < snap->header.h; ++y) {
                auto* d = reinterpret_cast<uint8_t*>(snap->data) + y * snap->header.stride;
                const auto* t = reinterpret_cast<const uint8_t*>(top->data) + y * top->header.stride;
                for (uint32_t x = 0; x < snap->header.w; ++x) {
                    const uint32_t al = t[x * 4 + 3];
                    for (int c = 0; c < 3; ++c) d[x * 4 + c] = static_cast<uint8_t>((t[x * 4 + c] * al + d[x * 4 + c] * (255 - al)) / 255);
                }
            }
        }
        if (top) lv_draw_buf_destroy(top);
        if (snap) {
            const std::string path = self->shot_dir_ + "/" + cur.shot + ".png";
            std::printf("[optic] shot %s %s\n", path.c_str(), write_png(path, snap) ? "ok" : "FAILED");
            lv_draw_buf_destroy(snap);
        }
    }
    self->shot_step_++;
    if (self->shot_step_ >= n) {
        lv_timer_delete(t);
        self->shot_timer_ = nullptr;
        self->quit_(self->quit_user_);
        return;
    }
    const ShotStep& next = kScript[self->shot_step_];
    auto press = [self](uint32_t k) {
        self->handle_key(k, false);
        self->handle_key_release(k);
    };
    if (next.key == kEscDown) {
        self->handle_key(LV_KEY_ESC, false);
    } else if (next.key == kEscUp) {
        self->handle_key_release(LV_KEY_ESC);
    } else if (next.key == kFnLayer || next.key == kSymLayer || next.key == kNoLayer) {
        self->set_key_layer(next.key == kFnLayer ? 1 : next.key == kSymLayer ? 2 : 0);
    } else if (next.key == kHoldEsc) {
        self->handle_key(LV_KEY_ESC, false);
        self->handle_key(LV_KEY_ESC, true);
        self->handle_key_release(LV_KEY_ESC);
    } else if ((next.key & 0xFF000000) == kGoto) {
        for (int i = 0; i < kHubCount; ++i) {
            if (self->page_kind_ == Page::Hub && P(kHubPages[i]) == (next.key & 0xFF)) self->hub_select(i, true);
        }
    } else if (next.key) {
        press(next.key);
    }
    if (next.text) {
        for (const char* c = next.text; *c; ++c) {
            press(*c == '\n' ? uint32_t{LV_KEY_ENTER} : *c == '\t' ? uint32_t{LV_KEY_NEXT} : static_cast<unsigned char>(*c));
        }
    }
    lv_timer_set_period(t, next.wait_ms);
    lv_timer_reset(t);
}

} // namespace velvet
