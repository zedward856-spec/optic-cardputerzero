// Velvet's reader browser: fetches a page, turns its HTML into styled text lines (headings,
// paragraphs, lists, links, pre blocks), and lays them out for a small fixed-width screen.
// Fetching: curl on the CardputerZero / PC, emscripten_fetch in the browser demo.
#pragma once

#include "lvgl.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace velvet {

enum SpanStyle : uint8_t { kText, kHeading, kLink, kPre, kQuote, kDim, kBold, kRule, kImage, kInput, kButton };

struct Span {
    std::string text;  // UTF-8
    uint8_t style{kText};
    int link{-1};      // index into Doc::links for links
    int img{-1};       // index into Doc::images (kImage)
    int field{-1};     // index into Doc::fields (kInput, kButton)
};

struct Block {
    std::vector<Span> spans;
    std::string prefix;  // "• " for list items, "> " for quotes
    bool pre{false};
    bool gap_before{true};  // blank line before (paragraphs, headings)
    int level{0};           // heading level 1..6
};

struct Img {
    std::string url, alt;
    int w{0}, h{0};  // from the width / height attributes, 0 if unknown
};
struct Form {
    std::string action, method;
    std::vector<std::pair<std::string, std::string>> hidden;
};
struct Field {
    int form{-1};
    std::string name, value, placeholder, type;  // type: "text" or "submit"
};

struct Doc {
    std::string title;
    std::string url;
    std::vector<std::string> links;
    std::vector<Block> blocks;
    std::vector<Img> images;
    std::vector<Form> forms;
    std::vector<Field> fields;
};

// ---- rendered view: the page laid out at a fixed pixel width (bigger than the screen)
enum BoxKind : uint8_t { kBoxText, kBoxImage, kBoxRule, kBoxInput, kBoxButton, kBoxBullet, kBoxPreBg, kBoxQuoteBar };
struct PBox {
    int32_t x{0}, y{0}, w{0}, h{0};
    uint8_t kind{kBoxText};
    uint8_t style{kText};
    int level{0};
    int link{-1}, img{-1}, field{-1};
    const lv_font_t* font{nullptr};
    std::string text;
};
struct PageFonts {
    const lv_font_t* body;  // monospace 8 px bitmap
    const lv_font_t* mono;
    const lv_font_t* h1;
    const lv_font_t* h2;
    const lv_font_t* h3;
};
struct PageLayout {
    int32_t width{512}, height{0};
    std::vector<PBox> boxes;
};
// sizes: decoded image sizes (0,0 = not loaded yet), one per Doc::images entry
PageLayout layout_page(const Doc& doc, int32_t width, const PageFonts& f, const std::vector<std::pair<int, int>>& sizes);

// Search: DuckDuckGo lite on the device; Wikipedia search in the browser demo (it allows it).
std::string search_url(const std::string& query);
// A search engine's click-tracking link -> the address it really goes to (else unchanged).
std::string unwrap_redirect(const std::string& url);
// where a script- or meta-refresh-only page sends you ("" if it doesn't)
std::string redirect_target(const std::string& html);
// FULL engine: Velvet's home page, and a DuckDuckGo lite result page made ready to show in
// Chromium (real result addresses, a <base> for the rest).
std::string full_home_html();
std::string ddg_lite_for_chrome(const std::string& html);
constexpr const char* kDdgLiteSearch = "https://lite.duckduckgo.com/lite/?q=";
// A JSON search answer -> a results page. False if `url` isn't a search we know.
bool search_results_html(const std::string& url, const std::string& body, std::string& html);
// The URL a form submits to, with the typed values (one per Doc::fields entry).
std::string form_url(const Doc& doc, int form, const std::vector<std::string>& values);
// PNG / JPEG / GIF / BMP bytes -> an ARGB8888 draw buffer, scaled down to fit max_w.
lv_draw_buf_t* decode_image(const std::string& bytes, int max_w, int max_h);

struct Line {
    std::vector<Span> spans;
};

Doc parse_html(const std::string& html, const std::string& base_url);
Doc parse_text(const std::string& text, const std::string& url);
std::vector<Line> layout_doc(const Doc& doc, int cols);
std::string resolve_url(const std::string& base, const std::string& href);
// What the user typed in the address bar -> a URL (adds https://, or searches).
std::string normalize_input(const std::string& typed);
size_t utf8_len(const std::string& s);
// Text the pixel font can draw: accents folded (é -> e), anything missing becomes ?
std::string font_safe_text(const lv_font_t* f, const std::string& s);

class Fetcher {
public:
    // ok, final URL, content type, body, error message. Always called on the UI thread.
    using Done = std::function<void(bool, const std::string&, const std::string&, const std::string&, const std::string&)>;
    virtual ~Fetcher() = default;
    // Several requests can run at once; each gets its own answer.
    virtual void get(const std::string& url, Done cb) = 0;
    virtual void cancel_all() = 0;  // answers to requests made before this are dropped
};
std::unique_ptr<Fetcher> make_fetcher();

// Built-in pages: "velvet:start", "velvet:help".
bool builtin_page(const std::string& url, std::string& html);

}  // namespace velvet
