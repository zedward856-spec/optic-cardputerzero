#include "music.h"
#include "sound.h"

#include <algorithm>
#include <cctype>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef __EMSCRIPTEN__
#include <atomic>
#include <condition_variable>
#include <map>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <strings.h>
#include <unistd.h>
#endif

namespace velvet {

std::string fmt_time(double s) {
    if (s < 0) s = 0;
    const long t = static_cast<long>(s);
    char b[24];
    if (t >= 3600) std::snprintf(b, sizeof b, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
    else std::snprintf(b, sizeof b, "%ld:%02ld", t / 60, t % 60);
    return b;
}

#ifdef __EMSCRIPTEN__
// ---- Optic Suite: the phone plays (window.VelvetAndroid.music*, VelvetMusic.kt) -------------------
namespace {
EM_JS_DEPS(velvet_music, "$stringToNewUTF8,$UTF8ToString");
EM_JS(int, vm_has, (), { return (typeof VelvetAndroid !== 'undefined' && VelvetAndroid.musicStatus) ? 1 : 0; });
EM_JS(int, vm_online, (), { try { return VelvetAndroid.musicOnline() ? 1 : 0; } catch (e) { return 0; } });
// string results: a new C string (free it), or 0
EM_JS(char*, vm_call, (const char* m), {
    try { const r = VelvetAndroid[UTF8ToString(m)](); return r == null ? 0 : stringToNewUTF8(String(r)); } catch (e) { return 0; }
});
EM_JS(void, vm_call1, (const char* m, const char* a), { try { VelvetAndroid[UTF8ToString(m)](UTF8ToString(a)); } catch (e) {} });
EM_JS(void, vm_calln, (const char* m, int n), { try { VelvetAndroid[UTF8ToString(m)](n); } catch (e) {} });
EM_JS(void, vm_play, (const char* id, const char* title, const char* uri, int video, double secs), {
    try { VelvetAndroid.musicPlay(UTF8ToString(id), UTF8ToString(title), UTF8ToString(uri), !!video, secs); } catch (e) {}
});
EM_JS(void, vm_save, (const char* id, const char* title), { try { VelvetAndroid.musicSave(UTF8ToString(id), UTF8ToString(title)); } catch (e) {} });
// the video's newest picture: fetched from the app (BGRA) one at a time; 1 = a new one copied to dst
EM_JS(int, vm_frame, (uint8_t* dst, int n), {
    const s = Module.__vmf || (Module.__vmf = {buf: null, fresh: false, busy: false});
    if (!s.busy) {
        s.busy = true;
        fetch('/bridge/music/frame', {cache: 'no-store'})
            .then(r => r.status === 200 ? r.arrayBuffer() : null)
            .then(b => { if (b && b.byteLength === n) { s.buf = new Uint8Array(b); s.fresh = true; } s.busy = false; })
            .catch(() => { s.busy = false; });
    }
    if (!s.fresh) return 0;
    s.fresh = false;
    HEAPU8.set(s.buf, dst);
    return 1;
});

// the cover: "serial<TAB>base64" from the app; a new serial = a new picture (copied to dst)
EM_JS(int, vm_art_serial, (), {
    try { const r = String(VelvetAndroid.musicArt() || ''); const t = r.indexOf('\t');
          Module.__vma = t < 0 ? '' : r.substring(t + 1); return t < 0 ? 0 : (parseInt(r) || 0); } catch (e) { return 0; }
});
EM_JS(int, vm_art_len, (), { try { return Module.__vma ? atob(Module.__vma).length : 0; } catch (e) { return 0; } });
EM_JS(void, vm_art_copy, (uint8_t* dst), {
    try { const b = atob(Module.__vma || ''); for (let i = 0; i < b.length; i++) HEAPU8[dst + i] = b.charCodeAt(i); } catch (e) {}
});

std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> f;
    size_t i = 0;
    for (;;) {
        const size_t j = s.find(d, i);
        f.push_back(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return f;
}

std::string take(char* p) {
    if (!p) return {};
    std::string s = p;
    std::free(p);
    return s;
}

// lines of: id, seconds, channel, title, uri, video
std::vector<Track> tracks(const std::string& s) {
    std::vector<Track> out;
    for (auto& l : split(s, '\n')) {
        const auto f = split(l, '\t');
        if (f.size() < 6 || f[0].empty()) continue;
        Track t;
        t.id = f[0];
        t.duration = std::atoi(f[1].c_str());
        t.channel = f[2];
        t.title = f[3];
        t.path = f[4];
        t.video_file = f[5] == "1";
        out.push_back(t);
    }
    return out;
}

class AndroidMusic : public Music {
public:
    bool available(std::string&) override { return true; }
    bool online() override { return vm_online() != 0; }
    std::vector<Track> library() override { return tracks(take(vm_call("musicLibrary"))); }
    void download(const Track& t) override { if (!t.local()) vm_save(t.id.c_str(), t.title.c_str()); }
    void search(const std::string& q) override { vm_call1("musicSearch", q.c_str()); }
    bool take_results(std::vector<Track>& out) override {
        char* p = vm_call("musicResults");
        if (!p) return false;
        out = tracks(take(p));
        return true;
    }
    void play(const Track& t) override { vm_play(t.id.c_str(), t.title.c_str(), t.path.c_str(), 0, t.duration); }
    void play_video(const Track& t) override { vm_play(t.id.c_str(), t.title.c_str(), t.path.c_str(), 1, t.duration); }
    bool frame(std::vector<uint32_t>& px) override {
        px.resize(static_cast<size_t>(kVW) * kVH);
        return vm_frame(reinterpret_cast<uint8_t*>(px.data()), kVW * kVH * 4) != 0;
    }
    void toggle_pause() override { vm_call("musicPause"); }
    void seek(int s) override { vm_calln("musicSeek", s); }
    void volume(int d) override { vm_calln("musicVolume", d); }
    void stop() override { vm_call("musicStop"); }
    Status status() override {
        Status s;
        const auto f = split(take(vm_call("musicStatus")), '\t');
        if (f.size() < 11) return s;
        s.busy = f[0] == "1";
        s.playing = f[1] == "1";
        s.paused = f[2] == "1";
        s.ended = f[3] == "1";
        s.video = f[4] == "1";
        s.pos = std::atof(f[5].c_str());
        s.dur = std::atof(f[6].c_str());
        s.volume = std::atoi(f[7].c_str());
        s.title = f[8];
        s.error = f[9];
        s.note = f[10];
        return s;
    }
    bool art(std::string& bytes) override {
        const int serial = vm_art_serial();
        if (serial == art_serial_) return false;
        art_serial_ = serial;
        bytes.assign(static_cast<size_t>(vm_art_len()), '\0');
        if (!bytes.empty()) vm_art_copy(reinterpret_cast<uint8_t*>(&bytes[0]));
        return true;
    }

private:
    int art_serial_{0};
};
}  // namespace

// in Optic Suite: the phone's player; in a plain browser: none (MUSIC shows a note)
std::unique_ptr<Music> make_music() {
    if (vm_has()) return std::make_unique<AndroidMusic>();
    return nullptr;
}
#else
namespace {

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool on_path(const char* exe) {
    const char* path = std::getenv("PATH");
    std::string p = path ? path : "/usr/local/bin:/usr/bin:/bin";
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    p += ":" + home + "/.local/bin";
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find(':', i);
        if (j == std::string::npos) j = p.size();
        const std::string f = p.substr(i, j - i) + "/" + exe;
        if (!f.empty() && access(f.c_str(), X_OK) == 0) return true;
        i = j + 1;
    }
    return false;
}

// Runs argv (no shell), returns its standard output; empty on failure.
std::string capture(const std::vector<std::string>& argv) {
    int fd[2];
    if (pipe(fd)) return {};
    std::vector<char*> a;
    for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fd[1], 1);
        const int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
        close(fd[0]);
        close(fd[1]);
        execvp(a[0], a.data());
        _exit(127);
    }
    close(fd[1]);
    std::string out;
    if (pid > 0) {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd[0], buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
        int st = 0;
        waitpid(pid, &st, 0);
    }
    close(fd[0]);
    return out;
}

// ---- yt-dlp: the system's, or this app's own copy, fetched the first time YouTube is used ----
// No root and no package: the official zip (about 3 MB, needs python3) is downloaded, unpacked and
// precompiled into ~/.local/share/optic-cardputerzero/yt-dlp. Unpacked it starts in about 4 s on a
// CardputerZero where the zip itself takes 10 (a zip can't keep compiled files): a search 8 s
// instead of 17 (measured 2026-11-25).
std::string ytdlp_dir() {
    const char* h = std::getenv("HOME");
    return std::string(h ? h : ".") + "/.local/share/optic-cardputerzero/yt-dlp";
}
bool ytdlp_own() { return access((ytdlp_dir() + "/__main__.py").c_str(), R_OK) == 0; }
bool ytdlp_ready() { return on_path("yt-dlp") || ytdlp_own(); }
bool ytdlp_can_install() { return on_path("python3") && on_path("curl"); }
// the command line: yt-dlp, or python3 on this app's own copy
std::vector<std::string> ytdlp(std::initializer_list<std::string> args) {
    std::vector<std::string> v;
    if (!on_path("yt-dlp") && ytdlp_own()) v = {"python3", ytdlp_dir()};
    else v = {"yt-dlp"};
    v.insert(v.end(), args.begin(), args.end());
    return v;
}
// blocks (a download, then about 20 s of unpacking and compiling): call it from a worker thread
bool ytdlp_install() {
    const std::string dir = ytdlp_dir(), zip = dir + ".zip", fresh = dir + ".new";
    capture({"mkdir", "-p", dir.substr(0, dir.rfind('/'))});
    capture({"curl", "-sSL", "--max-time", "180", "-o", zip, "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp"});
    struct stat st{};
    if (stat(zip.c_str(), &st) != 0 || st.st_size < 500000) { unlink(zip.c_str()); return false; }   // offline, or an error page
    capture({"rm", "-rf", fresh});
    capture({"python3", "-m", "zipfile", "-e", zip, fresh});
    capture({"python3", "-m", "compileall", "-q", "-j", "4", fresh});
    unlink(zip.c_str());
    if (access((fresh + "/__main__.py").c_str(), R_OK) != 0) { capture({"rm", "-rf", fresh}); return false; }
    capture({"rm", "-rf", dir});
    return rename(fresh.c_str(), dir.c_str()) == 0 && ytdlp_own();
}

class LinuxMusic : public Music {
public:
    // the first YouTube use on a machine without yt-dlp: get it, saying so (in a worker thread)
    bool need_ytdlp() {
        if (ytdlp_ready()) return true;
        { std::lock_guard<std::mutex> l(m_); note_ = "Getting yt-dlp: first time, ~1 min"; }
        installing_ = true;
        const bool ok = ytdlp_install();
        installing_ = false;
        std::lock_guard<std::mutex> l(m_);
        note_.clear();
        if (!ok) error_ = "CAN'T GET yt-dlp (OFFLINE?)";
        return ok;
    }

