// CardputerZero UI ("Velvet" in the code): MinUI-style pocket OS for the CardputerZero (320x170).
#pragma once

#include "btaudio.h"
#include "device.h"
#include "lvgl.h"
#include "browser.h"
#include "chrome.h"
#include "music.h"
#include "netlink.h"
#include "nettools.h"
#include "pwn.h"
#include "sensors.h"
#include "shell.h"
#include "sound.h"
#include "term.h"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>
#include <string>
#include <map>
#include <vector>

namespace app {
class AssetManager;
}

namespace velvet {

enum class Page { Hub, Stats, Wifi, System, Terminal, Camera, Sensors, Browser, Gallery, Music, Pwn, Net, Hdmi, Bluetooth };

class VelvetUI {
public:
    using QuitFn = void (*)(void* user);

    VelvetUI(app::AssetManager& assets, QuitFn quit, void* quit_user);
    ~VelvetUI();

    void build();
    // Returns true when the key was handled.
    bool handle_key(uint32_t key, bool long_pressed);
    // Key let go (the terminal sends Esc on release, so holding Esc can leave instead).
    void handle_key_release(uint32_t key);
    // Key layer picked with fn: 0 normal, 1 orange (FN badge), 2 blue (SYM badge).
    void set_key_layer(int layer);
    // a modifier key pressed (1 SYM, 2 Aa, 3 fn, 4 ctrl, 5 alt): SYM Aa fn ctrl alt in a row on
    // the System page opens the credits (an Easter egg: nothing hints at it)
    void mod_tap(int which);
    // Scripted run that saves PNG screenshots into `dir`, then quits.
    void start_autoshot(const std::string& dir);

private:
    // One family (Terminus bold bitmaps, 1 bpp) at three sizes.
    struct Fonts {
        lv_font_t* big{};     // 10x18: headings, big numbers
        lv_font_t* mono12{};  // 8x16: list rows, body text
        lv_font_t* mono10{};  // 8x14: small text, hints
        lv_font_t* bold11{};  // 8x14 (same as mono10)
    };

    // building
    void build_hub(lv_obj_t* parent);
    void build_page(Page page);
    void build_page_chrome(lv_obj_t* parent, const char* title, bool back_hint = true);
    lv_obj_t* status_pill(lv_obj_t* parent, int32_t right, int32_t y, bool day);
    static void status_draw_cb(lv_event_t* e);
    void build_stats(lv_obj_t* parent);
    void build_system(lv_obj_t* parent);
    void build_hdmi(lv_obj_t* parent);

    // behavior
    void hub_select(int index, bool animate);
    void open_page(Page page);
    void close_page();
    void transition(bool forward, void (*swap)(VelvetUI*));
    void show_quit_hint();
    void stats_select(int index);
    void clear_page_timers();
    void update_hub_status();
    void build_ui();
    void wifi_set_hints(std::initializer_list<std::pair<const char*, const char*>> items);

    // wifi page
    enum class WifiMode { List, Actions, Password, Eap, Busy };
    void build_wifi(lv_obj_t* parent);
    bool wifi_key(uint32_t key, bool long_pressed);
    // NETWORK: a menu (Wi-Fi, Ethernet, Tor, Tailscale, Speed test, Tools) and their views
    void build_net(lv_obj_t* parent);
    void net_set_view(int view);
    void net_hints();
    bool net_key(uint32_t key, bool long_pressed);
    static void net_draw_cb(lv_event_t* e);
    static void net_tick_cb(lv_timer_t* t);
    void wifi_scan();
    void wifi_refresh_status();
    void wifi_render_list();
    void wifi_render_status();
    void wifi_open_actions();
    void wifi_render_actions();
    void wifi_run_action();
    void wifi_open_password();
    void wifi_render_password();
    void wifi_open_eap();
    void wifi_render_eap();
    void wifi_eap_hints();
    void wifi_close_overlay();
    void wifi_connect(const std::string& password);
    void wifi_busy(const char* text);
    void wifi_result(bool ok, const std::string& message);
    static void wifi_tick_cb(lv_timer_t* t);

