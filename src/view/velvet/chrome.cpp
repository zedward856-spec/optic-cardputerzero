#include "chrome.h"
#include "netlink.h"

#include "browser.h"  // decode_image

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#define VELVET_CHROME 1
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <filesystem>
extern char** environ;
#endif

namespace velvet {

// ======================================================================== tiny JSON
namespace {
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t{Null};
    bool b{false};
    double n{0};
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;

    const J& operator[](const char* k) const {
        static const J kNull;
        for (const auto& [key, v] : o) {
            if (key == k) return v;
        }
        return kNull;
    }
    const J& operator[](size_t i) const {
        static const J kNull;
        return i < a.size() ? a[i] : kNull;
    }
};

void put_u8(std::string& s, uint32_t cp) {
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

struct Parser {
    const std::string& s;
    size_t i{0};
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    }
    J value() {
        ws();
        J v;
        if (i >= s.size()) return v;
        const char c = s[i];
        if (c == '{') {
            v.t = J::Obj;
            ++i;
            ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
                return v;
            }
            while (i < s.size()) {
                ws();
                J k = value();
                ws();
                if (i < s.size() && s[i] == ':') ++i;
                v.o.emplace_back(k.s, value());
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == '}') ++i;
                break;
            }
        } else if (c == '[') {
            v.t = J::Arr;
            ++i;
            ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
                return v;
            }
            while (i < s.size()) {
                v.a.push_back(value());
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == ']') ++i;
                break;
            }
        } else if (c == '"') {
            v.t = J::Str;
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    const char e = s[++i];
                    switch (e) {
                        case 'n': v.s += '\n'; break;
                        case 't': v.s += '\t'; break;
                        case 'r': v.s += '\r'; break;
                        case 'b': case 'f': break;
                        case 'u': {
                            uint32_t cp = static_cast<uint32_t>(std::strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
                            i += 4;
                            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
                                const uint32_t lo = static_cast<uint32_t>(std::strtoul(s.substr(i + 3, 4).c_str(), nullptr, 16));
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                i += 6;
                                v.s += '?';  // the pixel fonts have no emoji anyway
                            } else {
                                put_u8(v.s, cp);
                            }
                            break;
                        }
                        default: v.s += e;
                    }
                    ++i;
                } else {
                    v.s += s[i++];
                }
            }
            ++i;
        } else if (c == 't' || c == 'f') {
            v.t = J::Bool;
            v.b = c == 't';
            i += v.b ? 4 : 5;
        } else if (c == 'n') {
            i += 4;
        } else {
            v.t = J::Num;
            const char* start = s.c_str() + i;
            char* end = nullptr;
            v.n = std::strtod(start, &end);
            if (end == start) ++i;  // not a number: skip the character
            else i = static_cast<size_t>(end - s.c_str());
        }
        return v;
    }
};
J parse_json(const std::string& s) {
    Parser p{s};
    return p.value();
}

std::string jstr(const std::string& s) {  // a JSON string literal
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += static_cast<char>(c);
        } else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o += static_cast<char>(c);
        }
    }
    return o + "\"";
}

std::string base64_decode(const std::string& in) {
    static int8_t tbl[256];
    static bool init = false;
    if (!init) {
        std::memset(tbl, -1, sizeof tbl);
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int k = 0; k < 64; ++k) tbl[static_cast<unsigned char>(a[k])] = static_cast<int8_t>(k);
        init = true;
    }
    std::string out;
    out.reserve(in.size() * 3 / 4);
    uint32_t buf = 0;
    int bits = 0;
    for (unsigned char c : in) {
        const int v = tbl[c];
        if (v < 0) continue;
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    return out;
}
}  // namespace