    bool art(std::string& bytes) override {
        std::lock_guard<std::mutex> l(art_->m);
        if (!art_->fresh) return false;
        art_->fresh = false;
        bytes = art_->bytes;
        return true;
    }

    // the cover, in the background: a file's own picture (ffmpeg), else YouTube's thumbnail
    // The cover picture stored in an MP3 (its ID3v2 "APIC" frame), read straight from the file:
    // no outside program, so it is there at once. Empty when there is none or it isn't a JPEG.
    static std::string id3_cover(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return {};
        std::string out;
        unsigned char h[10];
        if (std::fread(h, 1, 10, f) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3' && (h[3] == 3 || h[3] == 4)) {
            const size_t total = (static_cast<size_t>(h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) | ((h[8] & 0x7f) << 7) | (h[9] & 0x7f);
            size_t at = 0;
            while (at + 10 <= total && total < (32u << 20)) {
                unsigned char fh[10];
                if (std::fread(fh, 1, 10, f) != 10 || fh[0] == 0) break;
                const size_t n = h[3] == 4
                    ? (static_cast<size_t>(fh[4] & 0x7f) << 21) | ((fh[5] & 0x7f) << 14) | ((fh[6] & 0x7f) << 7) | (fh[7] & 0x7f)
                    : (static_cast<size_t>(fh[4]) << 24) | (fh[5] << 16) | (fh[6] << 8) | fh[7];
                if (n == 0 || at + 10 + n > total) break;
                if (std::memcmp(fh, "APIC", 4) == 0 && n < (8u << 20)) {
                    std::string b(n, '\0');
                    if (std::fread(&b[0], 1, n, f) == n) {
                        const size_t j = b.find("\xFF\xD8\xFF");      // the JPEG itself, after the frame's text fields
                        if (j != std::string::npos && j < 300) out = b.substr(j);
                    }
                    break;
                }
                if (std::fseek(f, static_cast<long>(n), SEEK_CUR) != 0) break;
                at += 10 + n;
            }
        }
        std::fclose(f);
        return out;
    }

    void clear_art() {
        auto st = art_;
        std::lock_guard<std::mutex> l(st->m);
        ++st->gen;
        st->bytes.clear();
        st->fresh = true;
    }

    void fetch_art(const Track& t) {
        auto st = art_;
        int gen;
        { std::lock_guard<std::mutex> l(st->m); gen = ++st->gen; st->bytes.clear(); st->fresh = true; }   // the old one goes at once
        std::thread([st, gen, t] {
            std::string b;
            if (t.local()) {
                b = id3_cover(t.path);
                const bool mp3 = t.path.size() > 4 && strcasecmp(t.path.c_str() + t.path.size() - 4, ".mp3") == 0;
                if (b.empty() && !mp3 && on_path("ffmpeg"))   // an MP3 without a cover has none: don't start ffmpeg to learn that
                    b = capture({"ffmpeg", "-v", "quiet", "-i", t.path, "-an", "-frames:v", "1", "-c:v", "mjpeg", "-f", "image2pipe", "-"});
            } else if (!t.id.empty() && on_path("curl")) {
                b = capture({"curl", "-s", "--max-time", "8", "https://i.ytimg.com/vi/" + t.id + "/mqdefault.jpg"});
            }
            std::lock_guard<std::mutex> l(st->m);
            if (gen != st->gen) return;
            st->bytes = b;
            st->fresh = true;
        }).detach();
    }

    ~LinuxMusic() override {
        stop();
        if (worker_.joinable()) worker_.join();
        if (dl_.joinable()) dl_.join();
    }

    bool available(std::string& why) override {
        if (!on_path("mpv") && !on_path("ffplay") && !on_path("ffmpeg")) { why = "INSTALL mpv  (sudo apt install mpv)"; return false; }
        return true;
    }
    bool online() override { return ytdlp_ready() || ytdlp_can_install(); }

    static std::vector<std::string> music_dirs() {
        std::vector<std::string> d;
        if (const char* e = std::getenv("VELVET_MUSIC_DIR")) d.push_back(e);
        if (const char* h = std::getenv("HOME")) d.push_back(std::string(h) + "/Music");
        d.push_back("/media/SD Card/Music");
        return d;
    }

    // files under dir (2 folders deep), audio or video by extension
    static void scan(const std::string& dir, int depth, std::vector<Track>& out) {
        DIR* dp = opendir(dir.c_str());
        if (!dp) return;
        while (dirent* e = readdir(dp)) {
            const std::string n = e->d_name;
            if (n.empty() || n[0] == '.') continue;
            const std::string p = dir + "/" + n;
            struct stat st{};
            if (stat(p.c_str(), &st)) continue;
            if (S_ISDIR(st.st_mode)) { if (depth < 2) scan(p, depth + 1, out); continue; }
            const size_t dot = n.rfind('.');
            if (dot == std::string::npos) continue;
            std::string ext = n.substr(dot + 1);
            for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            static const char* kAudio[] = {"mp3", "m4a", "aac", "flac", "ogg", "opus", "wav", "wma", "webm"};
            static const char* kVideo[] = {"mp4", "mkv", "mov", "avi"};
            bool audio = false, video = false;
            for (auto* x : kAudio) audio |= ext == x;
            for (auto* x : kVideo) video |= ext == x;
            if (!audio && !video) continue;
            Track t;
            t.path = p;
            t.id = "file:" + p;
            t.title = n.substr(0, dot);
            for (auto& ch : t.title) if (ch == '_') ch = ' ';   // saved names use _ for spaces
            t.channel = ext;
            t.video_file = video;
            out.push_back(t);
        }
        closedir(dp);
    }

    std::vector<Track> library() override {
        std::vector<Track> out;
        for (auto& d : music_dirs()) scan(d, 0, out);
        std::sort(out.begin(), out.end(), [](const Track& a, const Track& b) {
            std::string x = a.title, y = b.title;
            for (auto& ch : x) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            for (auto& ch : y) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return x < y;
        });
        return out;
    }

    void download(const Track& t) override {
        if (t.local() || downloading_) return;
        if (!online()) { std::lock_guard<std::mutex> l(m_); error_ = "INSTALL yt-dlp TO SAVE"; return; }
        downloading_ = true;
        {
            std::lock_guard<std::mutex> l(m_);
            note_ = "SAVING: " + t.title;
        }
        if (dl_.joinable()) dl_.join();
        dl_ = std::thread([this, t] {
            const std::string dir = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/Music";
            mkdir(dir.c_str(), 0755);
            if (!need_ytdlp()) { downloading_ = false; return; }
            { std::lock_guard<std::mutex> l(m_); note_ = "SAVING: " + t.title; }
            // m4a needs no converting (no ffmpeg work on the little CPU); --print after_move = the saved file
            // With ffmpeg: the cover goes into the file too (YouTube's picture, cut to a square, as a
            // JPEG: what players and this app's own Now Playing read), with the title, artist and album.
            std::vector<std::string> a = ytdlp({"-f", "bestaudio[ext=m4a]/bestaudio", "--no-warnings", "--no-playlist",
                                                "--restrict-filenames", "-o", dir + "/%(title).80s.%(ext)s",
                                                "--print", "after_move:filepath"});
            if (on_path("ffmpeg")) {
                for (const char* x : {"--embed-thumbnail", "--embed-metadata", "--convert-thumbnails", "jpg", "--ppa",
                                      "ThumbnailsConvertor+ffmpeg_o:-c:v mjpeg -vf crop=ih:ih"})
                    a.push_back(x);
            }
            a.push_back("https://www.youtube.com/watch?v=" + t.id);
            const std::string out = capture(a);
            std::lock_guard<std::mutex> l(m_);
            note_ = out.find('/') != std::string::npos ? "SAVED TO MY MUSIC: " + t.title : "";
            note_at_ = now_s();
            if (note_.empty()) error_ = "COULDN'T SAVE (OFFLINE?)";
            downloading_ = false;
        });
    }

    void search(const std::string& q) override {
        if (busy_) return;
        if (!online()) { std::lock_guard<std::mutex> l(m_); error_ = "INSTALL yt-dlp FOR YOUTUBE"; return; }
        busy_ = true;
        {
            std::lock_guard<std::mutex> l(m_);
            error_.clear();
        }
        if (worker_.joinable()) worker_.join();
        worker_ = std::thread([this, q] {
            if (!need_ytdlp()) { busy_ = false; return; }
            const std::string out = capture(ytdlp({"--flat-playlist", "--no-warnings", "--print",
                                                   "%(id)s\t%(duration)s\t%(channel)s\t%(title)s", "ytsearch12:" + q}));
            std::vector<Track> r;
            size_t i = 0;
            while (i < out.size()) {
                size_t j = out.find('\n', i);
                if (j == std::string::npos) j = out.size();
                const std::string line = out.substr(i, j - i);
                i = j + 1;
                const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1),
                             c = b == std::string::npos ? b : line.find('\t', b + 1);
                if (c == std::string::npos) continue;
                Track t;
                t.id = line.substr(0, a);
                t.duration = std::atoi(line.substr(a + 1, b - a - 1).c_str());
                t.channel = line.substr(b + 1, c - b - 1);
                t.title = line.substr(c + 1);
                if (t.channel == "NA") t.channel.clear();
                if (!t.id.empty()) r.push_back(t);
            }
            std::lock_guard<std::mutex> l(m_);
            results_ = r;
            have_results_ = true;
            if (r.empty()) error_ = "NO RESULTS (OFFLINE?)";
            busy_ = false;
        });
    }