    // terminal page
    void build_terminal(lv_obj_t* parent);
    bool term_key(uint32_t key, bool long_pressed);
    void term_pump();
    void term_render_info();
    void term_restart();
    static void term_tick_cb(lv_timer_t* t);
    static void term_draw_cb(lv_event_t* e);

    // browser page
    struct BTab {
        int id{0};
        std::string url, title, error;
        Doc doc;
        std::vector<Line> lines;
        std::vector<int> link_line;  // first line of each link
        int scroll{0}, sel{-1};
        std::vector<std::string> back;
        bool loading{false};
        int hops{0};   // redirect-only pages followed in a row (meta refresh / a one-line script)
        // rendered view: the page laid out 512 px wide; (vx, vy) is the screen's top-left on it
        PageLayout page;
        int32_t vx{0}, vy{0};
        std::vector<lv_draw_buf_t*> imgs;
        std::vector<std::pair<int, int>> isz;
        std::vector<std::string> values;  // what's typed in each form field
        uint32_t gen{0};
        size_t img_pixels{0};
    };
    void build_browser(lv_obj_t* parent);
    bool browser_key(uint32_t key, bool long_pressed);
    void browser_go(const std::string& url, bool remember);
    void browser_show(BTab& tab, const Doc& doc);
    void browser_back();
    void browser_new_tab(const std::string& url);
    void browser_close_tab();
    void browser_select(int link);
    void browser_scroll(int delta);
    void browser_render();
    void browser_toolbar(bool open, bool edit_url);
    void browser_relayout(BTab& tab);
    void browser_free(BTab& tab);
    void browser_load_images(BTab& tab);
    void browser_pan(int32_t dx, int32_t dy);
    int browser_hover() const;  // index of the clickable box under the cursor, or -1
    void browser_click();
    void browser_submit(int field);
    void browser_draw_page(lv_layer_t* layer, const lv_area_t& a);
    void browser_draw_full(lv_layer_t* layer, const lv_area_t& a);
    void browser_set_engine(bool full);
    void browser_full_tick();
    void browser_toast(const std::string& text);
    void browser_full_search(const std::string& q_enc);
    void browser_zoom(int dir);
    float bz() const { return bfull_ ? bzoom_ : 1.0f; }  // screen px per page px
    int32_t bcur_x() const;  // the page point under the cursor
    int32_t bcur_y() const;
    int32_t browser_page_w() const;
    int32_t browser_page_h() const;
    BTab& btab() { return btabs_[bcur_]; }
    static void browser_draw_cb(lv_event_t* e);
    static void browser_tick_cb(lv_timer_t* t);

    // sensors page
    void build_sensors(lv_obj_t* parent);
    void build_pwn(lv_obj_t* parent);
    static void pwn_tick_cb(lv_timer_t* t);
    static void pwn_face_draw_cb(lv_event_t* e);
    bool pwn_key(uint32_t key, bool long_pressed);
    static void sensors_tick_cb(lv_timer_t* t);
    static void compass_draw_cb(lv_event_t* e);
    static void level_draw_cb(lv_event_t* e);

    // camera page
    void build_camera(lv_obj_t* parent);
    void camera_snap();
    static void camera_tick_cb(lv_timer_t* t);

    // hold-ESC popup: fills left to right while ESC is held; full = the hold action
    void hold_begin(const char* what);
    void hold_end();
    static void hold_tick_cb(lv_timer_t* t);
    void term_set_compact(bool on);

    // boot splash (an overlay over whatever is underneath)
    void splash_start();
    void splash_skip();
    float splash_time() const;
    static void splash_tick_cb(lv_timer_t* t);
    static void splash_draw_cb(lv_event_t* e);

