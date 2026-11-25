#include <vector>
#include <cctype>
#include "device.h"
#include "json_lite.h"

#include "lvgl.h"
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#define VELVET_HAVE_REAL_DEVICE 1
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace velvet {
namespace {

// ------------------------------------------------------------------ main-thread queue
// Worker threads post closures here; an lv_timer runs them on the LVGL thread.
class UiQueue {
public:
    UiQueue() { timer_ = lv_timer_create(drain_cb, 30, this); }
    ~UiQueue() {
        if (timer_) lv_timer_delete(timer_);
        std::lock_guard<std::mutex> lock(state_->mu);
        state_->alive = false;
        state_->items.clear();
    }

    // Safe to call from any thread, even after the queue is gone (the item is dropped).
    std::function<void(std::function<void()>)> poster() {
        auto st = state_;
        return [st](std::function<void()> fn) {
            std::lock_guard<std::mutex> lock(st->mu);
            if (st->alive) st->items.push_back(std::move(fn));
        };
    }

    // Run `fn` on the UI thread after `ms` (used by the demo backend).
    static void later(uint32_t ms, std::function<void()> fn) {
        auto* box = new std::function<void()>(std::move(fn));
        lv_timer_t* t = lv_timer_create(
            [](lv_timer_t* t) {
                auto* f = static_cast<std::function<void()>*>(lv_timer_get_user_data(t));
                (*f)();
                delete f;
            },
            ms, box);
        lv_timer_set_repeat_count(t, 1);
    }

private:
    struct State {
        std::mutex mu;
        std::vector<std::function<void()>> items;
        bool alive{true};
    };

    static void drain_cb(lv_timer_t* t) {
        auto* self = static_cast<UiQueue*>(lv_timer_get_user_data(t));
        std::vector<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> lock(self->state_->mu);
            run.swap(self->state_->items);
        }
        for (auto& fn : run) fn();
    }

    std::shared_ptr<State> state_ = std::make_shared<State>();
    lv_timer_t* timer_{};
};

int clamp_pct(int v) { return std::max(0, std::min(100, v)); }

// ------------------------------------------------------------------ demo backend
class DemoDevice : public Device {
public:
    DemoDevice() {
        aps_ = {
            {"Zedward-5G", 86, true, true, true},
            {"CafeNet", 64, true, false, false},
            {"Library-Guest", 52, false, false, false},
            {"Neighbor_2.4", 38, true, false, false},
            {"ESP32-LoRa-AP", 30, true, false, false},
            {"DIRECT-HP-Printer", 18, true, false, false},
        };
        status_ = {true, true, "Zedward-5G", "192.168.1.42", 86};
    }

    void net_status(std::function<void(NetStatus)> cb) override {
        auto st = status_;
        UiQueue::later(60, [cb, st] { cb(st); });
    }

    void wifi_scan(std::function<void(std::vector<WifiAp>)> cb) override {
        std::vector<WifiAp> out;
        if (status_.radio_on) {
            out = aps_;
            for (auto& ap : out) ap.signal = clamp_pct(ap.signal + (std::rand() % 7) - 3);
        }
        UiQueue::later(1100, [cb, out] { cb(out); });
    }

    void wifi_connect(const std::string& ssid, const std::string& pw, Done cb) override {
        auto it = find(ssid);
        bool ok = it != aps_.end();
        std::string msg = ok ? "" : "Network not found";
        if (ok && it->secure && !it->saved && pw.size() < 8) {
            ok = false;
            msg = "Password needs 8+ characters";
        } else if (ok && ssid == "Neighbor_2.4") {
            ok = false;  // shows the failure path in the demo
            msg = "Wrong password";
        }
        if (ok) {
            for (auto& ap : aps_) ap.active = false;
            it->active = true;
            it->saved = true;
            status_ = {true, true, ssid, "192.168.1." + std::to_string(20 + std::rand() % 200), it->signal};
            msg = "Connected";
        }
        UiQueue::later(1800, [cb, ok, msg] { cb(ok, msg); });
    }

    void wifi_disconnect(Done cb) override {
        for (auto& ap : aps_) ap.active = false;
        status_.connected = false;
        status_.ssid.clear();
        status_.ip.clear();
        UiQueue::later(500, [cb] { cb(true, "Disconnected"); });
    }

    void wifi_forget(const std::string& ssid, Done cb) override {
        auto it = find(ssid);
        if (it != aps_.end()) {
            it->saved = false;
            if (it->active) {
                it->active = false;
                status_.connected = false;
                status_.ssid.clear();
                status_.ip.clear();
            }
        }
        UiQueue::later(400, [cb] { cb(true, "Forgotten"); });
    }

    void wifi_set_radio(bool on, Done cb) override {
        status_.radio_on = on;
        if (!on) {
            for (auto& ap : aps_) ap.active = false;
            status_.connected = false;
            status_.ssid.clear();
            status_.ip.clear();
        }
        UiQueue::later(500, [cb, on] { cb(true, on ? "Wi-Fi on" : "Wi-Fi off"); });
    }

    PowerInfo power() override {
        const uint32_t s = lv_tick_get() / 60000;  // drains 1% a minute, for show
        return {true, std::max(5, 78 - static_cast<int>(s)), 3912, false};
    }
    int brightness() override { return brightness_; }
    bool set_brightness(int p) override { brightness_ = clamp_pct(p); return true; }
    int volume() override { return volume_; }
    bool set_volume(int p) override { volume_ = clamp_pct(p); return true; }
    std::string hostname() override { return "cardputerzero"; }
    bool is_demo() const override { return true; }
    // a pretend screen, so the HDMI page can be seen and tried on the PC and in the browser
    HdmiInfo hdmi() override { return {true, true, hdmi_on_, hdmi_on_ ? "1920x1080" : "", hdmi_on_ ? "60" : ""}; }
    void set_hdmi(bool on) override { hdmi_on_ = on; }

private:
    bool hdmi_on_{false};
    std::vector<WifiAp>::iterator find(const std::string& ssid) {
        return std::find_if(aps_.begin(), aps_.end(), [&](const WifiAp& a) { return a.ssid == ssid; });
    }
    std::vector<WifiAp> aps_;
    NetStatus status_;
    int brightness_{70};
    int volume_{50};
};

#if defined(__EMSCRIPTEN__)
// ------------------------------------------------------------------ Optic Suite (the phone)
// The Wi-Fi page and the battery from the phone itself (VelvetNet.kt, JSON). Android doesn't let
// apps join or leave networks: those open its Wi-Fi panel. Brightness / volume stay the demo's.
EM_JS(int, dv_has, (const char* m), { return (typeof VelvetAndroid !== 'undefined' && VelvetAndroid[UTF8ToString(m)]) ? 1 : 0; });
EM_JS(char*, dv_call, (const char* m), {
    try { return stringToNewUTF8(String(VelvetAndroid[UTF8ToString(m)]())); } catch (e) { return stringToNewUTF8(""); }
});
EM_JS(int, dv_int, (const char* m), { try { return VelvetAndroid[UTF8ToString(m)]() | 0; } catch (e) { return -1; } });
EM_JS(void, dv_set, (const char* m, int v), { try { VelvetAndroid[UTF8ToString(m)](v); } catch (e) {} });
EM_JS_DEPS(velvet_device, "$stringToNewUTF8,$UTF8ToString");
std::string dv_str(const char* m) { char* p = dv_call(m); std::string r = p ? p : ""; free(p); return r; }

class PhoneDevice final : public DemoDevice {
public:
    void net_status(std::function<void(NetStatus)> cb) override {
        const J j = json_parse(dv_str("netWifi"));
        NetStatus st;
        st.radio_on = j["radio"].b;
        st.connected = j["connected"].b;
        st.ssid = j["ssid"].s;
        st.ip = j["ip"].s;
        st.signal = static_cast<int>(j["signal"].n);
        UiQueue::later(30, [cb, st] { cb(st); });
    }
    void wifi_scan(std::function<void(std::vector<WifiAp>)> cb) override {
        // asks for a fresh scan, then reads what Android has a moment later
        dv_str("netWifiScan");
        UiQueue::later(1500, [cb] {
            const J j = json_parse(dv_str("netWifiScan"));
            std::vector<WifiAp> out;
            for (auto& o : j.a) {
                WifiAp ap;
                ap.ssid = o["ssid"].s;
                ap.signal = static_cast<int>(o["signal"].n);
                ap.secure = o["secure"].b;
                ap.active = o["active"].b;
                ap.saved = ap.active;
                out.push_back(ap);
            }
            cb(out);
        });
    }
    void wifi_connect(const std::string&, const std::string&, Done cb) override { panel(cb); }
    void wifi_disconnect(Done cb) override { panel(cb); }
    void wifi_forget(const std::string&, Done cb) override { panel(cb); }
    void wifi_set_radio(bool, Done cb) override { panel(cb); }
    PowerInfo power() override {
        const J j = json_parse(dv_str("netBattery"));
        if (!j["valid"].b) return DemoDevice::power();
        return {true, static_cast<int>(j["percent"].n), static_cast<int>(j["mv"].n), j["charging"].b};
    }
    std::string hostname() override { return "optic-suite"; }
    bool real_wifi() const override { return true; }
    // the phone's own screen brightness and media volume
    int brightness() override { return dv_has("netBrightness") ? dv_int("netBrightness") : DemoDevice::brightness(); }
    bool set_brightness(int p) override {
        if (!dv_has("netSetBrightness")) return DemoDevice::set_brightness(p);
        dv_set("netSetBrightness", clamp_pct(p));
        return true;
    }
    int volume() override { return dv_has("netVolume") ? dv_int("netVolume") : DemoDevice::volume(); }
    bool set_volume(int p) override {
        if (!dv_has("netSetVolume")) return DemoDevice::set_volume(p);
        dv_set("netSetVolume", clamp_pct(p));
        return true;
    }
    bool is_demo() const override { return false; }   // everything System shows is the phone's

private:
    static void panel(Done cb) {
        dv_str("netWifiPanel");
        UiQueue::later(300, [cb] { cb(false, "Finish it in Android's Wi-Fi panel"); });
    }
};
#endif

#if VELVET_HAVE_REAL_DEVICE
// ------------------------------------------------------------------ real backend helpers
struct RunResult {
    int code{-1};
    std::string out;  // stdout + stderr
};

// fork/exec with an argv list: arguments are never parsed by a shell.

RunResult run(const std::vector<std::string>& argv) {
    RunResult r;
    // Everything the child needs is prepared before fork(): this process has threads,
    // so the child may only call async-signal-safe functions before exec.
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    static const char kLang[] = "LC_ALL=C";  // stable, untranslated nmcli/amixer output
    std::vector<char*> env;
    for (char** e = ::environ; e && *e; ++e) {
        if (std::strncmp(*e, "LC_ALL=", 7) != 0) env.push_back(*e);
    }
    env.push_back(const_cast<char*>(kLang));
    env.push_back(nullptr);
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return r;
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return r;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        execvpe(args[0], args.data(), env.data());
        _exit(127);
    }
    close(fds[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) r.out.append(buf, static_cast<size_t>(n));
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find('\n', start);
        if (end == std::string::npos) end = s.size();
        if (end > start) out.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

// Split one line of `nmcli -t` output: fields are ':'-separated, with '\:' and '\\' escaped.
std::vector<std::string> nm_fields(const std::string& line) {
    std::vector<std::string> f(1);
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            f.back() += line[++i];
        } else if (c == ':') {
            f.emplace_back();
        } else {
            f.back() += c;
        }
    }
    return f;
}