// ======================================================================== engine
#ifdef VELVET_CHROME
namespace {
// Runs in every page before its own scripts: links and window.open stay in this one tab
// (a headless browser has nowhere to show a new tab).
const char* const kSameTabJs = R"JS(
(() => {
  // the real address behind a search engine's click-tracking link (Bing /ck/a?...&u=a1<base64>)
  window.__vh = a => {
    try {
      const h = new URL(a.href);
      if (/(^|\.)bing\.com$/.test(h.hostname) && h.pathname === '/ck/a') {
        const u = h.searchParams.get('u');
        if (u && u.startsWith('a1')) {
          let b = u.slice(2).replace(/-/g, '+').replace(/_/g, '/');
          while (b.length % 4) b += '=';
          const d = atob(b);
          return d.startsWith('/') ? h.origin + d : d;
        }
      }
    } catch (e) {}
    return a.href;
  };
  const fix = e => {
    const a = e.target && e.target.closest && e.target.closest('a[href]');
    if (!a) return;
    if (a.target && a.target !== '_self') a.target = '_self';  // no new tabs: nowhere to show them
    const real = window.__vh(a);
    if (real !== a.href) a.href = real;
  };
  document.addEventListener('click', fix, true);
  document.addEventListener('auxclick', fix, true);
  const open = window.open;
  window.open = function (u) { if (u) { location.href = u; return null; } return open.apply(this, arguments); };
})();
)JS";
}  // namespace
struct ChromeEngine::Impl {
    pid_t pid{-1};
    int to_chrome{-1}, from_chrome{-1};
    std::thread reader;
    std::mutex mu;
    std::vector<std::string> inbox;  // raw messages from Chrome
    bool alive{false};
    int next_id{1};
    std::string session;
    std::string target;  // our page; its id is also its main frame's id
    std::map<int, std::function<void(const J&)>> waiting;
    // screenshot queue: one at a time, the newest request wins
    bool capturing{false};
    int capture_next{-1};
    bool probing{false};
    std::string probe_next;

    void send(const std::string& method, const std::string& params, std::function<void(const J&)> cb, bool in_session) {
        const int id = next_id++;
        std::string msg = "{\"id\":" + std::to_string(id) + ",\"method\":\"" + method + "\"";
        if (!params.empty()) msg += ",\"params\":" + params;
        if (in_session && !session.empty()) msg += ",\"sessionId\":\"" + session + "\"";
        msg += "}";
        msg.push_back('\0');  // messages are NUL-separated on the pipe
        if (cb) waiting[id] = std::move(cb);
        size_t off = 0;
        while (to_chrome >= 0 && off < msg.size()) {
            const ssize_t n = ::write(to_chrome, msg.data() + off, msg.size() - off);
            if (n > 0) off += static_cast<size_t>(n);
            else if (n < 0 && errno == EINTR) continue;
            else break;
        }
    }
    void page(const std::string& method, const std::string& params = "", std::function<void(const J&)> cb = nullptr) {
        send(method, params, std::move(cb), true);
    }
    void eval(const std::string& js, std::function<void(const J&)> cb) {
        page("Runtime.evaluate", "{\"expression\":" + jstr(js) + ",\"returnByValue\":true}",
             [cb](const J& r) { cb(r["result"]["result"]["value"]); });
    }
};

ChromeEngine::ChromeEngine() : impl_(std::make_unique<Impl>()) {}
ChromeEngine::~ChromeEngine() {
    stop();
    if (st_.tile) lv_draw_buf_destroy(st_.tile);
}

std::string ChromeEngine::find_binary() {
    static const char* const kNames[] = {"chromium", "chromium-browser", "chrome-headless-shell", "google-chrome",
                                         "google-chrome-stable"};
    static const char* const kDirs[] = {"/usr/bin/", "/usr/local/bin/", "/bin/", "/snap/bin/"};
    for (const char* n : kNames) {
        for (const char* d : kDirs) {
            const std::string p = std::string(d) + n;
            if (access(p.c_str(), X_OK) == 0) return p;
        }
    }
    return {};
}
bool ChromeEngine::available() { return !find_binary().empty(); }
bool ChromeEngine::running() const { return impl_->alive; }