    // tutorial (an overlay over whatever screen opened it)
    void tutorial_open();
    void tutorial_close(bool never_again);
    void tutorial_render(int dir);
    bool tutorial_key(uint32_t key, bool long_pressed);

    // system page
    void system_render();
    void system_adjust(int delta);
    static void system_tick_cb(lv_timer_t* t);
    // the Bluetooth page: devices, where sound goes, a test sound
    void build_bluetooth(lv_obj_t* p);
    void bt_render();
    bool bt_key(uint32_t key);
    static void bt_tick_cb(lv_timer_t* t);
    struct BtRow { int kind; int ref; std::string left, right; };
    std::vector<BtRow> bt_rows();
    std::unique_ptr<BtAudio> bt_;
    BtAudioState bt_state_{};
    lv_obj_t* bt_box_{};
    lv_obj_t* bt_note_{};
    lv_timer_t* bt_timer_{};
    int bt_sel_{0}, bt_top_{0}, bt_beat_{0}, bt_hub_age_{0};
    void hdmi_render();
    void hdmi_toggle();
    static void hdmi_tick_cb(lv_timer_t* t);

    static void swap_to_target(VelvetUI* self);
    static void swap_to_hub(VelvetUI* self);
    static void render_ready_cb(lv_event_t* e);
    static void autoshot_cb(lv_timer_t* t);

    app::AssetManager& assets_;
    std::unique_ptr<Device> device_;
    std::unique_ptr<Sound> sound_;
    int key_layer_{0};
    lv_obj_t* fn_badge_{};
    NetStatus net_{};          // last known network status (hub + wifi + system)
    lv_obj_t* hub_status_{};   // status pills (time, Wi-Fi, battery) on the hub and on the open page
    lv_obj_t* page_status_{};
    lv_obj_t* hub_hints_{};
    lv_obj_t* hub_count_{};
    lv_timer_t* status_timer_{};
    QuitFn quit_;
    void* quit_user_;
    Fonts f_{};

    lv_obj_t* root_{};
    lv_obj_t* hub_{};
    lv_obj_t* page_{};
    Page page_kind_{Page::Hub};
    Page target_{Page::Hub};
    bool transitioning_{false};
    bool esc_from_hub_{false};

    // hub widgets
    int hub_index_{0};
    int hub_top_{0};  // first visible menu row (the menu scrolls once it has more than 5)
    lv_obj_t* hub_thumb_{};    // scroll bar thumb
    std::vector<lv_obj_t*> hub_labels_;
    std::vector<lv_obj_t*> hub_icons_;
    std::vector<lv_obj_t*> hub_values_;
    lv_obj_t* hub_plate_{};
    lv_obj_t* quit_hint_{};
    lv_timer_t* quit_hint_timer_{};

    // generic selectable list on pages
    int list_index_{0};


    // stats page
    std::vector<lv_obj_t*> stat_names_;
    std::vector<lv_obj_t*> stat_markers_;   // row pills
    std::vector<lv_obj_t*> stat_parts_;     // per row: track, fill, value, rank pill, rank letter

