#include "nettools.h"
#include "json_lite.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <functional>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#elif defined(__linux__)
#define VELVET_NET_REAL 1
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace velvet {
namespace {

using Clock = std::chrono::steady_clock;
double secs_since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// ------------------------------------------------------------------ the speed test
// Cloudflare's speed endpoints (the ones speed.cloudflare.com itself uses; CORS open, so the
// web build can use them too). Each direction runs ~8 s; the needle shows a 1 s average.
constexpr const char* kDownUrl = "https://speed.cloudflare.com/__down?bytes=25000000";   // (it refuses ~100 MB+: repeat it)
constexpr const char* kUpUrl = "https://speed.cloudflare.com/__up";
constexpr const char* kPingUrl = "https://speed.cloudflare.com/__down?bytes=0";
constexpr double kPhaseSecs = 8.0;

#if defined(__EMSCRIPTEN__)
// The browser does it: fetch() streams the download, XHR's upload progress times the upload.
EM_JS(void, js_speed_start, (), {
    const s = window.__vspeed = {phase: 1, ping: 0, down: 0, up: 0, live: 0, progress: 0, err: "", stop: false};
    const PHASE = 8000, mbps = (b, ms) => b * 8 / 1000 / Math.max(ms, 1);
    (async () => {
        try {
            let best = 1e9;
            for (let i = 0; i < 4 && !s.stop; i++) {
                const t = performance.now();
                await fetch("https://speed.cloudflare.com/__down?bytes=0&r=" + Math.random(), {cache: "no-store"});
                best = Math.min(best, performance.now() - t);
                s.ping = best; s.progress = (i + 1) / 4;
            }
            if (s.stop) return;
            s.phase = 2; s.progress = 0;
            const t0 = performance.now();
            let n = 0, win = [], over = false;
            while (!over) {   // 25 MB at a time (bigger ones are refused) until the time is up
                const ctl = new AbortController();
                const r = await fetch("https://speed.cloudflare.com/__down?bytes=25000000&r=" + Math.random(), {cache: "no-store", signal: ctl.signal});
                if (!r.ok) throw new Error("http " + r.status);
                const rd = r.body.getReader();
                for (;;) {
                    const {done, value} = await rd.read().catch(() => ({done: true}));
                    if (done) break;
                    const now = performance.now();
                    n += value.length; win.push([now, n]);
                    while (win.length > 1 && now - win[0][0] > 1000) win.shift();
                    s.live = mbps(n - win[0][1], now - win[0][0]);
                    s.progress = Math.min(1, (now - t0) / PHASE);
                    if (now - t0 > PHASE || s.stop) { ctl.abort(); over = true; break; }
                }
            }
            s.down = mbps(n, performance.now() - t0);
            if (s.stop) return;
            s.phase = 3; s.progress = 0; s.live = 0;
            const size = 25e6, t1 = performance.now();
            await new Promise((done) => {
                const x = new XMLHttpRequest(); let w2 = [], last = 0;
                x.open("POST", "https://speed.cloudflare.com/__up");
                x.upload.onprogress = (e) => {
                    const now = performance.now(); last = e.loaded; w2.push([now, e.loaded]);
                    while (w2.length > 1 && now - w2[0][0] > 1000) w2.shift();
                    s.live = mbps(e.loaded - w2[0][1], now - w2[0][0]);
                    s.progress = Math.min(1, (now - t1) / PHASE);
                    if (now - t1 > PHASE || s.stop) { s.up = mbps(e.loaded, now - t1); x.abort(); }
                };
                x.onloadend = () => { if (!s.up) s.up = mbps(last || size, performance.now() - t1); done(); };
                x.send(new Uint8Array(size));
            });
            s.phase = 4; s.live = s.down; s.progress = 1;
        } catch (e) { s.phase = -1; s.err = "No connection"; }
    })();
});
EM_JS(void, js_speed_cancel, (), { if (window.__vspeed) { window.__vspeed.stop = true; window.__vspeed.phase = 0; } });
EM_JS(double, js_speed_get, (int k), {
    const s = window.__vspeed; if (!s) return 0;
    return [s.phase, s.ping, s.down, s.up, s.live, s.progress][k] || 0;
});
#endif

// ------------------------------------------------------------------ processes (Linux)
#if VELVET_NET_REAL
struct Proc {
    pid_t pid{-1};
    int out{-1};   // its stdout+stderr (or -1)
    int in{-1};    // its stdin when asked for (or -1)
};

Proc spawn(const std::vector<std::string>& argv, bool want_in, bool want_out = true) {
    Proc p;
    int o[2] = {-1, -1}, i[2] = {-1, -1};
    if (want_out && pipe(o)) return p;
    if (want_in && pipe(i)) { if (want_out) { close(o[0]); close(o[1]); } return p; }
    p.pid = fork();
    if (p.pid == 0) {
        setpgid(0, 0);
        const int nul = open("/dev/null", O_RDWR);
        dup2(want_in ? i[0] : nul, 0);
        dup2(want_out ? o[1] : nul, 1);
        dup2(want_out ? o[1] : nul, 2);
        for (int fd = 3; fd < 256; ++fd) close(fd);
        setenv("LC_ALL", "C", 1);
        std::vector<char*> a;
        for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        execvp(a[0], a.data());
        _exit(127);
    }
    if (want_out) { close(o[1]); p.out = o[0]; }
    if (want_in) { close(i[0]); p.in = i[1]; }
    return p;
}

void reap(Proc& p, bool kill_it) {
    if (p.pid > 0) {
        if (kill_it) kill(-p.pid, SIGKILL);
        int st;
        waitpid(p.pid, &st, 0);
    }
    if (p.out >= 0) close(p.out);
    if (p.in >= 0) close(p.in);
    p = Proc{};
}

// run to the end (or [timeout] s), returning the output; [code] gets the exit status
std::string run(const std::vector<std::string>& argv, int* code = nullptr, double timeout = 15) {
    Proc p = spawn(argv, false);
    std::string out;
    if (p.pid < 0) { if (code) *code = -1; return out; }
    const auto t0 = Clock::now();
    char buf[4096];
    for (;;) {
        pollfd f{p.out, POLLIN, 0};
        if (poll(&f, 1, 200) > 0) {
            const ssize_t n = read(p.out, buf, sizeof buf);
            if (n <= 0) break;
            out.append(buf, n);
        }
        if (secs_since(t0) > timeout) { reap(p, true); if (code) *code = -1; return out; }
    }
    int st = 0;
    waitpid(p.pid, &st, 0);
    close(p.out);
    if (code) *code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return out;
}

bool have(const char* prog) {
    int c;
    run({"sh", "-c", std::string("command -v ") + prog}, &c, 3);   // prog is a constant here
    return c == 0;
}

std::string read_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return {};
    char buf[256];
    std::string s;
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}
#endif

// ------------------------------------------------------------------ the backend
class Tools final : public NetTools {
public:
    explicit Tools(bool demo) : demo_(demo) {}
    ~Tools() override {
        st_->cancel_speed = true;   // workers hold the shared state, so they can finish after us
        st_->tool_stop = true;
    }
    bool demo() const override { return demo_; }

    // ---- Ethernet
    void eth_refresh() override {
        if (demo_) {
            std::lock_guard<std::mutex> l(st_->mu);
            st_->eth = {true, true, "eth0", "192.168.1.57", "02:5e:a1:c0:0e:42", "192.168.1.1", 100};
            return;
        }
#if VELVET_NET_REAL
        auto st = st_;
        std::thread([st] {
            EthInfo e;
            if (DIR* d = opendir("/sys/class/net")) {
                while (auto* ent = readdir(d)) {
                    const std::string n = ent->d_name;
                    if (n.rfind("eth", 0) && n.rfind("en", 0)) continue;   // eth0, enx…, end0
                    e.present = true;
                    e.iface = n;
                    break;
                }
                closedir(d);
            }
            if (e.present) {
                const std::string base = "/sys/class/net/" + e.iface + "/";
                e.link = read_file(base + "carrier") == "1";
                e.mac = read_file(base + "address");
                e.speed = std::max(0, std::atoi(read_file(base + "speed").c_str()));
                const std::string a = run({"ip", "-4", "-o", "addr", "show", "dev", e.iface});
                if (auto p = a.find("inet "); p != std::string::npos) e.ip = a.substr(p + 5, a.find('/', p) - p - 5);
                const std::string r = run({"ip", "route", "show", "default", "dev", e.iface});
                if (auto p = r.find("via "); p != std::string::npos) e.gateway = r.substr(p + 4, r.find(' ', p + 4) - p - 4);
            }
            std::lock_guard<std::mutex> l(st->mu);
            st->eth = e;
        }).detach();
#endif
    }
    EthInfo eth() override { std::lock_guard<std::mutex> l(st_->mu); return st_->eth; }

    // ---- Tailscale
    void ts_refresh() override {
        if (demo_) {
            std::lock_guard<std::mutex> l(st_->mu);
            if (st_->ts.state == TsInfo::Missing) {
                st_->ts.state = TsInfo::On;
                st_->ts.name = "cardputer-zero";
                st_->ts.ip = "100.101.22.7";
                st_->ts.peers = {{"alienware", "100.84.10.2", true}, {"poco-f1", "100.92.4.18", true}, {"kalicyberdeck", "100.70.33.9", false}};
            }
            return;
        }
#if VELVET_NET_REAL
        auto st = st_;
        std::thread([st] { ts_read(st); }).detach();
#endif
    }
    void ts_set(bool on) override {
        if (demo_) {
            std::lock_guard<std::mutex> l(st_->mu);
            st_->ts.state = on ? TsInfo::On : TsInfo::Off;
            return;
        }
#if VELVET_NET_REAL
        auto st = st_;
        {
            std::lock_guard<std::mutex> l(st->mu);
            st->ts.state = TsInfo::Busy;
            st->ts.error.clear();
        }
        std::thread([st, on] {
            // the daemon needs root: try passwordless sudo first, then directly (an operator)
            std::vector<std::string> cmd = {"tailscale", on ? "up" : "down"};
            if (on) cmd.push_back("--timeout=6s");   // when it needs a login it prints the link and waits
            std::vector<std::string> s = {"sudo", "-n"};
            s.insert(s.end(), cmd.begin(), cmd.end());
            int code;
            std::string out = run(s, &code, 12);
            if (code != 0 && out.find("https://") == std::string::npos) out = run(cmd, &code, 12);
            std::string url;
            if (auto p = out.find("https://login.tailscale.com"); p != std::string::npos)
                url = out.substr(p, out.find_first_of(" \n\r\t", p) - p);
            ts_read(st);
            std::lock_guard<std::mutex> l(st->mu);
            if (!url.empty()) { st->ts.login_url = url; st->ts.state = TsInfo::NeedsLogin; }
            else if (code != 0 && st->ts.state != (on ? TsInfo::On : TsInfo::Off)) {
                std::string e = out.substr(0, out.find('\n'));
                st->ts.error = e.empty() ? "Didn't work (needs sudo)" : e.substr(0, 60);
            }
        }).detach();
#endif
    }
    TsInfo ts() override { std::lock_guard<std::mutex> l(st_->mu); return st_->ts; }

    // ---- speed test
    void speed_start() override {
#if defined(__EMSCRIPTEN__)
        js_speed_start();
#else
        auto st = st_;
        st->cancel_speed = false;
        {
            std::lock_guard<std::mutex> l(st->mu);
            st->speed = SpeedInfo{};
            st->speed.phase = SpeedInfo::Ping;
        }
        const int gen = ++st->speed_gen;   // an older run still going stops itself
        std::thread([st, gen] { speed_run(st, [st, gen] { return st->cancel_speed.load() || st->speed_gen.load() != gen; }); }).detach();
#endif
    }
    void speed_cancel() override {
#if defined(__EMSCRIPTEN__)
        js_speed_cancel();
#else
        st_->cancel_speed = true;
        std::lock_guard<std::mutex> l(st_->mu);
        st_->speed.phase = SpeedInfo::Idle;
#endif
    }
    SpeedInfo speed() override {
#if defined(__EMSCRIPTEN__)
        SpeedInfo s;
        s.phase = static_cast<int>(js_speed_get(0));
        s.ping_ms = js_speed_get(1);
        s.down = js_speed_get(2);
        s.up = js_speed_get(3);
        s.live = js_speed_get(4);
        s.progress = js_speed_get(5);
        if (s.phase == SpeedInfo::Failed) s.error = "No connection";
        return s;
#else
        std::lock_guard<std::mutex> l(st_->mu);
        return st_->speed;
#endif
    }

    // ---- tools
    void tool_run(NetTool t, const std::string& target) override {
        auto st = st_;
        st->tool_stop = false;
        {
            std::lock_guard<std::mutex> l(st->mu);
            st->lines.clear();
            st->running = true;
        }
        const int gen = ++st->tool_gen;   // an older run still going stops itself
        auto stopped = [st, gen] { return st->tool_stop.load() || st->tool_gen.load() != gen; };
        if (demo_) {
            std::thread([st, t, target, stopped] { tool_demo(st, t, target, stopped); }).detach();
            return;
        }
#if VELVET_NET_REAL
        std::thread([st, t, target, stopped] { tool_real(st, t, target, stopped); }).detach();
#endif
    }
    void tool_cancel() override { st_->tool_stop = true; }
    std::vector<std::string> tool_lines(bool* running) override {
        std::lock_guard<std::mutex> l(st_->mu);
        if (running) *running = st_->running;
        return st_->lines;
    }
    std::string tool_default(NetTool t) override {
        std::string gw = "192.168.1.1", ip = "192.168.1.42";
#if VELVET_NET_REAL
        if (!demo_) {
            const std::string r = run({"ip", "route", "show", "default"}, nullptr, 3);
            if (auto p = r.find("via "); p != std::string::npos) gw = r.substr(p + 4, r.find(' ', p + 4) - p - 4);
            const std::string s = run({"ip", "-4", "route", "get", "1.1.1.1"}, nullptr, 3);
            if (auto p = s.find("src "); p != std::string::npos) ip = s.substr(p + 4, s.find(' ', p + 4) - p - 4);
        }
#endif
        switch (t) {
            case NetTool::Ping: return gw;
            case NetTool::Scan: return ip.substr(0, ip.rfind('.')) + ".0/24";
            case NetTool::Ports: return gw;
            case NetTool::Dns: return "m5stack.com";
            case NetTool::Trace: return "1.1.1.1";
        }
        return gw;
    }

private:
    struct State {
        std::mutex mu;
        EthInfo eth;
        TsInfo ts;
        SpeedInfo speed;
        std::vector<std::string> lines;
        bool running{false};
        std::atomic<bool> cancel_speed{false}, tool_stop{false};
        std::atomic<int> speed_gen{0}, tool_gen{0};
    };
    using Stop = std::function<bool()>;

    static void add(const std::shared_ptr<State>& st, const std::string& s) {
        std::lock_guard<std::mutex> l(st->mu);
        st->lines.push_back(s);
        if (st->lines.size() > 400) st->lines.erase(st->lines.begin());
    }
    static void finish(const std::shared_ptr<State>& st) {
        std::lock_guard<std::mutex> l(st->mu);
        st->running = false;
    }

    // a host / address / net as typed: letters, digits, . - : / only (no options, no spaces)
    static bool safe_target(const std::string& s) {
        if (s.empty() || s.size() > 253 || s[0] == '-') return false;
        for (char c : s) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':' || c == '/')) return false;
        return true;
    }

    static void tool_demo(std::shared_ptr<State> st, NetTool t, std::string target, Stop stopped) {
        auto say = [&](const std::string& s, int ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            if (!stopped()) add(st, s);
        };
        switch (t) {
            case NetTool::Ping:
                say("PING " + target, 100);
                for (int i = 1; i <= 5 && !stopped(); ++i) say("reply from " + target + ": time=" + std::to_string(2 + std::rand() % 9) + " ms", 600);
                say("5 sent, 5 received, 0% lost", 200);
                break;
            case NetTool::Scan:
                say("Scanning " + target + " ...", 200);
                say("192.168.1.1    router.lan", 900);
                say("192.168.1.20   alienware", 500);
                say("192.168.1.42   cardputer-zero (this)", 400);
                say("192.168.1.77   poco-f1", 600);
                say("4 devices", 300);
                break;
            case NetTool::Ports:
                say("Ports on " + target + ":", 200);
                say("  22  ssh      open", 700);
                say("  53  dns      open", 300);
                say("  80  http     open", 300);
                say(" 443  https    open", 300);
                say("4 open of 24 checked", 500);
                break;
            case NetTool::Dns:
                say(target + ":", 200);
                say("  104.21.48.12", 400);
                say("  172.67.161.200", 100);
                say("  2606:4700:3036::6815:300c", 100);
                break;
            case NetTool::Trace:
                say("Route to " + target + ":", 200);
                say(" 1  192.168.1.1     2 ms", 500);
                say(" 2  10.64.0.1       9 ms", 600);
                say(" 3  203.69.112.5   14 ms", 600);
                say(" 4  " + target + "         15 ms", 600);
                break;
        }
        finish(st);
    }