    bool take_results(std::vector<Track>& out) override {
        std::lock_guard<std::mutex> l(m_);
        if (!have_results_) return false;
        have_results_ = false;
        out = results_;
        return true;
    }

    void play(const Track& t) override { open(t, false); }
    void play_video(const Track& t) override { open(t, true); }

    bool frame(std::vector<uint32_t>& px) override {
        std::lock_guard<std::mutex> l(fm_);
        if (!frame_new_) return false;
        frame_new_ = false;
        px = frame_;
        return true;
    }

    void open(const Track& t, bool video) {
        if (busy_) return;
        stop();
        busy_ = true;
        if (!t.local()) fetch_art(t);   // a file's own picture is read after its sound has started
        else clear_art();
        {
            std::lock_guard<std::mutex> l(m_);
            title_ = t.title;
            dur_ = t.duration;
            error_.clear();
            video_ = video;
        }
        if (worker_.joinable()) worker_.join();
        if (t.local()) {   // a file of your own: its length from ffprobe, then straight to the player
            // The player first and alone: on a CardputerZero each of these programs takes seconds
            // to load, and three at once (player, picture, length) made a song take ~10 s to start.
            worker_ = std::thread([this, t] {
                int gen;
                {
                    std::lock_guard<std::mutex> l(m_);
                    url_ = t.path;
                    aurl_.clear();
                    if (inapp::open(t.path)) {          // MP3 / FLAC / WAV: decoded here, starts at once
                        kill_player();
                        inapp_ = true;
                        paused_ = false;
                        sounding_ = true;
                    } else {
                        start_at(0);
                    }
                    busy_ = false;
                    gen = start_gen_.load();
                }
                for (int i = 0; i < 80 && !sounding_ && gen == start_gen_.load(); ++i) usleep(100000);   // up to 8 s
                if (gen != start_gen_.load()) return;          // stopped or another track meanwhile
                fetch_art(t);
                double len = inapp_ ? inapp::length(t.path) : 0;   // without starting another program
                if (len > 0) { std::lock_guard<std::mutex> l(m_); if (url_ == t.path) dur_ = len; }
                if (len <= 0 && on_path("ffprobe"))
                    len = std::atof(capture({"ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", t.path}).c_str());
                std::lock_guard<std::mutex> l(m_);
                if (url_ == t.path && len > 0) dur_ = len;
            });
            return;
        }
        worker_ = std::thread([this, t, video] {
            // The streams' own addresses: then the player doesn't need yt-dlp again (and seeks are
            // quick). Video: a small H.264 picture (the screen is 320x170) + the sound, or one
            // combined stream when that's all there is.
            const std::string fmt = video ? "bv*[height<=240][vcodec^=avc1]+ba[ext=m4a]/bv*[height<=360]+ba/b[height<=360]/b"
                                          : "bestaudio[ext=m4a]/bestaudio/best";
            if (!need_ytdlp()) { busy_ = false; return; }
            const std::vector<std::string> urls = resolve(t.id, fmt);
            std::lock_guard<std::mutex> l(m_);
            if (urls.empty()) { error_ = "CAN'T OPEN THIS ONE"; busy_ = false; return; }
            url_ = urls[0];
            aurl_ = urls.size() > 1 ? urls[1] : "";
            start_at(0);
            busy_ = false;
        });
    }