    // wifi page
    uint32_t wifi_gen_{0};  // bumps on every page build/teardown; stale callbacks compare it
    WifiMode wifi_mode_{WifiMode::List};
    std::vector<WifiAp> wifi_aps_;
    int wifi_sel_{0};
    int wifi_top_{0};
    bool wifi_scanning_{false};
    lv_obj_t* wifi_status_{};
    lv_obj_t* wifi_status_dot_{};
    lv_obj_t* wifi_scan_label_{};
    lv_obj_t* wifi_msg_{};
    lv_obj_t* wifi_count_{};
    lv_obj_t* wifi_hint_{};
    struct WifiRow {
        lv_obj_t* shadow{};
        lv_obj_t* plate{};
        lv_obj_t* name{};
        lv_obj_t* tag{};
        lv_obj_t* lock{};
        lv_obj_t* bars{};
    };
    std::vector<WifiRow> wifi_rows_;
    lv_obj_t* wifi_overlay_{};
    std::vector<std::string> wifi_actions_;
    std::vector<lv_obj_t*> wifi_action_plates_;
    std::vector<lv_obj_t*> wifi_action_labels_;
    int wifi_action_sel_{0};
    std::string wifi_target_;     // SSID the overlay is about
    std::string wifi_password_;
    bool wifi_show_pw_{false};
    lv_obj_t* wifi_pw_label_{};
    lv_obj_t* wifi_pw_info_{};
    // the login form of a WPA-Enterprise network: identity, anonymous identity, password, method
    static constexpr int kEapRows = 4;
    WifiEap wifi_eap_;
    int wifi_eap_sel_{0};
    lv_obj_t* wifi_eap_plates_[kEapRows]{};
    lv_obj_t* wifi_eap_names_[kEapRows]{};
    lv_obj_t* wifi_eap_values_[kEapRows]{};
    lv_obj_t* wifi_eap_info_{};
    lv_timer_t* wifi_timer_{};
    uint32_t wifi_result_until_{0};

    // system page
    int sys_sel_{0};
    std::vector<int> sys_vis_;   // the System rows shown: only what this device can set
    void sys_step(int d);
    lv_obj_t* sys_batt_{};
    lv_obj_t* sys_batt_shadow_{};
    lv_obj_t* sys_batt_info_{};
    static constexpr int kSysRows = 8;   // BRIGHTNESS VOLUME SOUND THEME POWER TUTORIAL (BOOT) R36S (Tor is in NETWORK)
    lv_obj_t* sys_rows_[kSysRows]{};
    lv_obj_t* sys_markers_[kSysRows]{};
    lv_obj_t* sys_fills_[kSysRows]{};
    lv_obj_t* sys_values_[kSysRows]{};
    lv_obj_t* sys_tracks_[kSysRows]{};
    lv_obj_t* sys_batt_bar_{};
    lv_obj_t* sys_ip_{};
    lv_timer_t* sys_timer_{};

    // HDMI: the Optic desktop on the HDMI screen, seen and switched from here
    HdmiInfo hdmi_{};
    lv_obj_t* hdmi_big_{};
    lv_obj_t* hdmi_sub_{};
    lv_obj_t* hdmi_val_{};
    lv_obj_t* hdmi_note_{};
    lv_timer_t* hdmi_timer_{};
    int hdmi_busy_{0};        // seconds left to wait for a start / stop to show
    bool hdmi_want_{false};
    int hdmi_hub_age_{0};
    lv_obj_t* fps_label_{};
    uint32_t frames_{0};
    uint32_t started_ms_{0};
    bool render_cb_added_{false};

    // terminal page (the session outlives the page: leave and come back to the same shell)
    std::unique_ptr<Terminal> term_;
    std::unique_ptr<Shell> shell_;
    bool term_exited_{false};
    lv_obj_t* term_view_{};
    lv_obj_t* term_info_{};
    lv_obj_t* term_banner_{};
    lv_timer_t* term_timer_{};
    bool term_blink_{true};
    bool term_esc_pending_{false};
    int term_cursor_row_{-1};
    uint32_t term_blink_at_{0};
    std::string term_info_text_;