#if VELVET_NET_REAL
    // streams a command's lines into the output
    static void stream(const std::shared_ptr<State>& st, const std::vector<std::string>& argv, const Stop& stopped) {
        Proc p = spawn(argv, false);
        if (p.pid < 0) { add(st, "Couldn't start " + argv[0]); return; }
        std::string part;
        char buf[1024];
        bool killed = false;
        for (;;) {
            if (stopped()) { killed = true; break; }
            pollfd f{p.out, POLLIN, 0};
            if (poll(&f, 1, 150) <= 0) continue;
            const ssize_t n = read(p.out, buf, sizeof buf);
            if (n <= 0) break;
            part.append(buf, n);
            size_t nl;
            while ((nl = part.find('\n')) != std::string::npos) {
                add(st, part.substr(0, nl));
                part.erase(0, nl + 1);
            }
        }
        if (!part.empty()) add(st, part);
        reap(p, killed);
    }

    static void tool_real(std::shared_ptr<State> st, NetTool t, std::string target, Stop stopped) {
        if (!safe_target(target)) {
            add(st, "Type a host name or an address");
            finish(st);
            return;
        }
        switch (t) {
            case NetTool::Ping:
                stream(st, {"ping", "-c", "5", "-W", "2", target}, stopped);
                break;
            case NetTool::Trace:
                if (have("tracepath")) stream(st, {"tracepath", "-n", "-m", "20", target}, stopped);
                else if (have("traceroute")) stream(st, {"traceroute", "-n", "-w", "2", "-m", "20", target}, stopped);
                else add(st, "Needs tracepath (sudo apt install iputils-tracepath)");
                break;
            case NetTool::Dns: {
                addrinfo hints{}, *res = nullptr;
                hints.ai_socktype = SOCK_STREAM;
                add(st, target + ":");
                const auto t0 = Clock::now();
                if (getaddrinfo(target.c_str(), nullptr, &hints, &res) != 0 || !res) { add(st, "  not found"); break; }
                std::vector<std::string> seen;
                for (addrinfo* a = res; a; a = a->ai_next) {
                    char h[INET6_ADDRSTRLEN] = "";
                    void* src = a->ai_family == AF_INET ? (void*)&((sockaddr_in*)a->ai_addr)->sin_addr : (void*)&((sockaddr_in6*)a->ai_addr)->sin6_addr;
                    inet_ntop(a->ai_family, src, h, sizeof h);
                    if (std::find(seen.begin(), seen.end(), h) == seen.end()) { seen.push_back(h); add(st, std::string("  ") + h); }
                }
                freeaddrinfo(res);
                char b[48];
                std::snprintf(b, sizeof b, "answered in %.0f ms", secs_since(t0) * 1000);
                add(st, b);
                break;
            }
            case NetTool::Ports: {
                static const std::pair<int, const char*> kPorts[] = {
                    {21, "ftp"}, {22, "ssh"}, {23, "telnet"}, {25, "smtp"}, {53, "dns"}, {80, "http"}, {110, "pop3"},
                    {139, "netbios"}, {143, "imap"}, {443, "https"}, {445, "smb"}, {554, "rtsp"}, {631, "ipp"},
                    {1883, "mqtt"}, {3306, "mysql"}, {3389, "rdp"}, {5000, "upnp"}, {5555, "adb"}, {5900, "vnc"},
                    {8000, "http-alt"}, {8080, "http-alt"}, {8443, "https-alt"}, {8883, "mqtts"}, {9100, "printer"}};
                addrinfo hints{}, *res = nullptr;
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                if (getaddrinfo(target.c_str(), nullptr, &hints, &res) != 0 || !res) { add(st, "Host not found"); break; }
                sockaddr_in addr = *(sockaddr_in*)res->ai_addr;
                freeaddrinfo(res);
                add(st, "Ports on " + target + ":");
                int open_n = 0;
                for (auto& pp : kPorts) {
                    if (stopped()) break;
                    const int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
                    if (s < 0) continue;
                    addr.sin_port = htons(pp.first);
                    connect(s, (sockaddr*)&addr, sizeof addr);
                    pollfd f{s, POLLOUT, 0};
                    int err = 1;
                    socklen_t len = sizeof err;
                    if (poll(&f, 1, 600) > 0) getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
                    close(s);
                    if (err == 0) {
                        char b[48];
                        std::snprintf(b, sizeof b, "%5d  %-9s open", pp.first, pp.second);
                        add(st, b);
                        ++open_n;
                    }
                }
                add(st, std::to_string(open_n) + " open of " + std::to_string(sizeof kPorts / sizeof kPorts[0]) + " checked");
                break;
            }
            case NetTool::Scan: {
                // a /24 around the typed address: ping them all (32 at a time), then the
                // neighbour table has who answered, with their hardware addresses
                std::string net = target.substr(0, target.find('/'));
                int a, b, c, d;
                if (std::sscanf(net.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) != 4) { add(st, "Type an address like 192.168.1.0/24"); break; }
                const std::string pre = std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + ".";
                add(st, "Scanning " + pre + "1-254 ...");
                for (int base = 1; base < 255 && !stopped(); base += 32) {
                    std::vector<Proc> ps;
                    for (int i = base; i < base + 32 && i < 255; ++i) ps.push_back(spawn({"ping", "-c", "1", "-W", "1", pre + std::to_string(i)}, false, false));
                    for (auto& p : ps) reap(p, false);
                }
                if (stopped()) break;
                int found = 0;
                const std::string arp = read_file("/proc/net/arp");
                size_t pos = arp.find('\n');
                while (pos != std::string::npos && pos + 1 < arp.size()) {
                    const size_t e = arp.find('\n', pos + 1);
                    const std::string ln = arp.substr(pos + 1, e == std::string::npos ? std::string::npos : e - pos - 1);
                    pos = e;
                    char ip[64], hw[64], flags[16];
                    if (std::sscanf(ln.c_str(), "%63s %*s %15s %63s", ip, flags, hw) != 3) continue;
                    if (std::strncmp(ip, pre.c_str(), pre.size()) || !std::strcmp(flags, "0x0")) continue;
                    sockaddr_in sa{};
                    sa.sin_family = AF_INET;
                    inet_pton(AF_INET, ip, &sa.sin_addr);
                    char host[NI_MAXHOST] = "";
                    getnameinfo((sockaddr*)&sa, sizeof sa, host, sizeof host, nullptr, 0, NI_NAMEREQD);
                    char b[160];
                    std::snprintf(b, sizeof b, "%-15s %s %s", ip, hw, host);
                    add(st, b);
                    ++found;
                }
                add(st, std::to_string(found) + " devices answered");
                break;
            }
        }
        finish(st);
    }

    static void ts_read(const std::shared_ptr<State>& st) {
        TsInfo t;
        int code;
        const std::string out = run({"tailscale", "status", "--json"}, &code, 8);
        if (code == 127 || (code != 0 && out.find("BackendState") == std::string::npos && !have("tailscale"))) {
            t.state = TsInfo::Missing;
        } else {
            JParse p{out.c_str()};
            const J j = p.val();
            const std::string bs = j["BackendState"].s;
            t.state = bs == "Running" ? TsInfo::On : bs == "NeedsLogin" ? TsInfo::NeedsLogin : TsInfo::Off;
            t.login_url = j["AuthURL"].s;
            t.name = j["Self"]["HostName"].s;
            if (!j["Self"]["TailscaleIPs"].a.empty()) t.ip = j["Self"]["TailscaleIPs"].a[0].s;
            for (auto& kv : j["Peer"].o) {
                TsPeer pe;
                pe.name = kv.second["HostName"].s;
                if (!kv.second["TailscaleIPs"].a.empty()) pe.ip = kv.second["TailscaleIPs"].a[0].s;
                pe.online = kv.second["Online"].b;
                t.peers.push_back(pe);
            }
            std::sort(t.peers.begin(), t.peers.end(), [](const TsPeer& a, const TsPeer& b) {
                return a.online != b.online ? a.online : a.name < b.name;
            });
        }
        std::lock_guard<std::mutex> l(st->mu);
        const std::string url = st->ts.login_url;
        st->ts = t;
        if (t.state == TsInfo::NeedsLogin && t.login_url.empty()) st->ts.login_url = url;
    }