    // Finding a stream's address is the slow part of starting a track (yt-dlp: about 12 s on a
    // CardputerZero), so it is done ahead for the highlighted track and kept for 3 hours.
    void prepare(const Track& t) override {
        if (t.local() || t.id.empty() || busy_ || !ytdlp_ready()) return;
        const std::string fmt = "bestaudio[ext=m4a]/bestaudio/best";
        {
            std::lock_guard<std::mutex> l(rm_);
            if (!resolving_.empty() || rcache_.count(t.id + "|" + fmt)) return;   // one at a time
        }
        std::thread([this, id = t.id, fmt] { resolve(id, fmt); }).detach();
    }

    std::vector<std::string> resolve(const std::string& id, const std::string& fmt) {
        const std::string key = id + "|" + fmt;
        std::unique_lock<std::mutex> l(rm_);
        rcv_.wait(l, [&] { return resolving_ != key; });      // being looked up already: wait for that
        auto it = rcache_.find(key);
        if (it != rcache_.end() && now_s() - it->second.second < 3 * 3600) return it->second.first;
        resolving_ = key;
        l.unlock();
        const std::string out = capture(ytdlp({"-f", fmt, "-g", "--no-warnings", "https://www.youtube.com/watch?v=" + id}));
        std::vector<std::string> urls;
        size_t i = 0;
        while (i < out.size()) {
            size_t j = out.find('\n', i);
            if (j == std::string::npos) j = out.size();
            std::string u = out.substr(i, j - i);
            while (!u.empty() && (u.back() == '\r' || u.back() == ' ')) u.pop_back();
            if (!u.empty()) urls.push_back(u);
            i = j + 1;
        }
        l.lock();
        if (resolving_ == key) resolving_.clear();
        if (!urls.empty()) rcache_[key] = {urls, now_s()};
        rcv_.notify_all();
        return urls;
    }