bool ChromeEngine::start(std::string& error) {
    if (impl_->alive) return true;
    const std::string bin = find_binary();
    if (bin.empty()) {
        error = "No Chromium installed. On the CardputerZero: sudo apt install chromium";
        return false;
    }
    const char* home = std::getenv("HOME");
    const std::string profile = std::string(home ? home : "/tmp") + "/.cache/velvet/chromium";
    std::error_code ec;
    std::filesystem::create_directories(profile, ec);
    int a[2], b[2];
    if (pipe(a) != 0 || pipe(b) != 0) {
        error = "pipe failed";
        return false;
    }
    // everything for the child is prepared before fork
    const std::string w = "--window-size=" + std::to_string(kW) + "," + std::to_string(kH);
    const std::string prof = "--user-data-dir=" + profile;
    std::vector<std::string> args = {bin, "--headless=new", "--remote-debugging-pipe", "--disable-gpu", "--no-first-run",
                                     "--no-default-browser-check", "--disable-extensions", "--mute-audio",
                                     "--hide-scrollbars", "--disable-dev-shm-usage", "--force-device-scale-factor=1",
                                     "--lang=en-US", w, prof};
    // SYSTEM's TOR: every request (names too) through it
    const std::string proxy = tor_proxy();
    if (!proxy.empty()) {
        args.push_back("--proxy-server=socks5://127.0.0.1:9052");
        args.push_back("--host-resolver-rules=MAP * ~NOTFOUND , EXCLUDE 127.0.0.1");
        args.push_back("--force-webrtc-ip-handling-policy=disable_non_proxied_udp");
    }
    proxy_ = proxy;
    args.push_back("about:blank");
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    impl_->pid = fork();
    if (impl_->pid < 0) {
        error = "fork failed";
        return false;
    }
    if (impl_->pid == 0) {
        // Chrome reads commands on fd 3 and writes answers on fd 4
        const int r = dup(a[0]), wfd = dup(b[1]);
        dup2(r, 3);
        dup2(wfd, 4);
        const int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, 0);
        dup2(devnull, 1);
        dup2(devnull, 2);
        for (int fd = 5; fd < 256; ++fd) close(fd);
        setsid();
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(a[0]);
    close(b[1]);
    impl_->to_chrome = a[1];
    impl_->from_chrome = b[0];
    impl_->alive = true;
    Impl* im = impl_.get();
    impl_->reader = std::thread([im] {
        std::string buf;
        char chunk[65536];
        for (;;) {
            const ssize_t n = ::read(im->from_chrome, chunk, sizeof chunk);
            if (n <= 0) break;
            buf.append(chunk, static_cast<size_t>(n));
            size_t z;
            while ((z = buf.find('\0')) != std::string::npos) {
                std::lock_guard<std::mutex> lock(im->mu);
                im->inbox.push_back(buf.substr(0, z));
                buf.erase(0, z + 1);
            }
        }
        std::lock_guard<std::mutex> lock(im->mu);
        im->inbox.push_back("{\"velvet\":\"closed\"}");
    });
    st_ = State{};
    st_.loading = true;
    // find the page, attach to it, set it up
    impl_->send("Target.getTargets", "", [this](const J& r) {
        std::string target;
        for (const J& t : r["result"]["targetInfos"].a) {
            if (t["type"].s == "page") target = t["targetId"].s;
        }
        if (target.empty()) {
            st_.error = "Chromium has no page";
            return;
        }
        impl_->target = target;
        impl_->send("Target.attachToTarget", "{\"targetId\":" + jstr(target) + ",\"flatten\":true}", [this](const J& r2) {
            impl_->session = r2["result"]["sessionId"].s;
            impl_->page("Page.enable");
            impl_->page("Page.addScriptToEvaluateOnNewDocument", "{\"source\":" + jstr(kSameTabJs) + "}");
            impl_->send("Target.setDiscoverTargets", "{\"discover\":true}", nullptr, false);
            impl_->page("Emulation.setDeviceMetricsOverride",
                        "{\"width\":" + std::to_string(kW) + ",\"height\":" + std::to_string(kH) +
                            ",\"deviceScaleFactor\":1,\"mobile\":false}");
            st_.ready = true;
            st_.loading = false;
        }, false);
    }, false);
    return true;
}

void ChromeEngine::stop() {
    Impl& im = *impl_;
    if (!im.alive && im.pid <= 0) return;
    im.alive = false;
    if (im.to_chrome >= 0) close(im.to_chrome);
    im.to_chrome = -1;
    if (im.pid > 0) {
        kill(-im.pid, SIGTERM);
        kill(im.pid, SIGTERM);
        for (int i = 0; i < 40 && waitpid(im.pid, nullptr, WNOHANG) == 0; ++i) usleep(10000);
        kill(-im.pid, SIGKILL);
        kill(im.pid, SIGKILL);
        waitpid(im.pid, nullptr, 0);
        im.pid = -1;
    }
    if (im.from_chrome >= 0) close(im.from_chrome);
    im.from_chrome = -1;
    if (im.reader.joinable()) im.reader.join();
    im.waiting.clear();
    im.session.clear();
    im.capturing = im.probing = false;
    st_.ready = false;
}