    // browser page (tabs survive leaving the app)
    std::unique_ptr<Fetcher> fetcher_;
    std::vector<BTab> btabs_;
    int bcur_{0};
    int bnext_id_{1};
    lv_obj_t* bview_{};
    lv_obj_t* btool_{};
    lv_obj_t* burl_{};
    lv_obj_t* bcount_{};
    lv_obj_t* bbtns_[5]{};
    lv_obj_t* blist_{};
    lv_obj_t* bload_{};
    lv_timer_t* btimer_{};
    bool btool_open_{false};
    int bfocus_{0};     // toolbar: 0 address, 1 buttons
    int bbtn_sel_{1};
    std::string bedit_;
    bool bedit_fresh_{false};  // the shown address is selected: typing replaces it
    bool blist_open_{false};
    int blist_sel_{0};
    bool b_esc_pending_{false};
    bool breader_{false};      // F1: text reader instead of the rendered page
    int bfield_{-1};           // form field being typed into
    lv_obj_t* bfbox_{};        // its typing strip
    uint32_t bpan_key_{0};     // pan key held down (repeats until released)
    // FULL engine (F2): headless Chromium renders the page
    bool bfull_{false};
    std::unique_ptr<ChromeEngine> chrome_;
    uint32_t bfull_loads_{0}, bfull_serial_{0};
    uint32_t bcap_at_[2]{0, 0};   // scheduled re-captures (ms ticks, 0 = none)
    int32_t bprobe_x_{-1}, bprobe_y_{-1};
    uint32_t bmoved_at_{0};
    uint32_t bacted_at_{0};       // last click / typing / navigation (FULL refreshes faster after it)
    uint32_t bshot_at_{0};        // last screenshot request
    lv_obj_t* bengine_{};
    std::string btoast_;           // a short note along the bottom
    float bzoom_{1.0f};            // FULL zoom: 1, 0.75, 0.5 (SYM+E / SYM+R)
    uint32_t btoast_until_{0};
    std::string bgoogle_seen_;     // a Google URL already sent to Bing
    uint32_t bpan_t0_{0};
    int32_t bpan_done_{0};     // pixels already glided this hold

    // sensors page
    std::unique_ptr<Sensors> sensors_;
    Motion motion_{};
    lv_obj_t* compass_{};
    lv_obj_t* level_{};
    lv_obj_t* sens_heading_{};
    lv_obj_t* sens_tilt_{};
    lv_obj_t* sens_values_{};
    lv_timer_t* sens_timer_{};

    // pwn page
    std::unique_ptr<Pwn> pwn_;
    PwnState pwn_state_{};
    lv_obj_t* pwn_face_{};
    lv_obj_t* pwn_face_img_{};
    lv_obj_t* pwn_name_{};
    lv_obj_t* pwn_tag_{};
    lv_obj_t* pwn_notice_{};
    lv_obj_t* pwn_line_{};
    lv_obj_t* pwn_stats_{};
    lv_obj_t* pwn_about_{};
    lv_timer_t* pwn_timer_{};
    std::string pwn_face_path_;
    std::string pwn_shown_line_;
    int pwn_shown_mood_{-1};
    bool pwn_about_open_{false};
    bool pwn_is_iris_{false};

    // camera page
    std::unique_ptr<Camera> camera_;
    lv_draw_buf_t* cam_buf_{};
    std::vector<uint32_t> cam_frame_;
    lv_obj_t* cam_img_{};
    lv_obj_t* cam_status_{};
    lv_obj_t* cam_msg_{};
    lv_obj_t* cam_banner_{};
    lv_obj_t* cam_flash_{};
    lv_timer_t* cam_timer_{};
    uint32_t cam_banner_until_{0};
    bool cam_busy_{false};