    void toggle_pause() override {
        std::lock_guard<std::mutex> l(m_);
        if (inapp_) { paused_ = !paused_; inapp::pause(paused_); return; }
        if (pid_ <= 0) return;
        if (paused_) {
            kill(pid_, SIGCONT);
            started_ += now_s() - paused_at_;
            paused_ = false;
        } else {
            kill(pid_, SIGSTOP);
            paused_at_ = now_s();
            paused_ = true;
        }
    }

    void seek(int s) override {
        std::lock_guard<std::mutex> l(m_);
        if (inapp_) {
            double p = inapp::pos() + s;
            if (p < 0) p = 0;
            if (dur_ > 0 && p > dur_ - 1) p = dur_ - 1;
            inapp::seek(p);
            return;
        }
        if (pid_ <= 0 || url_.empty()) return;
        double p = pos_locked() + s;
        if (p < 0) p = 0;
        if (dur_ > 0 && p > dur_ - 1) p = dur_ - 1;
        start_at(p);
    }

    void volume(int d) override {
        std::lock_guard<std::mutex> l(m_);
        vol_ = std::max(0, std::min(100, vol_ + d));
        if (pid_ > 0 && !url_.empty()) start_at(pos_locked());   // the players take the volume at start
    }

    void stop() override {
        std::lock_guard<std::mutex> l(m_);
        kill_player();
        url_.clear();
        title_.clear();
    }