void ChromeEngine::navigate(const std::string& url) {
    if (!st_.ready) return;
    st_.loading = true;
    st_.hover.clear();
    impl_->page("Page.navigate", "{\"url\":" + jstr(url) + "}");
}
void ChromeEngine::reload() {
    if (!st_.ready) return;
    st_.loading = true;
    impl_->page("Page.reload");
}
void ChromeEngine::back() {
    if (st_.ready) impl_->eval("history.back()", [](const J&) {});
}

void ChromeEngine::capture(int doc_y) {
    if (!st_.ready) return;
    Impl& im = *impl_;
    if (im.capturing) {  // one at a time; remember only the newest request
        im.capture_next = doc_y;
        return;
    }
    im.capturing = true;
    // scroll there, read back where it really went, then take the picture
    im.eval("window.scrollTo(0," + std::to_string(std::max(0, doc_y)) +
                ");JSON.stringify([window.scrollY,Math.max(document.documentElement.scrollHeight,"
                "document.body?document.body.scrollHeight:0),location.href,document.title,document.readyState])",
            [this](const J& v) {
                const J info = parse_json(v.s);
                if (info[size_t{4}].s == "complete" && st_.loading) {  // in case the load event was missed
                    st_.loading = false;
                    ++st_.loads;
                }
                const int y = static_cast<int>(info[size_t{0}].n);
                if (info[size_t{1}].n > 0) st_.height = std::max<int>(kH, static_cast<int>(info[size_t{1}].n));
                if (!info[size_t{2}].s.empty()) st_.url = info[size_t{2}].s;
                st_.title = info[size_t{3}].s;
                const float sc = scale_;
                std::string params = "{\"format\":\"png\"";
                if (sc < 0.99f) {  // zoomed out: Chrome draws the same page smaller (same layout, sharp text)
                    char clip[160];
                    std::snprintf(clip, sizeof clip, ",\"clip\":{\"x\":0,\"y\":%d,\"width\":%d,\"height\":%d,\"scale\":%.3f}",
                                  y, kW, kH, static_cast<double>(sc));
                    params += clip;
                }
                params += "}";
                impl_->page("Page.captureScreenshot", params, [this, y, sc](const J& r) {
                    const std::string png = base64_decode(r["result"]["data"].s);
                    lv_draw_buf_t* buf = png.empty() ? nullptr : decode_image(png, kW, 4000);
                    if (buf) {
                        if (st_.tile) lv_draw_buf_destroy(st_.tile);
                        st_.tile = buf;
                        st_.tile_y = y;
                        st_.tile_scale = sc;
                        ++st_.tile_serial;
                    }
                    Impl& im2 = *impl_;
                    im2.capturing = false;
                    if (im2.capture_next >= 0) {
                        const int next = im2.capture_next;
                        im2.capture_next = -1;
                        capture(next);
                    }
                });
            });
}

void ChromeEngine::click(int x, int doc_y) {
    if (!st_.ready || !st_.tile) return;
    const int vy = doc_y - st_.tile_y;  // viewport coordinates
    const std::string at = ",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(vy);
    impl_->page("Input.dispatchMouseEvent", "{\"type\":\"mouseMoved\"" + at + "}");
    impl_->page("Input.dispatchMouseEvent", "{\"type\":\"mousePressed\",\"button\":\"left\",\"clickCount\":1" + at + "}");
    impl_->page("Input.dispatchMouseEvent", "{\"type\":\"mouseReleased\",\"button\":\"left\",\"clickCount\":1" + at + "}");
}

void ChromeEngine::type(const std::string& text, bool enter) {
    if (!st_.ready) return;
    if (!text.empty()) impl_->page("Input.insertText", "{\"text\":" + jstr(text) + "}");
    if (enter) {
        impl_->page("Input.dispatchKeyEvent",
                    "{\"type\":\"rawKeyDown\",\"key\":\"Enter\",\"code\":\"Enter\",\"windowsVirtualKeyCode\":13}");
        impl_->page("Input.dispatchKeyEvent", "{\"type\":\"char\",\"text\":\"\\r\"}");
        impl_->page("Input.dispatchKeyEvent",
                    "{\"type\":\"keyUp\",\"key\":\"Enter\",\"code\":\"Enter\",\"windowsVirtualKeyCode\":13}");
    }
}