    // hold-ESC popup
    lv_obj_t* hold_pop_{};
    lv_obj_t* hold_fill_{};
    lv_timer_t* hold_timer_{};
    // GALLERY: the photos folder as a grid of thumbnails, and a zoomable viewer
    std::vector<std::string> gal_files_;
    std::map<std::string, lv_draw_buf_t*> gal_thumbs_;  // path -> 72x54 thumbnail (kept while Velvet runs)
    std::vector<lv_obj_t*> gal_cells_, gal_imgs_;
    lv_obj_t* gal_sel_box_{};
    lv_obj_t* gal_count_{};
    lv_obj_t* gal_empty_{};
    lv_timer_t* gal_timer_{};
    int gal_sel_{0}, gal_top_{0};
    lv_obj_t* gal_view_{};                 // the viewer, over the grid
    lv_obj_t* gal_view_img_{};
    lv_obj_t* gal_info_{};
    lv_draw_buf_t* gal_src_{};             // the open photo, decoded
    lv_draw_buf_t* gal_out_{};             // what the screen shows (320x170)
    int gal_open_{-1};
    bool gal_loading_{false};
    float gal_zoom_{1.0f}, gal_cx_{0.5f}, gal_cy_{0.5f};
    void build_gallery(lv_obj_t* p);
    void gallery_layout();
    bool gallery_key(uint32_t key, bool long_pressed);
    void gallery_open(int index);
    void gallery_close_view();
    void gallery_render();
    static void gallery_tick_cb(lv_timer_t* t);
    // MUSIC: YouTube search + audio (keeps playing when you leave the page)
    std::unique_ptr<Music> music_;
    // SYSTEM's TOR and R36S (netlink.h): the R36S gets the screen and sends keys back
    std::unique_ptr<Tor> tor_;
    // NETWORK page
    std::unique_ptr<NetTools> nett_;
    lv_obj_t* net_canvas_{};
    lv_obj_t* net_top_{};      // the title bar (rebuilt per view)
    lv_obj_t* net_hint_{};
    std::string net_hint_sig_;
    float net_needle_{0};      // the speed gauge's needle, easing toward the reading
    uint32_t net_refresh_at_{0};
    lv_timer_t* net_timer_{};
    int net_view_{0};          // NetView
    int net_sel_{0};           // the menu row
    int net_tool_{0};          // the tools list row
    int net_scroll_{0};        // first line shown (tool output, Tailscale devices)
    bool net_editing_{false};  // typing a tool's target
    std::string net_target_;
    bool wifi_from_net_{false};  // Wi-Fi was opened from NETWORK: ESC goes back there
    std::unique_ptr<ScreenLink> slink_;
    lv_timer_t* link_timer_{};
    std::string link_proxy_;           // the Tor proxy Chromium was last started with
    bool link_btn_[16]{};
    int link_axis_[4]{};
    uint32_t link_stick_key_{0};       // the arrow the left stick holds, and when it repeats
    uint32_t link_stick_next_{0};
    std::vector<uint16_t> link_px_;
    static void link_tick_cb(lv_timer_t* t);
    void link_tick();
    void link_button(int b, bool down);
    bool link_capture();
    std::vector<Track> mus_results_;
    bool mus_ended_ = false;
    Music::Status mus_status();
    std::string mus_query_, mus_playing_id_;
    int mus_sel_{0}, mus_top_{0};
    bool mus_typing_{true};
    bool mus_lib_{false};                  // the list shows MY MUSIC (files), not YouTube results
    lv_obj_t* mus_box_{};
    lv_obj_t* mus_query_lbl_{};
    lv_obj_t* mus_sel_bg_{};
    lv_obj_t* mus_msg_{};
    lv_obj_t* mus_now_{};
    lv_obj_t* mus_time_{};
    lv_obj_t* mus_fill_{};
    std::vector<lv_obj_t*> mus_rows_, mus_durs_;
    lv_timer_t* mus_timer_{};
    lv_obj_t* mus_vid_{};                  // video: full screen, over the page
    lv_obj_t* mus_vid_img_{};
    lv_obj_t* mus_vid_bar_{};
    lv_obj_t* mus_vid_info_{};
    lv_draw_buf_t* mus_vid_buf_{};
    std::vector<uint32_t> mus_frame_;
    uint32_t mus_vid_info_at_{0};
    int mus_tick_n_{0};
    int gvol_{-1}, gvol_unmuted_{50};   // the device's volume as last read / set here (-1: no mixer)
    std::string mus_prep_id_;           // the highlighted track, and how long it has been
    int mus_prep_n_{0};
    uint32_t volkey_{0};
    int volkey_from_{-1};
    lv_timer_t* volkey_timer_{nullptr};
    void global_volume(int delta);
    void global_mute();
    void music_video_show(bool on);
    // Now Playing: a track opened fills the page (cover, title, progress) until a short ESC
    void music_full(bool on);
    void music_set_art(const std::string& bytes);
    static void music_full_draw_cb(lv_event_t* e);
    // the credits Easter egg
    void egg_open();
    void egg_close();
    bool egg_key(uint32_t key, bool long_pressed);
    static void egg_draw_cb(lv_event_t* e);
    lv_obj_t* egg_{};
    lv_timer_t* egg_timer_{};
    std::vector<float> egg_cov_;   // the full-screen logo animation's coverage
    uint32_t egg_frame_at_{0};
    lv_draw_buf_t* egg_full_{};
    int egg_pos_{0};      // how far into SYM Aa fn ctrl alt
    bool egg_btc_{false};  // the bio shows the Bitcoin QR instead of GitHub's
    int egg_view_{0};     // 0 the QR + 2B, 1 2B full screen, 2 the bio + the QR
    float egg_qr_x_{0};   // where the QR is drawn (slides to its view's place)
    lv_obj_t* mus_full_{};          // the view (nullptr: the list shows)
    lv_obj_t* mus_art_img_{};
    lv_draw_buf_t* mus_art_buf_{};  // the cover, 104 x 104
    bool mus_has_art_{false};
    bool mus_esc_pending_{false};   // ESC down in Now Playing: back to the list on release
    void build_music(lv_obj_t* p);
    void music_render();
    bool music_key(uint32_t key, bool long_pressed);
    static void music_tick_cb(lv_timer_t* t);
    float gal_src_w() const { return gal_src_ ? static_cast<float>(gal_src_->header.w) : 1.0f; }
    float gal_src_h() const { return gal_src_ ? static_cast<float>(gal_src_->header.h) : 1.0f; }
    // Held Backspace / Delete repeats (neither the device nor the web build repeats keys)
    lv_timer_t* rep_timer_{};
    uint32_t rep_key_{0};
    bool rep_firing_{false};
    void rep_stop();
    uint32_t hold_t0_{0};
    const char* hold_what_{""};

