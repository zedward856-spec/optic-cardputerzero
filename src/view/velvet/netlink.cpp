#include "legacy.h"
#include "netlink.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
#include "olink.h"
}
#endif

namespace velvet {

static std::atomic<bool> g_tor_up{false};
std::string tor_proxy() { return g_tor_up ? "socks5h://127.0.0.1:9052" : ""; }

#ifdef __EMSCRIPTEN__
// ---------------------------------------------------------------- Optic Suite (VelvetAndroid)
EM_JS(int, nl_has, (const char* m), { return (typeof VelvetAndroid !== 'undefined' && VelvetAndroid[UTF8ToString(m)]) ? 1 : 0; });
EM_JS(char*, nl_call, (const char* m), {
    try { return stringToNewUTF8(String(VelvetAndroid[UTF8ToString(m)]())); } catch (e) { return stringToNewUTF8(""); }
});
EM_JS(void, nl_call1, (const char* m, int a), { try { VelvetAndroid[UTF8ToString(m)](a); } catch (e) {} });
EM_JS(void, nl_link_pump, (int on), { if (window.velvetLinkPump) window.velvetLinkPump(on); });
EM_JS_DEPS(velvet_netlink, "$stringToNewUTF8,$UTF8ToString");

static std::string call(const char* m) {
    char* s = nl_call(m);
    std::string r = s ? s : "";
    free(s);
    return r;
}

// torStatus(): "state pct error" (state as Tor::State)
class AndroidTor : public Tor {
public:
    int state() override { read(); return st_; }
    int percent() override { return pct_; }
    std::string error() override { return err_; }
    void toggle() override { nl_call1("torToggle", 0); }

private:
    void read() {
        if (!nl_has("torStatus")) { st_ = Unavailable; return; }
        const std::string s = call("torStatus");
        int st = 0, pct = 0, n = 0;
        if (std::sscanf(s.c_str(), "%d %d %n", &st, &pct, &n) >= 2) {
            st_ = st; pct_ = pct;
            err_ = n > 0 && n <= (int)s.size() ? s.substr(n) : "";
        }
        g_tor_up = st_ == On;
    }
    int st_{Off}, pct_{0};
    std::string err_;
};

// the page sends the pictures (it copies the canvas) and turns the R36S's buttons into keys
class AndroidLink : public ScreenLink {
public:
    int state() override {
        if (!nl_has("linkStatus")) return Unavailable;
        const std::string s = call("linkStatus");   // "state code peer"
        int st = 0;
        char code[8] = "";
        int n = 0;
        if (std::sscanf(s.c_str(), "%d %7s %n", &st, code, &n) >= 1) {
            code_ = code[0] == '-' ? "" : code;
            peer_ = n > 0 && n <= (int)s.size() ? s.substr(n) : "";
        }
        return st;
    }
    std::string code() override { return code_; }
    std::string peer() override { return peer_; }
    void set_on(bool on) override { nl_call1("linkSet", on ? 1 : 0); nl_link_pump(on && state() > 0 ? 1 : 0); }
    bool on() override { return state() > 0; }   // the app may have refused (port busy)
    bool wants_frame() override { return false; }
    void frame(const uint16_t*, int, int) override {}
    bool next_event(Event&) override { return false; }

private:
    std::string code_, peer_;
};

std::unique_ptr<Tor> make_tor() { return std::make_unique<AndroidTor>(); }
std::unique_ptr<ScreenLink> make_screen_link() { return std::make_unique<AndroidLink>(); }

#else
// ---------------------------------------------------------------- CardputerZero / PC

static std::string home() { const char* h = std::getenv("HOME"); return h && *h ? h : "/tmp"; }
static bool on_path(const char* exe) {
    std::string cmd = std::string("command -v ") + exe + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

class LinuxTor : public Tor {
public:
    // "On" = Tor running and wanted (the 'on' file). Turning it off only stops using it: Tor
    // stays up, warm, so turning it on again is instant instead of another bootstrap.
    LinuxTor() : dir_((adopt_old_dir(home() + "/.local/share/velvet-tor", home() + "/.local/share/optic-tor"), home() + "/.local/share/optic-tor")) {
        // a Tor from a previous run is still ours
        g_tor_up = wanted() && pid() > 0 && percent_now() == 100;
    }
    int state() override {
        if (installing_) return Installing;
        if (!have_) { if (!checked_) { have_ = on_path("tor"); checked_ = true; } if (!have_) return Missing; }
        if (pid() <= 0 || !wanted()) {
            g_tor_up = false;
            // off but still running (warm) for 15 minutes: stop it
            if (!wanted() && off_at_ && std::time(nullptr) - off_at_ > 15 * 60) {
                if (const int p = pid(); p > 0) kill(p, SIGTERM);
                std::remove((dir_ + "/tor.pid").c_str());
                off_at_ = 0;
            }
            return Off;
        }
        pct_ = percent_now();
        g_tor_up = pct_ == 100;
        return pct_ == 100 ? On : Starting;
    }
    int percent() override { return pct_; }
    std::string error() override { std::lock_guard<std::mutex> l(m_); return err_; }
    void toggle() override {
        if (installing_) return;
        { std::lock_guard<std::mutex> l(m_); err_.clear(); }
        if (pid() > 0) {
            set_wanted(!wanted());   // warm: just start or stop using it
            off_at_ = wanted() ? 0 : std::time(nullptr);
            g_tor_up = false;
            return;
        }
        set_wanted(true);
        if (!(have_ = on_path("tor"))) {
            // Pi OS lets the user install without a password; if not, say how
            installing_ = true;
            std::thread([this] {
                const int r = std::system("sudo -n apt-get install -y -q tor >/tmp/optic-tor-install.log 2>&1");
                have_ = on_path("tor");
                installing_ = false;
                if (!have_) { std::lock_guard<std::mutex> l(m_); err_ = r ? "NEEDS: sudo apt install tor" : "INSTALL FAILED"; return; }
                start();
            }).detach();
            return;
        }
        start();
    }

private:
    void start() {
        mkdir((home() + "/.local").c_str(), 0755);
        mkdir((home() + "/.local/share").c_str(), 0755);
        mkdir(dir_.c_str(), 0700);
        mkdir((dir_ + "/data").c_str(), 0700);
        std::FILE* f = std::fopen((dir_ + "/notices.log").c_str(), "w");
        if (f) std::fclose(f);
        // fewer disk writes (the SD card) and the mobile padding setting (battery)
        const std::string cmd = "tor --RunAsDaemon 1 --PidFile '" + dir_ + "/tor.pid' --DataDirectory '" + dir_ +
                                "/data' --SocksPort 127.0.0.1:9052 --ClientOnly 1 --AvoidDiskWrites 1"
                                " --ReducedConnectionPadding 1 --Log 'notice file " + dir_ + "/notices.log' >/dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0) { std::lock_guard<std::mutex> l(m_); err_ = "TOR WOULDN'T START"; }
    }
    bool wanted() { return access((dir_ + "/on").c_str(), F_OK) == 0; }
    void set_wanted(bool on) {
        if (on) {
            mkdir((home() + "/.local").c_str(), 0755);
            mkdir((home() + "/.local/share").c_str(), 0755);
            mkdir(dir_.c_str(), 0700);
            if (std::FILE* f = std::fopen((dir_ + "/on").c_str(), "w")) std::fclose(f);
        } else std::remove((dir_ + "/on").c_str());
    }
    int pid() {
        std::FILE* f = std::fopen((dir_ + "/tor.pid").c_str(), "r");
        if (!f) return 0;
        int p = 0;
        if (std::fscanf(f, "%d", &p) != 1) p = 0;
        std::fclose(f);
        return p > 0 && kill(p, 0) == 0 ? p : 0;
    }
    int percent_now() {
        std::FILE* f = std::fopen((dir_ + "/notices.log").c_str(), "r");
        if (!f) return 0;
        char line[512];
        int pct = 0;
        while (std::fgets(line, sizeof line, f)) {
            const char* b = std::strstr(line, "Bootstrapped ");
            if (b) pct = std::atoi(b + 13);
        }
        std::fclose(f);
        return pct;
    }
    std::string dir_;
    std::mutex m_;
    std::string err_;
    std::atomic<bool> installing_{false};
    bool have_{false}, checked_{false};
    std::time_t off_at_{0};   // when it was turned off while left running
    int pct_{0};
};

class LinuxLink : public ScreenLink {
public:
    ~LinuxLink() override { set_on(false); }
    int state() override {
        if (!l_) return Off;
        char code[5] = "", peer[64] = "";
        const int s = olink_state(l_, code, peer, sizeof peer);
        code_ = code;
        peer_ = peer;
        return s == OLS_LIVE ? Live : s == OLS_PAIRING ? Pairing : Waiting;
    }
    std::string code() override { return code_; }
    std::string peer() override { return peer_; }
    void set_on(bool on) override {
        if (on && !l_) {
            adopt_old_dir(home() + "/.config/velvet", home() + "/.config/optic-suite");
            const std::string dir = home() + "/.config/optic-suite";
            mkdir((home() + "/.config").c_str(), 0755);
            mkdir(dir.c_str(), 0755);
            char host[64] = "";
            gethostname(host, sizeof host - 1);
            std::string name = "CardputerZero", h = host;
            for (auto& ch : h) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (!h.empty() && h != "cardputerzero") name += " " + std::string(host);
            l_ = olink_start(name.c_str(), (dir + "/link-trusted").c_str());
        } else if (!on && l_) {
            olink_stop(l_);
            l_ = nullptr;
        }
    }
    bool on() override { return l_ != nullptr; }
    bool wants_frame() override { return l_ && olink_wants_frame(l_); }
    void frame(const uint16_t* px, int w, int h) override { if (l_) olink_frame(l_, px, w, h); }
    bool next_event(Event& e) override {
        OLinkEv v;
        if (!l_ || !olink_input(l_, &v)) return false;
        e = {v.type, v.code, v.value};
        return true;
    }

private:
    OLink* l_{nullptr};
    std::string code_, peer_;
};

std::unique_ptr<Tor> make_tor() { return std::make_unique<LinuxTor>(); }
std::unique_ptr<ScreenLink> make_screen_link() { return std::make_unique<LinuxLink>(); }
#endif

}  // namespace velvet