void ChromeEngine::probe(int x, int doc_y) {
    if (!st_.ready || !st_.tile) return;
    Impl& im = *impl_;
    const std::string js =
        "(()=>{const e=document.elementFromPoint(" + std::to_string(x) + "," + std::to_string(doc_y - st_.tile_y) +
        ");if(!e)return '';const t=e.closest('input,textarea,[contenteditable=true],select');"
        "if(t&&!/^(submit|button|image|checkbox|radio|reset)$/i.test(t.type||''))return 'INPUT';"
        "const a=e.closest('a[href]');if(a)return 'LINK '+(window.__vh?window.__vh(a):a.href);"
        "if(e.tagName==='IFRAME'||e.closest('button,[role=button],input,[onclick],summary,label'))return 'BUTTON';return ''})()";
    if (im.probing) {
        im.probe_next = js;
        return;
    }
    im.probing = true;
    std::function<void(const J&)> done;
    done = [this](const J& v) {
        st_.hover = v.s;
        Impl& im2 = *impl_;
        im2.probing = false;
        if (!im2.probe_next.empty()) {
            const std::string next = im2.probe_next;
            im2.probe_next.clear();
            im2.probing = true;
            im2.eval(next, [this](const J& v2) {
                st_.hover = v2.s;
                impl_->probing = false;
            });
        }
    };
    im.eval(js, done);
}

void ChromeEngine::show_html(const std::string& html) {
    if (!st_.ready) return;
    st_.loading = true;
    st_.hover.clear();
    // a blank page first, then replace its document with ours
    impl_->page("Page.navigate", "{\"url\":\"about:blank\"}", [this, html](const J&) {
        impl_->page("Page.setDocumentContent",
                    "{\"frameId\":" + jstr(impl_->target) + ",\"html\":" + jstr(html) + "}", [this](const J&) {
                        st_.loading = false;
                        ++st_.loads;
                    });
    });
}
void ChromeEngine::search(const std::string&) {}  // on the device Velvet fetches, then show_html()
void ChromeEngine::home() { show_html(full_home_html()); }

void ChromeEngine::poll() {
    Impl& im = *impl_;
    std::vector<std::string> msgs;
    {
        std::lock_guard<std::mutex> lock(im.mu);
        msgs.swap(im.inbox);
    }
    for (const std::string& m : msgs) {
        const J j = parse_json(m);
        if (j["velvet"].s == "closed") {
            if (im.alive) st_.error = "Chromium stopped";
            im.alive = false;
            st_.ready = false;
            continue;
        }
        if (j["id"].t == J::Num) {
            const int id = static_cast<int>(j["id"].n);
            auto it = im.waiting.find(id);
            if (it != im.waiting.end()) {
                auto cb = std::move(it->second);
                im.waiting.erase(it);
                if (j["error"].t == J::Obj) {
                    st_.error = j["error"]["message"].s;
                    if (id && im.capturing) im.capturing = false;
                    if (im.probing) im.probing = false;
                }
                cb(j);
            }
            continue;
        }
        const std::string& method = j["method"].s;
        if (method == "Target.targetCreated" || method == "Target.targetInfoChanged") {
            // a page that opened a new tab anyway: show it here and close the extra tab
            const J& info = j["params"]["targetInfo"];
            const std::string& url = info["url"].s;
            if (info["type"].s == "page" && info["openerId"].t == J::Str && url.rfind("http", 0) == 0 &&
                info["targetId"].s != im.target) {
                im.send("Target.closeTarget", "{\"targetId\":" + jstr(info["targetId"].s) + "}", nullptr, false);
                navigate(url);
            }
            continue;
        }
        if ((method == "Page.frameStartedLoading" || method == "Page.frameRequestedNavigation") &&
            j["params"]["frameId"].s == im.target) {  // the page itself, not one of its frames
            st_.loading = true;
        } else if (method == "Page.loadEventFired") {
            st_.loading = false;
            ++st_.loads;
        } else if (method == "Page.frameNavigated" && j["params"]["frame"]["parentId"].t == J::Null) {
            st_.url = j["params"]["frame"]["url"].s;
        }
    }
}