    Status status() override {
        std::lock_guard<std::mutex> l(m_);
        Status s;
        s.busy = busy_;
        if (pid_ > 0) {
            int st = 0;
            if (waitpid(pid_, &st, WNOHANG) == pid_) {   // finished by itself
                pid_ = -1;
                ended_ = true;
                url_.clear();
            }
        }
        if (inapp_ && inapp::ended()) {   // the file ran out
            inapp::stop();
            inapp_ = false;
            ended_ = true;
            url_.clear();
        }
        s.playing = pid_ > 0 || inapp_;
        s.video = video_ && pid_ > 0;
        s.paused = paused_;
        s.loading = pid_ > 0 && !sounding_;
        s.pos = !s.playing ? 0 : inapp_ ? inapp::pos() : s.loading ? load_pos_ : pos_locked();
        s.dur = dur_;
        s.volume = vol_;
        s.title = title_;
        s.error = error_;
        if (downloading_ || installing_ || now_s() - note_at_ < 4) s.note = note_;   // "SAVED ..." stays 4 s
        s.ended = ended_;
        ended_ = false;
        return s;
    }

private:
    double pos_locked() const { return (paused_ ? paused_at_ : now_s()) - started_; }

    void kill_player() {
        if (inapp_) { inapp::stop(); inapp_ = false; }
        ++start_gen_;                   // the "has the sound started" watcher of this player gives up
        sounding_ = true;
        if (vfd_ >= 0) { reader_stop_ = true; }
        if (pid_ > 0) {
            if (paused_) kill(pid_, SIGCONT);
            kill(pid_, SIGTERM);
            int st = 0;
            for (int i = 0; i < 20 && waitpid(pid_, &st, WNOHANG) == 0; ++i) usleep(10000);
            if (waitpid(pid_, &st, WNOHANG) == 0) { kill(pid_, SIGKILL); waitpid(pid_, &st, 0); }
        }
        pid_ = -1;
        paused_ = false;
        if (reader_.joinable()) reader_.join();   // the pipe closed with ffmpeg: read() ends
        if (vfd_ >= 0) { close(vfd_); vfd_ = -1; }
        reader_stop_ = false;
    }