std::string first_line(const std::string& s) {
    std::string l = s.substr(0, s.find('\n'));
    // nmcli prefixes errors with "Error: "
    if (l.rfind("Error: ", 0) == 0) l = l.substr(7);
    if (l.size() > 60) l = l.substr(0, 60);
    return l;
}

bool read_long(const std::string& path, long& out) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    const bool ok = std::fscanf(f, "%ld", &out) == 1;
    std::fclose(f);
    return ok;
}

std::string read_str(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return {};
    char buf[128] = {};
    if (!std::fgets(buf, sizeof buf, f)) buf[0] = 0;
    std::fclose(f);
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

NetStatus query_status() {
    NetStatus st;
    st.radio_on = run({"nmcli", "radio", "wifi"}).out.rfind("enabled", 0) == 0;
    std::string dev;
    for (const auto& l : lines(run({"nmcli", "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device"}).out)) {
        const auto f = nm_fields(l);
        if (f.size() >= 4 && f[1] == "wifi" && f[2] == "connected") {
            dev = f[0];
            st.connected = true;
            st.ssid = f[3];
            break;
        }
    }
    if (!st.connected) return st;
    for (const auto& l : lines(run({"nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL", "device", "wifi", "list", "--rescan", "no"}).out)) {
        const auto f = nm_fields(l);
        if (f.size() >= 3 && f[0] == "*") {
            st.ssid = f[1];
            st.signal = std::atoi(f[2].c_str());
            break;
        }
    }
    const std::string ip = first_line(run({"nmcli", "-g", "IP4.ADDRESS", "device", "show", dev}).out);
    st.ip = ip.substr(0, ip.find('/'));
    st.ip = st.ip.substr(0, st.ip.find(' '));
    return st;
}

std::vector<WifiAp> query_scan() {
    std::map<std::string, bool> saved;
    for (const auto& l : lines(run({"nmcli", "-t", "-f", "NAME,TYPE", "connection", "show"}).out)) {
        const auto f = nm_fields(l);
        if (f.size() >= 2 && f[1] == "802-11-wireless") saved[f[0]] = true;
    }
    std::vector<WifiAp> aps;
    const auto out = run({"nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "device", "wifi", "list", "--rescan", "yes"}).out;
    for (const auto& l : lines(out)) {
        const auto f = nm_fields(l);
        if (f.size() < 4 || f[1].empty()) continue;  // hidden networks have no SSID
        WifiAp ap;
        ap.active = f[0] == "*";
        ap.ssid = f[1];
        ap.signal = std::atoi(f[2].c_str());
        ap.secure = !f[3].empty() && f[3] != "--";
        ap.saved = saved.count(ap.ssid) > 0;
        // one row per SSID: keep the strongest / active access point
        auto it = std::find_if(aps.begin(), aps.end(), [&](const WifiAp& a) { return a.ssid == ap.ssid; });
        if (it == aps.end()) {
            aps.push_back(ap);
        } else {
            it->active = it->active || ap.active;
            it->signal = std::max(it->signal, ap.signal);
        }
    }
    std::stable_sort(aps.begin(), aps.end(), [](const WifiAp& a, const WifiAp& b) {
        if (a.active != b.active) return a.active;
        if (a.saved != b.saved) return a.saved;
        return a.signal > b.signal;
    });
    return aps;
}

// ------------------------------------------------------------------ real backend
class LinuxDevice final : public Device {
public:
    LinuxDevice() : post_(queue_.poster()) {}

    void net_status(std::function<void(NetStatus)> cb) override {
        async([cb] { return [cb, st = query_status()] { cb(st); }; });
    }

    void wifi_scan(std::function<void(std::vector<WifiAp>)> cb) override {
        async([cb] { return [cb, aps = query_scan()] { cb(aps); }; });
    }

    void wifi_connect(const std::string& ssid, const std::string& pw, Done cb) override {
        async([cb, ssid, pw] {
            RunResult r;
            if (pw.empty()) {
                // saved profile first; for an open network this falls through to a new connection
                r = run({"nmcli", "-w", "30", "connection", "up", "id", ssid});
                if (r.code != 0) r = run({"nmcli", "-w", "30", "device", "wifi", "connect", ssid});
            } else {
                // a stale profile with an old password would win over the new one: drop it first
                run({"nmcli", "connection", "delete", "id", ssid});
                r = run({"nmcli", "-w", "30", "device", "wifi", "connect", ssid, "password", pw});
            }
            const bool ok = r.code == 0;
            std::string msg = ok ? "Connected" : first_line(r.out);
            if (!ok && msg.find("Secrets were required") != std::string::npos) msg = "Wrong password";
            if (!ok && !pw.empty()) {
                // don't keep a profile with a bad password: the next try should ask again
                run({"nmcli", "connection", "delete", "id", ssid});
            }
            if (!ok && msg.empty()) msg = "Failed (code " + std::to_string(r.code) + ")";
            return [cb, ok, msg] { cb(ok, msg); };
        });
    }

    void wifi_disconnect(Done cb) override {
        async([cb] {
            std::string dev;
            for (const auto& l : lines(run({"nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "device"}).out)) {
                const auto f = nm_fields(l);
                if (f.size() >= 3 && f[1] == "wifi" && f[2] == "connected") dev = f[0];
            }
            if (dev.empty()) return std::function<void()>([cb] { cb(true, "Not connected"); });
            const auto r = run({"nmcli", "device", "disconnect", dev});
            const bool ok = r.code == 0;
            const std::string msg = ok ? "Disconnected" : first_line(r.out);
            return std::function<void()>([cb, ok, msg] { cb(ok, msg); });
        });
    }

    void wifi_forget(const std::string& ssid, Done cb) override {
        async([cb, ssid] {
            const auto r = run({"nmcli", "connection", "delete", "id", ssid});
            const bool ok = r.code == 0;
            const std::string msg = ok ? "Forgotten" : first_line(r.out);
            return [cb, ok, msg] { cb(ok, msg); };
        });
    }

    void wifi_set_radio(bool on, Done cb) override {
        async([cb, on] {
            const auto r = run({"nmcli", "radio", "wifi", on ? "on" : "off"});
            const bool ok = r.code == 0;
            const std::string msg = ok ? (on ? "Wi-Fi on" : "Wi-Fi off") : first_line(r.out);
            return [cb, ok, msg] { cb(ok, msg); };
        });
    }

    PowerInfo power() override {
        PowerInfo p;
        const std::string dir = battery_dir();
        if (dir.empty()) return p;
        long cap = 0, uv = 0;
        if (!read_long(dir + "/capacity", cap)) return p;
        read_long(dir + "/voltage_now", uv);
        p.valid = true;
        p.percent = static_cast<int>(cap);
        p.millivolts = static_cast<int>(uv / 1000);
        p.charging = read_str(dir + "/status") == "Charging";
        return p;
    }

    int brightness() override {
        long cur = 0, max = 0;
        if (!read_long(kBacklight + std::string("/brightness"), cur) ||
            !read_long(kBacklight + std::string("/max_brightness"), max) || max <= 0) {
            return -1;
        }
        return static_cast<int>((cur * 100 + max / 2) / max);
    }

    bool set_brightness(int percent) override {
        long max = 0;
        if (!read_long(kBacklight + std::string("/max_brightness"), max) || max <= 0) return false;
        // never fully dark: the screen would look dead
        const long v = std::max(1L, (clamp_pct(percent) * max + 50) / 100);
        FILE* f = std::fopen((kBacklight + std::string("/brightness")).c_str(), "w");
        if (!f) return false;
        const bool ok = std::fprintf(f, "%ld", v) > 0;
        return std::fclose(f) == 0 && ok;
    }

    // Volume through PipeWire, as the stock launcher does now (cp0_audio_mixer_control.cpp). The
    // old "Headphone Playback Volume" (0..63) belonged to the TPA6130A2 amp on the V0.3 board only;
    // production V0.5 boards have the ES8389 codec, whose DAC attenuator PipeWire drives.
    // The left and right levels of the default output, in percent (empty if pactl can't say).
    static std::vector<int> channel_volumes() {
        std::vector<int> v;
        auto r = run({"pactl", "get-sink-volume", "@DEFAULT_SINK@"});   // "Volume: front-left: 14156 /  22% / -40 dB,   front-right: ..."
        if (r.code != 0) return v;
        const size_t eol = r.out.find('\n');
        const std::string line = r.out.substr(0, eol);
        for (size_t pos = line.find('%'); pos != std::string::npos; pos = line.find('%', pos + 1)) {
            size_t st = pos;
            while (st > 0 && std::isdigit((unsigned char)line[st - 1])) st--;
            if (st < pos) v.push_back(std::atoi(line.c_str() + st));
        }
        return v;
    }

    // The volume is the louder side: with a balance set (one ear turned down), that is the
    // number the user thinks of as "the volume".
    int volume() override {
        const std::vector<int> ch = channel_volumes();
        if (!ch.empty()) return *std::max_element(ch.begin(), ch.end());
        auto r = run({"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"});        // "Volume: 0.22"
        const size_t pos = r.out.find("Volume:");
        if (r.code != 0 || pos == std::string::npos) return -1;
        return (int)(std::atof(r.out.c_str() + pos + 7) * 100 + 0.5);
    }

    // One volume for the whole device: the sound server's (PipeWire's default sink), the same value
    // M5's launcher shows and changes. The launcher also keeps it in ~/.config/cardputerzero/config.json
    // ("volume") and puts that back when it starts, so a change made here is written there too:
    // otherwise the next boot would undo it. A left/right balance set on the output is kept:
    // both sides move together, in proportion.
    bool set_volume(int percent) override {
        const int v = clamp_pct(percent);
        const std::vector<int> ch = channel_volumes();
        const int top = ch.empty() ? 0 : *std::max_element(ch.begin(), ch.end());
        std::vector<std::string> cmd = {"pactl", "set-sink-volume", "@DEFAULT_SINK@"};
        const bool balanced = ch.size() >= 2 && top > 0 && *std::min_element(ch.begin(), ch.end()) != top;
        if (balanced) for (int c : ch) cmd.push_back(std::to_string((v * c + top / 2) / top) + "%");
        else cmd.push_back(std::to_string(v) + "%");
        bool ok = run(cmd).code == 0;
        if (!ok) ok = run({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", std::to_string(v) + "%"}).code == 0;
        if (ok) save_launcher_volume(v);
        return ok;
    }

    static void save_launcher_volume(int v) {
        const char* home = std::getenv("HOME");
        if (!home) return;
        const std::string dir = std::string(home) + "/.config/cardputerzero", path = dir + "/config.json";
        if (access(dir.c_str(), W_OK) != 0) return;      // not a CardputerZero with M5's launcher
        std::string j = read_str(path);
        const std::string num = std::to_string(v);
        const size_t k = j.find("\"volume\"");
        if (k != std::string::npos) {                    // replace the number after "volume":
            size_t a0 = j.find(':', k);
            if (a0 == std::string::npos) return;
            ++a0;
            while (a0 < j.size() && (j[a0] == ' ' || j[a0] == '\t')) ++a0;
            size_t b0 = a0;
            while (b0 < j.size() && (std::isdigit(static_cast<unsigned char>(j[b0])) || j[b0] == '-')) ++b0;
            j = j.substr(0, a0) + num + j.substr(b0);
        } else {
            const size_t close = j.rfind('}');
            const size_t open = j.find('{');
            if (open == std::string::npos || close == std::string::npos) j = "{\n  \"volume\": " + num + "\n}\n";
            else {
                const bool empty = j.find_first_not_of(" \t\r\n", open + 1) == close;
                j = j.substr(0, close) + (empty ? "" : ",") + "\n  \"volume\": " + num + "\n" + j.substr(close);
            }
        }
        const std::string tmp = path + ".optic";
        if (FILE* f = std::fopen(tmp.c_str(), "w")) {
            std::fputs(j.c_str(), f);
            std::fclose(f);
            std::rename(tmp.c_str(), path.c_str());
        }
    }

    std::string hostname() override {
        char buf[64] = {};
        gethostname(buf, sizeof buf - 1);
        return buf;
    }

    // The Optic desktop on HDMI: optic-de-hdmi (from the optic-de package) starts and stops it and
    // says what is there ("status": connected=, running=, mode=, hz= lines).
    HdmiInfo hdmi() override {
        HdmiInfo h;
        if (access(kHdmiTool, X_OK) != 0) return h;
        h.supported = true;
        for (const std::string& l : lines(run({kHdmiTool, "status"}).out)) {
            const size_t eq = l.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
            if (k == "connected") h.connected = v == "1";
            else if (k == "running") h.running = v == "1";
            else if (k == "mode") h.mode = v;
            else if (k == "hz" && !v.empty()) h.hz = std::to_string(static_cast<int>(std::atof(v.c_str()) + 0.5));
        }
        return h;
    }
    void set_hdmi(bool on) override { run({kHdmiTool, on ? "start" : "stop"}); }

    bool is_demo() const override { return false; }

private:
    static constexpr const char* kBacklight = "/sys/class/backlight/backlight";
    static constexpr const char* kHdmiTool = "/opt/optic-de/bin/optic-de-hdmi";

    // Run `work` on a thread; it returns the closure to run on the UI thread.
    template <class Work>
    void async(Work work) {
        auto post = post_;
        std::thread([post, work]() mutable { post(std::function<void()>(work())); }).detach();
    }

    // The bq27220 fuel gauge, as found by the stock launcher.
    std::string battery_dir() {
        if (!battery_dir_.empty()) return battery_dir_;
        const char* base = "/sys/class/power_supply";
        DIR* d = opendir(base);
        if (!d) return {};
        std::string fallback;
        while (auto* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            const std::string dir = std::string(base) + "/" + e->d_name;
            if (access((dir + "/capacity").c_str(), R_OK) != 0) continue;
            if (std::strstr(e->d_name, "bq27")) {
                battery_dir_ = dir;
                break;
            }
            if (fallback.empty()) fallback = dir;
        }
        closedir(d);
        if (battery_dir_.empty()) battery_dir_ = fallback;
        return battery_dir_;
    }

    UiQueue queue_;
    std::function<void(std::function<void()>)> post_;
    std::string battery_dir_;
};
#endif

}  // namespace

std::unique_ptr<Device> make_device() {
#if VELVET_HAVE_REAL_DEVICE
#if USE_DESKTOP
    const char* real = std::getenv("VELVET_REAL_DEVICE");
    if (real && real[0] == '1') return std::make_unique<LinuxDevice>();
#else
    return std::make_unique<LinuxDevice>();
#endif
#endif
#if defined(__EMSCRIPTEN__)
    if (dv_has("netWifi")) return std::make_unique<PhoneDevice>();   // in Optic Suite: the phone's
#endif
    return std::make_unique<DemoDevice>();
}

}  // namespace velvet