#else  // the browser demo: the Chromium lives on a PC, reached through tools/velvet-bridge.py
}  // namespace velvet
#include <emscripten.h>
#include <emscripten/fetch.h>
namespace velvet {
namespace {
EM_JS(char*, web_bridge_url, (), {
    let u = window.velvetBridge || '';
    try { u = u || localStorage.getItem('velvet-bridge') || ''; } catch (e) {}
    return stringToNewUTF8(u || 'http://localhost:8766');
});
std::string url_enc(const std::string& t) {
    std::string q;
    for (unsigned char c : t) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') q += static_cast<char>(c);
        else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            q += b;
        }
    }
    return q;
}
std::string url_dec(const std::string& t) {
    std::string o;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '%' && i + 2 < t.size()) {
            o += static_cast<char>(std::strtol(t.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            o += t[i];
        }
    }
    return o;
}
using Got = std::function<void(bool ok, const std::string& body, const std::string& headers)>;
struct Req {
    Got cb;
};
void bridge_get(const std::string& url, Got cb) {
    auto* r = new Req{std::move(cb)};
    emscripten_fetch_attr_t a;
    emscripten_fetch_attr_init(&a);
    std::strcpy(a.requestMethod, "GET");
    a.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    a.userData = r;
    auto done = [](emscripten_fetch_t* f) {
        auto* rq = static_cast<Req*>(f->userData);
        const bool ok = f->status >= 200 && f->status < 300;
        std::string body(f->data ? f->data : "", f->data ? static_cast<size_t>(f->numBytes) : 0);
        std::string hdr;
        const size_t hl = emscripten_fetch_get_response_headers_length(f);
        if (hl) {
            hdr.assign(hl + 1, '\0');
            emscripten_fetch_get_response_headers(f, hdr.data(), hl + 1);
        }
        rq->cb(ok, body, hdr);
        delete rq;
        emscripten_fetch_close(f);
    };
    a.onsuccess = done;
    a.onerror = done;
    emscripten_fetch(&a, url.c_str());
}
std::string header(const std::string& hdr, const char* name) {
    std::string low;
    for (char c : hdr) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string key = std::string(name) + ":";
    const size_t p = low.find(key);
    if (p == std::string::npos) return {};
    size_t v = p + key.size();
    while (v < hdr.size() && hdr[v] == ' ') ++v;
    return url_dec(hdr.substr(v, hdr.find_first_of("\r\n", v) - v));
}
}  // namespace

struct ChromeEngine::Impl {
    std::string base;
    bool alive{false};
    bool capturing{false};
    int capture_next{-1};
    bool probing{false};
    bool state_busy{false};
    uint32_t state_at{0};
    long server_loads{-1};
};
ChromeEngine::ChromeEngine() : impl_(std::make_unique<Impl>()) {}
ChromeEngine::~ChromeEngine() {
    if (st_.tile) lv_draw_buf_destroy(st_.tile);
}
std::string ChromeEngine::find_binary() { return {}; }
bool ChromeEngine::available() { return true; }
bool ChromeEngine::running() const { return impl_->alive; }