    // ffmpeg: picture -> 320x170 BGRA on a pipe, sound -> the speaker; -re = in real time
    std::vector<std::string> video_argv(const char* start) {
        char vol[16];
        std::snprintf(vol, sizeof vol, "volume=%.2f", vol_ / 100.0);
        std::vector<std::string> a = {"ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-re", "-ss", start, "-i", url_};
        if (!aurl_.empty()) { a.insert(a.end(), {"-re", "-ss", start, "-i", aurl_}); }
        a.insert(a.end(), {"-map", "0:v:0", "-vf",
                           "fps=25,scale=320:170:force_original_aspect_ratio=decrease:flags=bilinear,pad=320:170:(ow-iw)/2:(oh-ih)/2",
                           "-pix_fmt", "bgra", "-f", "rawvideo", "pipe:1",
                           "-map", aurl_.empty() ? "0:a:0?" : "1:a:0", "-af", vol});
        // where the sound goes: VELVET_AUDIO_OUT=null for tests; PulseAudio if it runs, else ALSA
        const char* force = std::getenv("VELVET_AUDIO_OUT");
        const char* rt = std::getenv("XDG_RUNTIME_DIR");
        const bool pulse = std::getenv("PULSE_SERVER") || (rt && access((std::string(rt) + "/pulse/native").c_str(), F_OK) == 0);
        if (force && !std::strcmp(force, "null")) a.insert(a.end(), {"-f", "null", "/dev/null"});
        else if (pulse) a.insert(a.end(), {"-f", "pulse", "default"});
        else a.insert(a.end(), {"-f", "alsa", "default"});
        return a;
    }