#endif

#if !defined(__EMSCRIPTEN__)
    static void speed_run(std::shared_ptr<State> st, Stop stopped) {
        auto set = [&](auto f) { std::lock_guard<std::mutex> l(st->mu); f(st->speed); };
        auto fail = [&](const char* why) { set([&](SpeedInfo& s) { s.phase = SpeedInfo::Failed; s.error = why; }); };
#if VELVET_NET_REAL
        // latency: the quickest of a few tiny requests (curl's own connect time)
        float best = 1e9f;
        for (int i = 0; i < 4 && !stopped(); ++i) {
            int code;
            const std::string o = run({"curl", "-s", "-o", "/dev/null", "-w", "%{time_connect}", "--max-time", "5", kPingUrl}, &code, 6);
            if (code == 0) best = std::min(best, static_cast<float>(std::atof(o.c_str()) * 1000));
            set([&](SpeedInfo& s) { s.ping_ms = best < 1e8f ? best : 0; s.progress = (i + 1) / 4.f; });
        }
        if (stopped()) return;
        if (best > 1e8f) return fail("No connection");

        // download: count what curl hands over
        set([](SpeedInfo& s) { s.phase = SpeedInfo::Down; s.progress = 0; });
        {
            std::vector<std::string> cmd = {"curl", "-s", "--max-time", "12"};
            for (int i = 0; i < 16; ++i) cmd.push_back(kDownUrl);   // one after another into the same pipe
            Proc p = spawn(cmd, false);
            if (p.pid < 0) return fail("Needs curl");
            const auto t0 = Clock::now();
            double n = 0;
            std::vector<std::pair<double, double>> win;
            static char buf[65536];
            while (!stopped() && secs_since(t0) < kPhaseSecs) {
                pollfd f{p.out, POLLIN, 0};
                if (poll(&f, 1, 100) <= 0) continue;
                const ssize_t r = read(p.out, buf, sizeof buf);
                if (r <= 0) break;
                n += r;
                const double now = secs_since(t0);
                win.emplace_back(now, n);
                while (win.size() > 1 && now - win.front().first > 1.0) win.erase(win.begin());
                const double dt = std::max(0.05, now - win.front().first);
                set([&](SpeedInfo& s) { s.live = static_cast<float>((n - win.front().second) * 8 / 1e6 / dt); s.progress = static_cast<float>(now / kPhaseSecs); });
            }
            const double secs = std::max(0.1, secs_since(t0));
            reap(p, true);
            if (stopped()) return;
            if (n < 1) return fail("Download failed");
            set([&](SpeedInfo& s) { s.down = static_cast<float>(n * 8 / 1e6 / secs); });
        }

        // upload: feed curl zeros as fast as it takes them
        set([](SpeedInfo& s) { s.phase = SpeedInfo::Up; s.progress = 0; s.live = 0; });
        {
            // -T - streams stdin as it comes (--data-binary @- would read it all first)
            Proc p = spawn({"curl", "-s", "-o", "/dev/null", "--max-time", "12", "-X", "POST", "-H",
                            "Content-Type: application/octet-stream", "-T", "-", kUpUrl}, true, false);
            if (p.pid < 0) return fail("Needs curl");
            signal(SIGPIPE, SIG_IGN);
            const auto t0 = Clock::now();
            double n = 0;
            std::vector<std::pair<double, double>> win;
            static char zeros[65536];
            while (!stopped() && secs_since(t0) < kPhaseSecs) {
                pollfd f{p.in, POLLOUT, 0};
                if (poll(&f, 1, 100) <= 0) continue;
                const ssize_t w = write(p.in, zeros, sizeof zeros);
                if (w <= 0) break;
                n += w;
                const double now = secs_since(t0);
                win.emplace_back(now, n);
                while (win.size() > 1 && now - win.front().first > 1.0) win.erase(win.begin());
                const double dt = std::max(0.05, now - win.front().first);
                set([&](SpeedInfo& s) { s.live = static_cast<float>((n - win.front().second) * 8 / 1e6 / dt); s.progress = static_cast<float>(now / kPhaseSecs); });
            }
            const double secs = std::max(0.1, secs_since(t0));
            reap(p, true);
            if (stopped()) return;
            set([&](SpeedInfo& s) { s.up = static_cast<float>(n * 8 / 1e6 / secs); });
        }
        set([](SpeedInfo& s) { s.phase = SpeedInfo::Done; s.live = s.down; s.progress = 1; });
#else
        (void)stopped;
        fail("Not available here");
#endif
    }
