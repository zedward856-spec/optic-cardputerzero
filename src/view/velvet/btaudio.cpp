#include "btaudio.h"

#ifdef __EMSCRIPTEN__
namespace velvet {
namespace {
class NoBtAudio final : public BtAudio {
public:
    BtAudioState state() override { return {}; }
    void refresh() override {}
    void scan() override {}
    void act(const std::string&) override {}
    void forget(const std::string&) override {}
    void set_default(int) override {}
    void move_stream(int, int) override {}
    void test() override {}
};
}  // namespace
std::unique_ptr<BtAudio> make_btaudio() { return std::make_unique<NoBtAudio>(); }
}  // namespace velvet
#else

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace velvet {
namespace {

bool on_path(const char* name) {
    for (const char* dir : {"/usr/bin/", "/usr/local/bin/", "/bin/", "/usr/sbin/"}) {
        if (access((std::string(dir) + name).c_str(), X_OK) == 0) return true;
    }
    return false;
}

// Runs a shell command, returns its output. Arguments that come from outside (MAC addresses,
// sink names) are checked by safe() before they get here.
std::string sh(const std::string& cmd) {
    std::string out;
    FILE* p = popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!p) return out;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

bool safe(const std::string& s) {
    if (s.empty() || s.size() > 120) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == ':' || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l)) v.push_back(l);
    return v;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// "Device AA:BB:CC:DD:EE:FF Name with spaces" lines -> mac -> name
std::map<std::string, std::string> device_lines(const std::string& out) {
    std::map<std::string, std::string> m;
    for (const std::string& l : lines(out)) {
        if (l.rfind("Device ", 0) != 0 || l.size() < 24) continue;
        const std::string mac = l.substr(7, 17);
        if (!safe(mac)) continue;
        m[mac] = l.size() > 25 ? trim(l.substr(25)) : "";
    }
    return m;
}

// A device that never told its name shows as its address with dashes: not worth a row.
bool named(const std::string& mac, const std::string& name) {
    if (name.empty()) return false;
    std::string dashed = mac;
    for (char& c : dashed) if (c == ':') c = '-';
    return name != dashed && name != mac;
}

void write_chime(const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    const int rate = 44100, per = static_cast<int>(rate * 0.42), notes = 3;
    const uint32_t data = static_cast<uint32_t>(per * notes * 4);
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(data);
    const double freq[3] = {523.25, 659.25, 783.99};
    for (int k = 0; k < notes; ++k) {
        for (int i = 0; i < per; ++i) {
            const double env = std::min(1.0, i / 700.0) * std::exp(-i / 8500.0);
            const int16_t v = static_cast<int16_t>(9000 * env * std::sin(2 * 3.14159265358979 * freq[k] * i / rate));
            // the first note left, the second right, the third both: also a left / right check
            const int16_t l = k == 1 ? 0 : v, r = k == 0 ? 0 : v;
            u16(static_cast<uint16_t>(l)); u16(static_cast<uint16_t>(r));
        }
    }
    std::fclose(f);
}

class LinuxBtAudio final : public BtAudio {
public:
    LinuxBtAudio() {
        have_ = on_path("bluetoothctl") && on_path("pactl");
        if (have_) worker_ = std::thread([this] { run(); });
    }
    ~LinuxBtAudio() override {
        {
            std::lock_guard<std::mutex> l(m_);
            quit_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (scanner_.joinable()) scanner_.join();
    }

    BtAudioState state() override {
        std::lock_guard<std::mutex> l(m_);
        BtAudioState s = st_;
        s.scanning = scanning_;
        s.busy = busy_ || !jobs_.empty();
        return s;
    }

    void refresh() override { post([this] { snapshot(); }, true); }

    void scan() override {
        if (!have_ || scanning_.exchange(true)) return;
        if (scanner_.joinable()) scanner_.join();
        scanner_ = std::thread([this] {
            sh("bluetoothctl power on");
            sh("bluetoothctl --timeout 10 scan on");
            scanning_ = false;
            refresh();
        });
    }

    void act(const std::string& mac) override {
        if (!safe(mac)) return;
        post([this, mac] {
            const std::string info = sh("bluetoothctl info " + mac);
            const bool paired = info.find("Paired: yes") != std::string::npos;
            const bool connected = info.find("Connected: yes") != std::string::npos;
            if (connected) {
                note("Disconnecting");
                sh("bluetoothctl disconnect " + mac);
                note("Disconnected");
            } else {
                if (!paired) {
                    note("Pairing...");
                    const std::string p = sh("bluetoothctl --timeout 20 pair " + mac);
                    if (p.find("Pairing successful") == std::string::npos && sh("bluetoothctl info " + mac).find("Paired: yes") == std::string::npos) {
                        note("Couldn't pair. Pairing mode?");
                        snapshot();
                        return;
                    }
                    sh("bluetoothctl trust " + mac);
                }
                note("Connecting");
                sh("bluetoothctl --timeout 15 connect " + mac);
                bool ok = false;
                for (int i = 0; i < 8 && !ok; ++i) {     // the sound output takes a moment to appear
                    usleep(500000);
                    ok = sh("bluetoothctl info " + mac).find("Connected: yes") != std::string::npos;
                }
                note(ok ? "Connected" : "Couldn't connect. Is it on?");
            }
            snapshot();
        });
    }

    void forget(const std::string& mac) override {
        if (!safe(mac)) return;
        post([this, mac] {
            sh("bluetoothctl remove " + mac);
            note("Forgotten");
            snapshot();
        });
    }

    void set_default(int output) override {
        post([this, output] {
            std::string sink;
            std::vector<int> ids;
            {
                std::lock_guard<std::mutex> l(m_);
                if (output < 0 || output >= static_cast<int>(st_.outputs.size())) return;
                sink = st_.outputs[output].sink;
                for (const auto& s : st_.streams) ids.push_back(s.id);
            }
            if (!safe(sink)) return;
            sh("pactl set-default-sink " + sink);
            for (int id : ids) sh("pactl move-sink-input " + std::to_string(id) + " " + sink);
            snapshot();
        });
    }

    void move_stream(int id, int output) override {
        post([this, id, output] {
            std::string sink;
            {
                std::lock_guard<std::mutex> l(m_);
                if (output < 0 || output >= static_cast<int>(st_.outputs.size())) return;
                sink = st_.outputs[output].sink;
            }
            if (!safe(sink)) return;
            sh("pactl move-sink-input " + std::to_string(id) + " " + sink);
            snapshot();
        });
    }

    void test() override {
        post([this] {
            const char* path = "/tmp/optic-chime.wav";
            if (access(path, R_OK) != 0) write_chime(path);
            note("Left, right, both");
            sh(std::string("paplay ") + path);
            note("");
        });
    }

private:
    void post(std::function<void()> job, bool only_if_idle = false) {
        if (!have_) return;
        {
            std::lock_guard<std::mutex> l(m_);
            if (only_if_idle && (busy_ || !jobs_.empty())) return;
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

    void note(const std::string& n) {
        std::lock_guard<std::mutex> l(m_);
        st_.note = n;
    }

    void run() {
        snapshot();
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> l(m_);
                busy_ = false;
                cv_.wait(l, [this] { return quit_ || !jobs_.empty(); });
                if (quit_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
                busy_ = true;
            }
            job();
        }
    }

    void snapshot() {
        BtAudioState s;
        s.available = sh("bluetoothctl list").find("Controller") != std::string::npos;
        // devices: paired first, then the rest that have a name
        const auto all = device_lines(sh("bluetoothctl devices"));
        const auto paired = device_lines(sh("bluetoothctl devices Paired"));
        const auto conn = device_lines(sh("bluetoothctl devices Connected"));
        for (const auto& [mac, name] : paired) s.devices.push_back({mac, name.empty() ? mac : name, true, conn.count(mac) > 0});
        for (const auto& [mac, name] : all) {
            if (paired.count(mac) || !named(mac, name)) continue;
            s.devices.push_back({mac, name, false, conn.count(mac) > 0});
        }
        // outputs
        const bool jack = sh("amixer -c 1 cget name='Headphones Jack'").find("values=on") != std::string::npos;
        std::map<int, int> by_index;
        for (const std::string& l : lines(sh("pactl list short sinks"))) {
            std::istringstream in(l);
            int idx;
            std::string name;
            if (!(in >> idx >> name) || !safe(name)) continue;
            AudioOutput o;
            o.sink = name;
            if (name.rfind("bluez_output.", 0) == 0) {
                o.bluetooth = true;
                std::string mac = name.substr(13, 17);
                for (char& c : mac) if (c == '_') c = ':';
                o.label = paired.count(mac) && !paired.at(mac).empty() ? paired.at(mac) : "Bluetooth";
            } else if (name.find("hdmi") != std::string::npos) {
                o.label = "HDMI";
            } else {
                o.label = jack ? "Headphone jack" : "Speaker";
            }
            by_index[idx] = static_cast<int>(s.outputs.size());
            s.outputs.push_back(o);
        }
        const std::string def = trim(sh("pactl get-default-sink"));
        for (size_t i = 0; i < s.outputs.size(); ++i) if (s.outputs[i].sink == def) s.default_output = static_cast<int>(i);
        // what is playing, and where
        AudioStream cur;
        bool open = false;
        auto close = [&] {
            if (open && !cur.app.empty()) s.streams.push_back(cur);
            open = false;
        };
        for (const std::string& raw : lines(sh("pactl list sink-inputs"))) {
            const std::string l = trim(raw);
            if (l.rfind("Sink Input #", 0) == 0) {
                close();
                cur = AudioStream{};
                cur.id = std::atoi(l.c_str() + 12);
                open = true;
            } else if (l.rfind("Sink: ", 0) == 0) {
                const int idx = std::atoi(l.c_str() + 6);
                cur.output = by_index.count(idx) ? by_index[idx] : -1;
            } else if (l.rfind("application.name = \"", 0) == 0) {
                std::string a = l.substr(20);
                if (!a.empty() && a.back() == '"') a.pop_back();
                if (a == "optic-cardputerzero") a = "Optic Suite";
                else if (a.find("APPLaunch") != std::string::npos) a = "Launcher";
                else if (a == "Lavf" || a.rfind("ffmpeg", 0) == 0 || a.rfind("ffplay", 0) == 0) a = "Music (stream)";
                cur.app = a;
            }
        }
        close();
        std::lock_guard<std::mutex> l(m_);
        s.note = st_.note;
        st_ = s;
    }

    bool have_{false};
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool busy_{false}, quit_{false};
    std::atomic<bool> scanning_{false};
    BtAudioState st_;
    std::thread worker_, scanner_;
};

// Made-up devices for screenshots and for trying the page on a PC (OPTIC_BT_DEMO=1).
class DemoBtAudio final : public BtAudio {
public:
    DemoBtAudio() {
        st_.available = true;
        st_.devices = {{"C8:22:26:00:00:01", "Redmi Buds 6 Active", true, true}, {"11:22:33:00:00:02", "Desk speaker", true, false},
                       {"11:22:33:00:00:03", "Someone's headphones", false, false}};
        st_.outputs = {{"builtin", "Speaker", false}, {"bluez_output.demo", "Redmi Buds 6 Active", true}};
        st_.default_output = 1;
        st_.streams = {{1, "Optic Suite", 1}, {2, "Launcher", 0}};
    }
    BtAudioState state() override { return st_; }
    void refresh() override {}
    void scan() override { st_.note = "Looking..."; }
    void act(const std::string& mac) override {
        for (auto& d : st_.devices) if (d.mac == mac) { d.connected = !d.connected; d.paired = true; }
    }
    void forget(const std::string& mac) override {
        for (auto& d : st_.devices) if (d.mac == mac) d.paired = d.connected = false;
    }
    void set_default(int o) override { st_.default_output = o; for (auto& s : st_.streams) s.output = o; }
    void move_stream(int id, int o) override { for (auto& s : st_.streams) if (s.id == id) s.output = o; }
    void test() override { st_.note = "Left, right, both"; }

private:
    BtAudioState st_;
};

}  // namespace

std::unique_ptr<BtAudio> make_btaudio() {
    if (const char* d = std::getenv("OPTIC_BT_DEMO"); d && *d == '1') return std::make_unique<DemoBtAudio>();
    return std::make_unique<LinuxBtAudio>();
}

}  // namespace velvet
#endif