bool ChromeEngine::start(std::string&) {
    char* u = web_bridge_url();
    impl_->base = u;
    free(u);
    impl_->alive = true;
    st_ = State{};
    st_.loading = true;
    bridge_get(impl_->base + "/state", [this](bool ok, const std::string& body, const std::string&) {
        const J j = ok ? parse_json(body) : J{};
        if (!ok || !j["ready"].b) {
            impl_->alive = false;
            st_.loading = false;
            st_.error = "No browser bridge found at " + impl_->base +
                        ". On your PC run: python3 tools/velvet-bridge.py, then with the phone on USB: "
                        "adb reverse tcp:8766 tcp:8766. Then press F2.";
            return;
        }
        impl_->server_loads = static_cast<long>(j["loads"].n);
        st_.ready = true;
        st_.loading = false;
    });
    return true;
}
void ChromeEngine::stop() {
    impl_->alive = false;
    st_.ready = false;
}
void ChromeEngine::navigate(const std::string& url) {
    if (!st_.ready) return;
    st_.loading = true;
    st_.hover.clear();
    bridge_get(impl_->base + "/nav?url=" + url_enc(url), [](bool, const std::string&, const std::string&) {});
}
void ChromeEngine::reload() {
    if (!st_.ready) return;
    st_.loading = true;
    bridge_get(impl_->base + "/reload", [](bool, const std::string&, const std::string&) {});
}
void ChromeEngine::back() {
    if (st_.ready) bridge_get(impl_->base + "/back", [](bool, const std::string&, const std::string&) {});
}
void ChromeEngine::capture(int doc_y) {
    if (!st_.ready) return;
    if (impl_->capturing) {
        impl_->capture_next = doc_y;
        return;
    }
    impl_->capturing = true;
    char sc[16];
    std::snprintf(sc, sizeof sc, "%.3f", static_cast<double>(scale_));
    bridge_get(impl_->base + "/shot?y=" + std::to_string(std::max(0, doc_y)) + "&s=" + sc,
               [this](bool ok, const std::string& body, const std::string& hdr) {
                   if (ok && !body.empty()) {
                       lv_draw_buf_t* buf = decode_image(body, kW, 4000);
                       if (buf) {
                           if (st_.tile) lv_draw_buf_destroy(st_.tile);
                           st_.tile = buf;
                           st_.tile_y = std::atoi(header(hdr, "x-tile-y").c_str());
                           const int h = std::atoi(header(hdr, "x-height").c_str());
                           if (h > 0) st_.height = std::max(kH, h);
                           const std::string u = header(hdr, "x-url");
                           if (!u.empty()) st_.url = u;
                           st_.title = header(hdr, "x-title");
                           const std::string scs = header(hdr, "x-scale");
                           st_.tile_scale = scs.empty() ? 1.0f : static_cast<float>(std::atof(scs.c_str()));
                           ++st_.tile_serial;
                       }
                   }
                   impl_->capturing = false;
                   if (impl_->capture_next >= 0) {
                       const int next = impl_->capture_next;
                       impl_->capture_next = -1;
                       capture(next);
                   }
               });
}
void ChromeEngine::click(int x, int doc_y) {
    if (!st_.ready) return;
    bridge_get(impl_->base + "/click?x=" + std::to_string(x) + "&y=" + std::to_string(doc_y),
               [](bool, const std::string&, const std::string&) {});
}
void ChromeEngine::type(const std::string& text, bool enter) {
    if (!st_.ready) return;
    bridge_get(impl_->base + "/type?text=" + url_enc(text) + "&enter=" + (enter ? "1" : "0"),
               [](bool, const std::string&, const std::string&) {});
}
void ChromeEngine::probe(int x, int doc_y) {
    if (!st_.ready || impl_->probing) return;
    impl_->probing = true;
    bridge_get(impl_->base + "/probe?x=" + std::to_string(x) + "&y=" + std::to_string(doc_y),
               [this](bool ok, const std::string& body, const std::string&) {
                   impl_->probing = false;
                   if (ok) st_.hover = parse_json(body)["hover"].s;
               });
}
void ChromeEngine::show_html(const std::string&) {}
void ChromeEngine::search(const std::string& q_enc) {
    if (!st_.ready) return;
    st_.loading = true;
    st_.hover.clear();
    bridge_get(impl_->base + "/search?q=" + q_enc, [](bool, const std::string&, const std::string&) {});
}
void ChromeEngine::home() {
    if (!st_.ready) return;
    st_.loading = true;
    bridge_get(impl_->base + "/home", [](bool, const std::string&, const std::string&) {});
}
void ChromeEngine::poll() {
    // ask the bridge how loading is going, a couple of times a second
    if (!st_.ready || impl_->state_busy || lv_tick_elaps(impl_->state_at) < 500) return;
    impl_->state_busy = true;
    impl_->state_at = lv_tick_get();
    bridge_get(impl_->base + "/state", [this](bool ok, const std::string& body, const std::string&) {
        impl_->state_busy = false;
        if (!ok) return;
        const J j = parse_json(body);
        st_.loading = j["loading"].b;
        if (!j["url"].s.empty()) st_.url = j["url"].s;
        st_.title = j["title"].s;
        if (j["height"].n > 0) st_.height = std::max<int>(kH, static_cast<int>(j["height"].n));
        const long loads = static_cast<long>(j["loads"].n);
        if (loads != impl_->server_loads) {
            impl_->server_loads = loads;
            ++st_.loads;
        }
    });
}
#endif

}  // namespace velvet