    // (mutex held) (re)starts the player at p seconds
    void start_at(double p) {
        kill_player();
        char start[32], vol[16];
        std::snprintf(start, sizeof start, "%.1f", p);
        std::snprintf(vol, sizeof vol, "%d", vol_);
        std::vector<std::string> argv;
        if (video_) argv = video_argv(start);
        else if (on_path("mpv"))
            argv = {"mpv", "--no-video", "--no-terminal", "--really-quiet", std::string("--start=") + start,
                    std::string("--volume=") + vol, url_};
        else {
            // ffmpeg straight to the sound server starts sooner than ffplay (no SDL to bring up);
            // ffplay stays as the fallback. A stream isn't read ahead to analyse it.
            const bool http = url_.compare(0, 4, "http") == 0;
            const char* rt = std::getenv("XDG_RUNTIME_DIR");
            const bool pulse = std::getenv("PULSE_SERVER") || (rt && access((std::string(rt) + "/pulse/native").c_str(), F_OK) == 0);
            if (pulse && on_path("ffmpeg")) {
                argv = {"ffmpeg", "-nostdin", "-v", "error"};
                if (http) argv.insert(argv.end(), {"-probesize", "32768", "-analyzeduration", "0"});
                argv.insert(argv.end(), {"-ss", start, "-i", url_, "-vn"});
                if (vol_ != 100) { char af[32]; std::snprintf(af, sizeof af, "volume=%.2f", vol_ / 100.0); argv.insert(argv.end(), {"-af", af}); }
                argv.insert(argv.end(), {"-buffer_duration", "300", "-f", "pulse", "default"});
            } else {
                argv = {"ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet"};
                if (http) argv.insert(argv.end(), {"-probesize", "32768", "-analyzeduration", "0"});
                argv.insert(argv.end(), {"-ss", start, "-volume", vol, url_});
            }
        }
        std::vector<char*> a;
        for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        int fd[2] = {-1, -1};
        if (video_ && pipe(fd)) fd[0] = fd[1] = -1;
        const pid_t pid = fork();
        if (pid == 0) {
            const int nul = ::open("/dev/null", O_RDWR);
            if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
            if (fd[1] >= 0) { dup2(fd[1], 1); close(fd[0]); close(fd[1]); }
            setpgid(0, 0);
            execvp(a[0], a.data());
            _exit(127);
        }
        if (fd[1] >= 0) close(fd[1]);
        vfd_ = fd[0];
        if (vfd_ >= 0) {
            reader_ = std::thread([this, f = vfd_] {
                const size_t n = static_cast<size_t>(kVW) * kVH * 4;
                std::vector<uint32_t> buf(static_cast<size_t>(kVW) * kVH);
                for (;;) {
                    size_t got = 0;
                    auto* b = reinterpret_cast<char*>(buf.data());
                    while (got < n) {
                        const ssize_t r = read(f, b + got, n - got);
                        if (r <= 0) return;
                        got += static_cast<size_t>(r);
                    }
                    if (reader_stop_) return;
                    std::lock_guard<std::mutex> l(fm_);
                    frame_.swap(buf);
                    if (buf.size() != frame_.size()) buf.resize(frame_.size());
                    frame_new_ = true;
                }
            });
        }
        pid_ = pid;
        started_ = now_s() - p;
        load_pos_ = p;
        // The player takes a few seconds to open a stream and make its first sound (about 5 on a
        // CardputerZero). Until its stream shows up at the sound server the track is "loading" and
        // the clock waits, instead of counting through silence as if it played.
        sounding_ = false;
        const int gen = ++start_gen_;
        const pid_t pl = pid;
        if (pl > 0 && on_path("pactl")) {
            std::thread([this, gen, pl, p] {
                const std::string key = "application.process.id = \"" + std::to_string(pl) + "\"";
                for (int i = 0; i < 80; ++i) {                 // up to 24 s
                    usleep(300000);
                    if (gen != start_gen_.load()) return;      // another start, or stopped
                    const std::string o = capture({"pactl", "list", "sink-inputs"});
                    if (o.find(key) == std::string::npos) continue;
                    std::lock_guard<std::mutex> l(m_);
                    if (gen != start_gen_.load()) return;
                    started_ = now_s() - p;                    // the clock starts with the sound
                    if (paused_) paused_at_ = now_s();
                    sounding_ = true;
                    return;
                }
                sounding_ = true;                              // never seen: don't say "loading" for ever
            }).detach();
        } else {
            sounding_ = true;
        }
        paused_ = false;
    }

    std::mutex m_;
    struct Art { std::mutex m; std::string bytes; bool fresh{false}; int gen{0}; };
    std::shared_ptr<Art> art_ = std::make_shared<Art>();
    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::vector<Track> results_;
    bool have_results_{false};
    std::string url_, aurl_, title_, error_, note_;
    std::thread dl_;
    double note_at_{-100};
    std::atomic<bool> installing_{false};   // yt-dlp is being fetched (the first YouTube use)
    std::atomic<bool> downloading_{false};
    bool video_{false};
    int vfd_{-1};
    std::thread reader_;
    std::atomic<bool> reader_stop_{false};
    std::mutex fm_;
    std::vector<uint32_t> frame_ = std::vector<uint32_t>(static_cast<size_t>(kVW) * kVH, 0xFF000000u);
    bool frame_new_{false};
    double dur_{0}, started_{0}, paused_at_{0};
    bool inapp_{false};                     // the app's own decoder is playing (no outside player)
    double load_pos_{0};                    // where the track will start from, shown while it loads
    std::atomic<bool> sounding_{true};      // the player's stream has reached the sound server
    std::atomic<int> start_gen_{0};
    bool paused_{false}, ended_{false};
    int vol_{100};   // the player at full: loudness is the device's one volume (fn+S / fn+D)
    std::mutex rm_;
    std::condition_variable rcv_;
    std::string resolving_;
    std::map<std::string, std::pair<std::vector<std::string>, double>> rcache_;
    pid_t pid_{-1};
};

}  // namespace

std::unique_ptr<Music> make_music() { return std::make_unique<LinuxMusic>(); }
#endif

}  // namespace velvet
