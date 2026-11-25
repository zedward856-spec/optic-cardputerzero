// FULL browser engine: a headless Chromium that Velvet drives over Chrome's DevTools protocol
// (the --remote-debugging-pipe variant: JSON messages on file descriptors 3 and 4, no network
// port). Chromium renders the page 512 px wide; Velvet shows screenshots of it, sends it clicks
// and typing, and asks what is under the cursor. Real JavaScript, so every site works.
// Linux only (the CardputerZero and the PC); the browser demo can't run it.
#pragma once

#include "lvgl.h"

#include <cstdint>
#include <memory>
#include <string>

namespace velvet {

class ChromeEngine {
public:
    static constexpr int kW = 512, kH = 768;  // Chromium's viewport (tall enough for pop-ups like a robot check)

    struct State {
        bool ready{false};       // attached to a page
        bool loading{false};
        std::string url, title;
        int height{kH};          // document height
        lv_draw_buf_t* tile{};   // latest screenshot (kW x kH), owned by the engine
        int tile_y{0};           // document y of the tile's top row
        float tile_scale{1.0f};  // the tile is the page drawn at this size (zoomed out < 1)
        uint32_t tile_serial{0}; // bumps on every new tile
        std::string hover;       // under the cursor: "" | "LINK <url>" | "INPUT" | "BUTTON"
        uint32_t loads{0};       // bumps when a page finishes loading
        std::string error;
    };

    ChromeEngine();
    ~ChromeEngine();

    static std::string find_binary();  // "" if no Chromium / Chrome is installed
    static bool available();

    bool start(std::string& error);
    void stop();
    bool running() const;
    // the Tor proxy it was started with ("" = none): Velvet restarts it when SYSTEM's TOR changes
    const std::string& proxy() const { return proxy_; }

    void navigate(const std::string& url);
    void reload();
    void back();
    void capture(int doc_y);                 // screenshot with the page scrolled to doc_y
    void set_scale(float s) { scale_ = s; }  // zoom: screenshots draw the page this much smaller
    void click(int x, int doc_y);            // a left click at document coordinates
    void type(const std::string& text, bool enter);
    void probe(int x, int doc_y);            // fills State::hover
    void show_html(const std::string& html); // our own page (search results, home) in Chromium
    void search(const std::string& q_enc);   // web demo: the bridge searches (q URL-encoded)
    void home();                             // the FULL home page
    void poll();                             // call often from the UI thread

    const State& state() const { return st_; }

private:
    std::string proxy_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    State st_;
    float scale_{1.0f};
};

}  // namespace velvet