    // terminal size: normal 40x11 (8x14 cells under a header) or F1 compact 53x14 (6x12, no header)
    bool term_compact_{false};
    bool term_compact_changed_{false};
    int32_t term_cw_{8}, term_ch_{14}, term_top_{16};
    const lv_font_t* term_font_{};

    // boot splash
    lv_obj_t* splash_{};
    lv_obj_t* splash_fx_{};
    lv_obj_t* logo_img_{};
    lv_timer_t* splash_timer_{};
    uint32_t splash_t0_{0};
    uint32_t splash_cues_{0};
    bool splash_exiting_{false};
    std::vector<float> logo_cov_;   // the Optic Suite logo, drawn each frame
    lv_draw_buf_t* logo_buf_{};

    // tutorial
    // ECO power mode
    void eco_set(bool on);
    bool eco_wake();
    bool music_playing_video();
    bool eco_{false};
    int eco_dim_{0};               // 0 bright, 1 dimmed, 2 backlight off
    int eco_saved_bright_{-1};
    uint32_t last_input_ms_{0};
    lv_timer_t* eco_timer_{};
    int theme_{1};                  // 0 Dark, 1 White (setting ui_theme; White by default)
    bool tutorial_at_start_{true};  // saved setting; "never show again" turns it off
    bool tutorial_pending_{false};  // the next key on the home menu opens it
    lv_obj_t* tut_{};
    lv_obj_t* tut_body_{};
    lv_obj_t* hub_prompt_{};
    int tut_page_{0};
    int tut_sel_{0};

    // autoshot
    std::string shot_dir_;
    size_t shot_step_{0};
    lv_timer_t* shot_timer_{};
};

} // namespace velvet