#endif

    bool demo_;
    std::shared_ptr<State> st_ = std::make_shared<State>();
};

#if defined(__EMSCRIPTEN__)
// ------------------------------------------------------------------ Optic Suite (the phone)
// VelvetNet.kt does the work on the phone's real network; its answers are JSON.
EM_JS(int, nt_has, (const char* m), { return (typeof VelvetAndroid !== 'undefined' && VelvetAndroid[UTF8ToString(m)]) ? 1 : 0; });
EM_JS(char*, nt_call, (const char* m), {
    try { return stringToNewUTF8(String(VelvetAndroid[UTF8ToString(m)]())); } catch (e) { return stringToNewUTF8(""); }
});
EM_JS(char*, nt_call_i, (const char* m, int a), {
    try { return stringToNewUTF8(String(VelvetAndroid[UTF8ToString(m)](a))); } catch (e) { return stringToNewUTF8(""); }
});
EM_JS(void, nt_run, (int tool, const char* target), { try { VelvetAndroid.netToolRun(tool, UTF8ToString(target)); } catch (e) {} });
EM_JS_DEPS(velvet_nettools, "$stringToNewUTF8,$UTF8ToString");

std::string nt_str(char* p) { std::string r = p ? p : ""; free(p); return r; }

class PhoneTools final : public NetTools {
public:
    bool demo() const override { return false; }
    void eth_refresh() override {}
    EthInfo eth() override {
        const J j = json_parse(nt_str(nt_call("netEth")));
        EthInfo e;
        e.present = j["present"].b; e.link = j["link"].b;
        e.iface = j["iface"].s; e.ip = j["ip"].s; e.gateway = j["gateway"].s; e.mac = j["mac"].s;
        e.speed = static_cast<int>(j["speed"].n);
        return e;
    }
    void ts_refresh() override {}
    void ts_set(bool) override { free(nt_call("netTsOpen")); }   // Android: the Tailscale app does it
    bool ts_external() const override { return true; }
    TsInfo ts() override {
        const J j = json_parse(nt_str(nt_call("netTs")));
        TsInfo t;
        t.state = static_cast<int>(j["state"].n);
        t.name = j["name"].s; t.ip = j["ip"].s; t.error = j["error"].s;
        for (auto& p : j["peers"].a) t.peers.push_back({p["name"].s, p["ip"].s, p["online"].b});
        return t;
    }
    void speed_start() override { free(nt_call("netSpeedStart")); }
    void speed_cancel() override { free(nt_call("netSpeedStop")); }
    SpeedInfo speed() override {
        const J j = json_parse(nt_str(nt_call("netSpeed")));
        SpeedInfo sp;
        sp.phase = static_cast<int>(j["phase"].n);
        sp.ping_ms = static_cast<float>(j["ping"].n); sp.down = static_cast<float>(j["down"].n); sp.up = static_cast<float>(j["up"].n);
        sp.live = static_cast<float>(j["live"].n); sp.progress = static_cast<float>(j["progress"].n); sp.error = j["err"].s;
        return sp;
    }
    void tool_run(NetTool t, const std::string& target) override { nt_run(static_cast<int>(t), target.c_str()); }
    void tool_cancel() override { free(nt_call("netToolStop")); }
    std::vector<std::string> tool_lines(bool* running) override {
        const J j = json_parse(nt_str(nt_call("netToolLines")));
        if (running) *running = j["running"].b;
        std::vector<std::string> out;
        for (auto& l : j["lines"].a) out.push_back(l.s);
        return out;
    }
    std::string tool_default(NetTool t) override { return nt_str(nt_call_i("netToolDefault", static_cast<int>(t))); }
};
#endif

}  // namespace

std::unique_ptr<NetTools> make_net_tools() {
#if defined(__EMSCRIPTEN__)
    if (nt_has("netEth")) return std::make_unique<PhoneTools>();   // in Optic Suite: the phone's network
#endif
#if VELVET_NET_REAL && !USE_DESKTOP
    return std::make_unique<Tools>(false);
#elif VELVET_NET_REAL
    const char* real = std::getenv("VELVET_REAL_DEVICE");
    return std::make_unique<Tools>(!(real && real[0] == '1'));
#else
    return std::make_unique<Tools>(true);
#endif
}

}  // namespace velvet
