#include "pwn.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#ifndef __EMSCRIPTEN__
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace velvet {
namespace {

const char* const kMoods[] = {"awake", "happy", "excited", "cool", "bored", "sad", "lonely", "angry", "sleep",
                              "intense", "grateful", "motivated", "demotivated", "friend", "smart", "broken",
                              "look_l", "look_r"};
constexpr int kNMood = 18;

// the construct's own lines (matches Optic DE's Pwn)
const char* const kLines[kNMood][3] = {
    {"Systems up. Listening.", "Booting the good stuff.", "Hello, world. Again."},
    {"Good air today.", "Everything's humming.", "I like it here."},
    {"Got one!", "A new friend said hi.", "Again, again!"},
    {"Cool as a heatsink.", "No rush.", "Smooth."},
    {"Nothing new on the air...", "Same channels, same quiet.", "Wake me if something moves."},
    {"Quiet out there.", "Nobody's talking to me.", "Hm."},
    {"Is anyone else around?", "Just me and the noise floor.", "Lonely channel."},
    {"Stop moving around.", "That one wandered off.", "Grr."},
    {"Zzz...", "Resting my antennas.", "Five more minutes."},
    {"Locked on.", "Focus.", "Counting the quiet."},
    {"Thanks for keeping me on.", "Good company.", "Appreciated."},
    {"Let's find something.", "New channel, new luck.", "Onward."},
    {"Not my day.", "Why do I even scan.", "Meh."},
    {"A friend nearby!", "Hi, other unit.", "Good to see you."},
    {"Learning the air.", "Patterns, patterns.", "I get it now."},
    {"Something broke.", "Error, error.", "Ouch."},
    {"What's over there?", "Looking left.", "Hm?"},
    {"And over there?", "Looking right.", "Hm?"},
};

int mood_index(const std::string& m) {
    for (int i = 0; i < kNMood; ++i)
        if (m == kMoods[i]) return i;
    if (m.rfind("sleep", 0) == 0) return 8;
    if (m.rfind("look_r", 0) == 0) return 17;
    if (m.rfind("look_l", 0) == 0) return 16;
    return 0;
}

// Iris's own voice (an original character), one set per mood
const char* const kIrisLines[kNMood][3] = {
    {"Optics online.", "Eyes open. Watching.", "Iris, awake."},
    {"Nice signal today.", "I'm all lit up.", "This is the good stuff."},
    {"Ooh, a new one!", "Found a friend!", "Blink blink!"},
    {"Shades on. Chill.", "Too cool to rush.", "Smooth scanning."},
    {"Flat line out here.", "Staring at static.", "Nudge me if it moves."},
    {"Signal's lonely.", "Nobody to look at.", "Low light..."},
    {"Anyone out there?", "Just me and the hum.", "An empty frame."},
    {"Hey! Come back!", "That flicked off.", "Grr, static."},
    {"Dimming optics...", "Powering the lens down.", "Zzz."},
    {"Zoomed in. Sharp.", "Full focus.", "I see everything."},
    {"Glad you're watching.", "Thanks for the light.", "Good to be on."},
    {"New angle, let's look.", "Panning around.", "Onward, lens first."},
    {"Lens fog today.", "Can't be bothered.", "Meh, static."},
    {"Another Iris!", "Hi, fellow optic.", "Twin lenses!"},
    {"Mapping the noise.", "I see the pattern.", "Learned that one."},
    {"Lens cracked!", "Glitch in my eye.", "Ow, static burst."},
    {"Peeking left.", "What's that side?", "Panning left."},
    {"Peeking right.", "And over there?", "Panning right."},
};

std::string home_dir() {
#ifndef __EMSCRIPTEN__
    const char* h = std::getenv("HOME");
    if (h && *h) return h;
#endif
    return "/root";
}

// A small animation that stands in for the companion when the real one isn't present (and always
// in Optic Suite / the web build). Cycles mood and a few counters so the page can be tried out.
class Demo {
public:
    void start() {
        on_ = true;
        t0_ = std::time(nullptr);
        next_ = 0;
        found_ = 0;
        mood_ = 0;
        ch_ = 1;
        seen_ = 0;
    }
    void stop() { on_ = false; }
    bool on() const { return on_; }

    PwnState read() {
        PwnState s;
        if (!on_) return s;
        const time_t now = std::time(nullptr);
        if (now >= next_) step(now);
        s.live = s.demo = true;
        s.mood = kMoods[mood_];
        s.line = kLines[mood_][line_];
        char b[48];
        std::snprintf(b, sizeof b, "%d", ch_); s.ch = b;
        std::snprintf(b, sizeof b, "%d", seen_); s.aps = b;
        std::snprintf(b, sizeof b, "%d", found_); s.pwnd = b;
        std::snprintf(b, sizeof b, "%d", 7 + found_); s.total = b;
        s.peers = mood_ == 13 ? "1" : "0";
        const long up = static_cast<long>(now - t0_);
        std::snprintf(b, sizeof b, "%02ld:%02ld:%02ld", up / 3600, up / 60 % 60, up % 60);
        s.uptime = b;
        s.mode = "DEMO";
        return s;
    }

private:
    void step(time_t now) {
        ch_ = ch_ % 13 + 1;
        seen_ += (std::rand() % 5) - 2;
        if (seen_ < 0) seen_ = 0;
        if (seen_ > 24) seen_ = 24;
        const long up = static_cast<long>(now - t0_);
        const int r = std::rand() % 100;
        if (up < 6) mood_ = 0;
        else if (seen_ > 6 && r < 18) { mood_ = 2; ++found_; }
        else if (r < 30) mood_ = 1;
        else if (r < 40) mood_ = 16 + std::rand() % 2;
        else if (r < 48) mood_ = seen_ < 3 ? 6 : 4;
        else if (r < 54) mood_ = 8;
        else if (r < 60) mood_ = 3;
        else if (r < 65) mood_ = 9;
        else if (r < 70) mood_ = 14;
        else if (r < 74) mood_ = 5;
        else if (r < 77) mood_ = 7;
        else if (r < 81) mood_ = 10;
        else if (r < 85) mood_ = 11;
        else if (r < 88) mood_ = 12;
        else if (r < 92) mood_ = 13;
        else mood_ = 1;
        line_ = std::rand() % 3;
        next_ = now + 4 + std::rand() % 4;
    }
    bool on_{false};
    time_t t0_{0}, next_{0};
    int mood_{0}, line_{0}, ch_{1}, seen_{0}, found_{0};
};

// ---- the skin on disk: faces/<mood>.png, lines/<mood>.txt, name.txt, ABOUT.txt ----
class Skins {
public:
    Skins() { load_conf(); scan(); load(); }

    std::string name() const { return name_; }
    std::string notice() const { return notice_; }

    std::string face(const std::string& mood) const {
#ifdef __EMSCRIPTEN__
        (void)mood;
        return "";
#else
        std::string p = dir_ + "/faces/" + mood + ".png";
        struct stat st;
        if (::stat(p.c_str(), &st) != 0) return "";
        return "A:" + p;   // LVGL stdio driver, letter 'A'
#endif
    }
    std::string line(int mood_idx) const {
        auto it = lines_.find(mood_idx);
        if (it == lines_.end() || it->second.empty()) return "";
        return it->second[std::rand() % it->second.size()];
    }

    void next() {
        scan();   // pick up skins added since the page opened
        if (list_.empty()) return;
        size_t i = 0;
        for (; i < list_.size(); ++i)
            if (list_[i] == skin_) break;
        skin_ = list_[(i + 1) % list_.size()];
        save_conf();
        load();
    }

private:
    void conf_path(char* o, size_t n) const { std::snprintf(o, n, "%s/.config/optic-pwn/conf", home_dir().c_str()); }
    void load_conf() {
#ifndef __EMSCRIPTEN__
        char p[512];
        conf_path(p, sizeof p);
        FILE* f = std::fopen(p, "r");
        if (!f) return;
        char l[256];
        while (std::fgets(l, sizeof l, f)) {
            l[strcspn(l, "\r\n")] = 0;
            if (!std::strncmp(l, "skin=", 5)) skin_ = l + 5;
        }
        std::fclose(f);
#endif
    }
    void save_conf() {
#ifndef __EMSCRIPTEN__
        std::string d = home_dir() + "/.config/optic-pwn";
        ::mkdir((home_dir() + "/.config").c_str(), 0755);
        ::mkdir(d.c_str(), 0755);
        char p[512];
        conf_path(p, sizeof p);
        // keep the other keys Optic DE writes
        std::string mode = "manual";
        FILE* r = std::fopen(p, "r");
        if (r) {
            char l[256];
            while (std::fgets(l, sizeof l, r))
                if (!std::strncmp(l, "mode=", 5)) { mode = l + 5; mode.erase(mode.find_last_not_of("\r\n") + 1); }
            std::fclose(r);
        }
        FILE* f = std::fopen(p, "w");
        if (f) { std::fprintf(f, "skin=%s\nmode=%s\n", skin_.c_str(), mode.c_str()); std::fclose(f); }
#endif
    }
    void scan() {
        list_.clear();
        list_.push_back("construct");
        list_.push_back("iris");   // a built-in drawn character (works in every build)
#ifndef __EMSCRIPTEN__
        std::string root = home_dir() + "/.local/share/optic-pwn/skins";
        DIR* d = ::opendir(root.c_str());
        if (!d) return;
        struct dirent* e;
        while ((e = ::readdir(d)))
            if (e->d_name[0] != '.' && std::strcmp(e->d_name, "construct")) list_.push_back(e->d_name);
        ::closedir(d);
#endif
    }
    static std::string read_small(const std::string& path) {
        std::string out;
#ifndef __EMSCRIPTEN__
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return out;
        char b[1024];
        size_t n = std::fread(b, 1, sizeof b - 1, f);
        b[n] = 0;
        out = b;
        std::fclose(f);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
#else
        (void)path;
#endif
        return out;
    }
    void load() {
        lines_.clear();
        notice_.clear();
#ifndef __EMSCRIPTEN__
        dir_ = home_dir() + "/.local/share/optic-pwn/skins/" + skin_;
        name_ = read_small(dir_ + "/name.txt");
        std::string about = read_small(dir_ + "/ABOUT.txt");
        if (!about.empty()) notice_ = about.substr(0, about.find('\n'));
        for (int i = 0; i < kNMood; ++i) {
            FILE* f = std::fopen((dir_ + "/lines/" + kMoods[i] + ".txt").c_str(), "r");
            if (!f) continue;
            char l[300];
            while (std::fgets(l, sizeof l, f)) {
                l[strcspn(l, "\r\n")] = 0;
                if (l[0]) lines_[i].push_back(l);
            }
            std::fclose(f);
        }
#endif
        if (name_.empty()) name_ = skin_ == "construct" ? "Construct" : skin_ == "iris" ? "Iris" : skin_;
    }

    std::string skin_{"iris"}, dir_, name_, notice_;
    std::vector<std::string> list_;
    std::map<int, std::vector<std::string>> lines_;
};

// Reads the status file the Optic plugin (or Optic DE's Pwn) writes, and runs the local demo
// when nothing real is present.
class PwnImpl : public Pwn {
public:
    PwnState read() override {
        PwnState s = from_file();
        if (!s.live) s = demo_.read();   // the demo fills in on the phone / web / a PC without it
        if (!s.live) { line_mood_ = -99; return s; }
        const int mi = mood_index(s.mood);
        last_mood_ = mi;
        const std::string sk = skins_.name();
        if (sk != last_skin_) { last_skin_ = sk; line_mood_ = -99; }   // fresh line after a skin switch
        if (mi != line_mood_) {          // re-pick the line only when the mood changes (no flicker)
            line_mood_ = mi;
            std::string sl = skins_.line(mi);                                  // the skin's own file lines win
            if (sl.empty() && skins_.name() == "Iris") sl = kIrisLines[mi][std::rand() % 3];
            cached_line_ = !sl.empty() ? sl : (!s.line.empty() ? s.line : kLines[mi][0]);
        }
        s.line = cached_line_;
        return s;
    }
    void toggle() override {
        PwnState s = from_file();
        const bool real = have_real();
        if (s.live && real) run("optic-pwn stop");
        else if (real) run("optic-pwn start");
        else if (demo_.on()) demo_.stop();
        else demo_.start();
    }
    void next_skin() override { skins_.next(); }
    std::string skin_name() override { return skins_.name(); }
    std::string notice() override { return skins_.notice(); }
    std::string face_file(const std::string& mood) override { return skins_.face(mood); }

private:
    static bool have_real() {
#ifndef __EMSCRIPTEN__
        return ::access("/usr/local/bin/pwnagotchi", X_OK) == 0 || ::access("/usr/bin/pwnagotchi", X_OK) == 0;
#else
        return false;
#endif
    }
    static void run(const char* cmd) {
#ifndef __EMSCRIPTEN__
        if (std::system(cmd)) { /* best effort */ }
#else
        (void)cmd;
#endif
    }
    PwnState from_file() {
        PwnState s;
#ifndef __EMSCRIPTEN__
        struct stat st;
        if (::stat("/tmp/optic-pwn.status", &st) != 0) return s;
        if (std::time(nullptr) - st.st_mtime > 20) return s;   // gone stale: treat as off
        FILE* f = std::fopen("/tmp/optic-pwn.status", "r");
        if (!f) return s;
        char l[256];
        std::string state;
        while (std::fgets(l, sizeof l, f)) {
            l[strcspn(l, "\r\n")] = 0;
            char* eq = std::strchr(l, '=');
            if (!eq) continue;
            *eq = 0;
            const char* v = eq + 1;
            if (!std::strcmp(l, "state")) state = v;
            else if (!std::strcmp(l, "mood")) s.mood = v;
            else if (!std::strcmp(l, "line")) s.line = v;
            else if (!std::strcmp(l, "ch")) s.ch = v;
            else if (!std::strcmp(l, "aps")) s.aps = v;
            else if (!std::strcmp(l, "pwnd")) s.pwnd = v;
            else if (!std::strcmp(l, "pwnd_total")) s.total = v;
            else if (!std::strcmp(l, "peers")) s.peers = v;
            else if (!std::strcmp(l, "uptime")) s.uptime = v;
            else if (!std::strcmp(l, "mode")) s.mode = v;
            else if (!std::strcmp(l, "source")) s.demo = (std::string(v) == "demo");
        }
        std::fclose(f);
        s.live = (state == "on");
#endif
        return s;
    }

    Demo demo_;
    Skins skins_;
    int last_mood_{0};
    int line_mood_{-99};
    std::string cached_line_, last_skin_;
};

}  // namespace

std::unique_ptr<Pwn> make_pwn() { return std::make_unique<PwnImpl>(); }

}  // namespace velvet
