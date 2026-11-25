#include "shell.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <sstream>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifndef __EMSCRIPTEN__
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>
extern char** environ;
#endif

namespace velvet {

#ifndef __EMSCRIPTEN__
// ======================================================================== real shell (pty)
class PtyShell : public Shell {
public:
    ~PtyShell() override {
        if (fd_ >= 0) close(fd_);
        if (pid_ > 0) {
            kill(-pid_, SIGHUP);  // the whole session: the shell and its jobs
            kill(pid_, SIGHUP);
            for (int i = 0; i < 20; ++i) {
                if (waitpid(pid_, nullptr, WNOHANG) == pid_) return;
                usleep(5000);
            }
            kill(-pid_, SIGKILL);
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
        }
    }

    bool start(int cols, int rows) override {
        fd_ = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (fd_ < 0) return false;
        if (grantpt(fd_) != 0 || unlockpt(fd_) != 0) return false;
        char slave[128] = {};
        if (ptsname_r(fd_, slave, sizeof slave) != 0) return false;
        winsize ws{};
        ws.ws_row = static_cast<unsigned short>(rows);
        ws.ws_col = static_cast<unsigned short>(cols);
        ws.ws_xpixel = static_cast<unsigned short>(cols * 8);
        ws.ws_ypixel = static_cast<unsigned short>(rows * 14);
        ioctl(fd_, TIOCSWINSZ, &ws);

        // Everything the child needs is prepared before fork (the app has threads, so the
        // child may only call async-signal-safe functions).
        const passwd* pw = getpwuid(getuid());
        std::string sh = pw && pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "";
        if (sh.empty() || access(sh.c_str(), X_OK) != 0) sh = access("/bin/bash", X_OK) == 0 ? "/bin/bash" : "/bin/sh";
        std::string home = pw && pw->pw_dir ? pw->pw_dir : (std::getenv("HOME") ? std::getenv("HOME") : "/");
        name_ = sh.substr(sh.find_last_of('/') + 1);
        const std::string argv0 = "-" + name_;  // login shell: reads ~/.profile like a console login

        std::vector<std::string> env;
        for (char** e = environ; e && *e; ++e) {
            const std::string kv = *e;
            if (kv.rfind("TERM=", 0) == 0 || kv.rfind("COLORTERM=", 0) == 0 || kv.rfind("COLUMNS=", 0) == 0 ||
                kv.rfind("LINES=", 0) == 0 || kv.rfind("SHELL=", 0) == 0 || kv.rfind("HOME=", 0) == 0) {
                continue;
            }
            env.push_back(kv);
        }
        env.push_back("TERM=xterm-256color");
        env.push_back("COLORTERM=truecolor");
        env.push_back("SHELL=" + sh);
        env.push_back("HOME=" + home);
        env.push_back("VELVET_TERMINAL=1");
        std::vector<char*> envp;
        for (auto& s : env) envp.push_back(s.data());
        envp.push_back(nullptr);
        char* argv[] = {const_cast<char*>(argv0.c_str()), nullptr};

        pid_ = fork();
        if (pid_ < 0) return false;
        if (pid_ == 0) {
            setsid();
            const int s = open(slave, O_RDWR);
            if (s < 0) _exit(127);
            ioctl(s, TIOCSCTTY, 0);
            dup2(s, 0);
            dup2(s, 1);
            dup2(s, 2);
            if (s > 2) close(s);
            for (int sig : {SIGPIPE, SIGCHLD, SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGTSTP}) signal(sig, SIG_DFL);
            sigset_t none;
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, nullptr);
            if (chdir(home.c_str()) != 0 && chdir("/") != 0) _exit(127);
            execve(sh.c_str(), argv, envp.data());
            _exit(127);
        }
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
        return true;
    }

    bool is_demo() const override { return false; }

    void resize(int cols, int rows) override {
        if (fd_ < 0) return;
        winsize ws{};
        ws.ws_row = static_cast<unsigned short>(rows);
        ws.ws_col = static_cast<unsigned short>(cols);
        ioctl(fd_, TIOCSWINSZ, &ws);  // the kernel tells the shell (SIGWINCH)
    }

    void write(const std::string& bytes) override {
        size_t off = 0;
        while (fd_ >= 0 && off < bytes.size()) {
            const ssize_t n = ::write(fd_, bytes.data() + off, bytes.size() - off);
            if (n > 0) off += static_cast<size_t>(n);
            else if (n < 0 && errno == EINTR) continue;
            else break;  // full or closed: drop the rest of this key press
        }
    }

    std::string read() override {
        std::string out;
        char buf[4096];
        while (fd_ >= 0 && out.size() < 64 * 1024) {
            const ssize_t n = ::read(fd_, buf, sizeof buf);
            if (n > 0) out.append(buf, static_cast<size_t>(n));
            else if (n < 0 && errno == EINTR) continue;
            else break;
        }
        return out;
    }

    bool alive() override {
        if (pid_ <= 0) return false;
        int st = 0;
        if (waitpid(pid_, &st, WNOHANG) == pid_) {
            pid_ = -1;
            return false;
        }
        return true;
    }

    std::string name() const override { return name_; }

private:
    int fd_{-1};
    pid_t pid_{-1};
    std::string name_;
};
#endif

// ======================================================================== demo shell
// A tiny line-editing shell for the browser build: history, Tab completion, a pretend home
// folder, and enough commands to look around. Speaks the same bytes as a real pty.
class DemoShell : public Shell {
public:
    bool start(int cols, int rows) override {
        cols_ = cols;
        (void)rows;
        started_ = std::chrono::steady_clock::now();
        seed();
        out("\x1b[1;33m CARDPUTERZERO SHELL\x1b[0m \x1b[2m(browser demo)\x1b[0m\r\n");
        out("\x1b[2m The real one is bash on the device.\x1b[0m\r\n");
        out(" Type \x1b[1;31mhelp\x1b[0m.  \x1b[1;31mTab\x1b[0m completes.\r\n");
        prompt();
        alive_ = true;
        return true;
    }

    bool is_demo() const override { return true; }
    bool alive() override { return alive_; }
    void resize(int cols, int rows) override {
        (void)rows;
        cols_ = cols;
        cur_row_ = (plen_ + pos_) / cols_;
        redraw();
    }
    std::string name() const override { return "demo sh"; }

    std::string read() override {
        std::string s;
        s.swap(out_);
        return s;
    }

    void write(const std::string& bytes) override {
        for (char ch : bytes) {
            if (!alive_) return;
            input(static_cast<unsigned char>(ch));
        }
    }

private:
    struct Node {
        bool dir{false};
        std::string data;
    };

    // ------------------------------------------------------------ output helpers
    void out(const std::string& s) { out_ += s; }
    // Program output: plain "\n" becomes "\r\n" like a tty with onlcr.
    void say(const std::string& s) {
        for (char c : s) {
            if (c == '\n') out_ += "\r\n";
            else out_ += c;
        }
    }

    std::string prompt_text() const {
        std::string dir = cwd_;
        if (dir == home_) dir = "~";
        else if (dir.rfind(home_ + "/", 0) == 0) dir = "~" + dir.substr(home_.size());
        return "\x1b[1;33mpi\x1b[0m@\x1b[1;31mzero\x1b[0m:\x1b[1;34m" + dir + "\x1b[0m$ ";
    }
    static size_t visible_len(const std::string& s) {
        size_t n = 0;
        bool esc = false;
        for (char c : s) {
            if (esc) {
                if (c >= 0x40 && c <= 0x7E && c != '[') esc = false;
            } else if (c == '\x1b') {
                esc = true;
            } else {
                ++n;
            }
        }
        return n;
    }

    void prompt() {
        prompt_ = prompt_text();
        plen_ = visible_len(prompt_);
        line_.clear();
        pos_ = 0;
        hist_i_ = static_cast<int>(hist_.size());
        out(prompt_);
        cur_row_ = plen_ > 0 ? (plen_ - 1) / cols_ : 0;
        if (plen_ % cols_ == 0) {
            out(" \r");
            cur_row_ = plen_ / cols_;
        }
    }

    // Redraws prompt + line (which may wrap over several rows) and puts the cursor at pos_.
    void redraw() {
        std::string s;
        if (cur_row_ > 0) s += "\x1b[" + std::to_string(cur_row_) + "A";
        s += "\r\x1b[J" + prompt_ + line_;
        const size_t end = plen_ + line_.size();
        size_t end_row = end / cols_;
        if (end > 0 && end % cols_ == 0) s += " \r";  // leave the pending wrap on a fresh row
        const size_t at = plen_ + pos_;
        const size_t row = at / cols_, col = at % cols_;
        if (end_row > row) s += "\x1b[" + std::to_string(end_row - row) + "A";
        s += "\r";
        if (col) s += "\x1b[" + std::to_string(col) + "C";
        cur_row_ = row;
        out(s);
    }

    void move_to_end() {
        const size_t end = plen_ + line_.size();
        const size_t end_row = end > 0 && end % cols_ == 0 ? end / cols_ - 1 : end / cols_;
        if (end_row > cur_row_) out("\x1b[" + std::to_string(end_row - cur_row_) + "B");
        cur_row_ = end_row;
    }

    // ------------------------------------------------------------ line editing
    void input(unsigned char c) {
        if (in_ == In::Esc) {
            in_ = c == '[' ? In::Csi : c == 'O' ? In::Ss3 : In::Normal;
            seq_.clear();
            return;
        }
        if (in_ == In::Csi || in_ == In::Ss3) {
            if (c >= 0x40 && c <= 0x7E) {
                const std::string s = seq_;
                in_ = In::Normal;
                switch (c) {
                    case 'A': history(-1); break;
                    case 'B': history(1); break;
                    case 'C': if (pos_ < line_.size()) { ++pos_; redraw(); } break;
                    case 'D': if (pos_ > 0) { --pos_; redraw(); } break;
                    case 'H': pos_ = 0; redraw(); break;
                    case 'F': pos_ = line_.size(); redraw(); break;
                    case '~':
                        if (s == "3" && pos_ < line_.size()) { line_.erase(pos_, 1); redraw(); }
                        else if (s == "1" || s == "7") { pos_ = 0; redraw(); }
                        else if (s == "4" || s == "8") { pos_ = line_.size(); redraw(); }
                        break;
                    default: break;
                }
            } else {
                seq_ += static_cast<char>(c);
            }
            return;
        }
        switch (c) {
            case 0x1B: in_ = In::Esc; return;
            case '\r': case '\n': submit(); return;
            case 0x7F: case 0x08:
                if (pos_ > 0) {
                    line_.erase(--pos_, 1);
                    redraw();
                }
                return;
            case 0x03:  // ^C
                move_to_end();
                out("^C\r\n");
                prompt();
                return;
            case 0x04:  // ^D
                if (line_.empty()) {
                    out("logout\r\n");
                    alive_ = false;
                } else if (pos_ < line_.size()) {
                    line_.erase(pos_, 1);
                    redraw();
                }
                return;
            case 0x0C:  // ^L
                out("\x1b[H\x1b[2J");
                cur_row_ = 0;
                redraw();
                return;
            case 0x15: line_.erase(0, pos_); pos_ = 0; redraw(); return;          // ^U
            case 0x0B: line_.erase(pos_); redraw(); return;                       // ^K
            case 0x01: pos_ = 0; redraw(); return;                                // ^A
            case 0x05: pos_ = line_.size(); redraw(); return;                     // ^E
            case 0x02: if (pos_ > 0) { --pos_; redraw(); } return;                // ^B
            case 0x06: if (pos_ < line_.size()) { ++pos_; redraw(); } return;     // ^F
            case 0x10: history(-1); return;                                       // ^P
            case 0x0E: history(1); return;                                        // ^N
            case 0x17: {                                                          // ^W
                size_t p = pos_;
                while (p > 0 && line_[p - 1] == ' ') --p;
                while (p > 0 && line_[p - 1] != ' ') --p;
                line_.erase(p, pos_ - p);
                pos_ = p;
                redraw();
                return;
            }
            case '\t': complete(); return;
            default: break;
        }
        if (c < 0x20 || c >= 0x7F || line_.size() >= 300) return;  // ASCII only in the demo
        line_.insert(pos_++, 1, static_cast<char>(c));
        if (pos_ == line_.size()) {
            out(std::string(1, static_cast<char>(c)));
            const size_t end = plen_ + line_.size();
            cur_row_ = (end - 1) / cols_;
        } else {
            redraw();
        }
    }

    void history(int dir) {
        if (hist_.empty()) return;
        const int n = static_cast<int>(hist_.size());
        if (hist_i_ == n) draft_ = line_;
        const int i = std::clamp(hist_i_ + dir, 0, n);
        if (i == hist_i_) return;
        hist_i_ = i;
        line_ = i == n ? draft_ : hist_[i];
        pos_ = line_.size();
        redraw();
    }

    void complete() {
        size_t start = pos_;
        while (start > 0 && line_[start - 1] != ' ') --start;
        const std::string word = line_.substr(start, pos_ - start);
        const bool first = line_.find_first_not_of(' ') >= start;
        std::vector<std::string> options;
        std::string dir_part, base = word;
        if (first && word.find('/') == std::string::npos) {
            for (const auto& c : commands()) {
                if (c.first.rfind(word, 0) == 0) options.push_back(c.first + " ");
            }
        } else {
            const size_t slash = word.find_last_of('/');
            if (slash != std::string::npos) {
                dir_part = word.substr(0, slash + 1);
                base = word.substr(slash + 1);
            }
            const std::string dir = resolve(dir_part.empty() ? "." : dir_part);
            for (const auto& name : children(dir)) {
                if (name.rfind(base, 0) == 0 && (base.size() || name[0] != '.')) {
                    options.push_back(name + (fs_[join(dir, name)].dir ? "/" : " "));
                }
            }
        }
        if (options.empty()) return;
        std::string common = options[0];
        for (const auto& o : options) {
            size_t k = 0;
            while (k < common.size() && k < o.size() && common[k] == o[k]) ++k;
            common.resize(k);
        }
        const std::string have = first && dir_part.empty() && word.find('/') == std::string::npos ? word : base;
        if (common.size() > have.size()) {
            const std::string add = common.substr(have.size());
            line_.insert(pos_, add);
            pos_ += add.size();
            redraw();
            return;
        }
        // ambiguous: list the options under the line
        move_to_end();
        out("\r\n");
        std::string row;
        for (const auto& o : options) {
            std::string shown = o.back() == ' ' ? o.substr(0, o.size() - 1) : o;
            if (row.size() + shown.size() + 2 > static_cast<size_t>(cols_)) {
                out(row + "\r\n");
                row.clear();
            }
            row += shown + "  ";
        }
        out(row + "\r\n");
        const std::string keep = line_;
        const size_t keep_pos = pos_;
        prompt();
        line_ = keep;
        pos_ = keep_pos;
        redraw();
    }

    void submit() {
        move_to_end();
        out("\r\n");
        std::string cmd = line_;
        const size_t a = cmd.find_first_not_of(' ');
        if (a != std::string::npos) {
            cmd = cmd.substr(a);
            if (hist_.empty() || hist_.back() != cmd) hist_.push_back(cmd);
            if (hist_.size() > 100) hist_.erase(hist_.begin());
            run(cmd);
        }
        if (alive_) prompt();
    }

    // ------------------------------------------------------------ file system
    void seed() {
        auto dir = [&](const std::string& p) { fs_[p] = Node{true, ""}; };
        auto file = [&](const std::string& p, const std::string& d) { fs_[p] = Node{false, d}; };
        dir("/");
        dir("/home");
        dir(home_);
        dir(home_ + "/notes");
        dir(home_ + "/projects");
        dir("/etc");
        dir("/tmp");
        dir("/usr");
        dir("/usr/bin");
        file(home_ + "/README.txt",
             "This is the browser demo of the\n"
             "CardputerZero terminal. Files here live in memory and\n"
             "reset when the page reloads.\n\n"
             "On the CardputerZero the same page runs\n"
             "your real login shell (bash).\n");
        file(home_ + "/schedule.txt",
             "TODAY  Print minideck hinge v3\n"
             "TUE    Order LoRa cap for CZ\n"
             "WED    Riftblade room 4 playtest\n"
             "FRI    Math quiz: chapter 3\n");
        file(home_ + "/notes/hinge.txt",
             "Friction hinge needs 0.2 mm more\nclearance. Try PETG and print it flat.\n");
        file(home_ + "/notes/lora.txt",
             "Two caps, 1.2 km line of sight.\nNext: test through the school building.\n");
        file(home_ + "/notes/riftblade.txt",
             "Room 4 wall jump is too strict. Widen\nthe ledge grab window by 2 frames.\n");
        file(home_ + "/projects/hello.sh", "#!/bin/sh\necho \"hello from the zero\"\n");
        file(home_ + "/.bashrc", "# demo shell ignores this\nalias ll='ls -l'\n");
        file("/etc/hostname", "zero\n");
        file("/etc/os-release",
             "PRETTY_NAME=\"CardputerZero on Raspberry Pi OS\"\nNAME=\"Debian GNU/Linux\"\nVERSION_ID=\"13\"\n");
        file("/etc/motd", "Welcome to the CardputerZero.\n");
        for (const auto& c : commands()) file("/usr/bin/" + c.first, "(demo command)\n");
    }

    static std::string join(const std::string& dir, const std::string& name) {
        return dir == "/" ? "/" + name : dir + "/" + name;
    }

    std::string resolve(std::string p) const {
        if (p == "~" || p.rfind("~/", 0) == 0) p = home_ + p.substr(1);
        if (p.empty() || p[0] != '/') p = cwd_ + "/" + p;
        std::vector<std::string> parts;
        std::stringstream ss(p);
        std::string part;
        while (std::getline(ss, part, '/')) {
            if (part.empty() || part == ".") continue;
            if (part == "..") {
                if (!parts.empty()) parts.pop_back();
            } else {
                parts.push_back(part);
            }
        }
        std::string r;
        for (const auto& x : parts) r += "/" + x;
        return r.empty() ? "/" : r;
    }

    std::vector<std::string> children(const std::string& dir) const {
        std::vector<std::string> names;
        const std::string pre = dir == "/" ? "/" : dir + "/";
        for (auto it = fs_.lower_bound(pre); it != fs_.end() && it->first.rfind(pre, 0) == 0; ++it) {
            const std::string rest = it->first.substr(pre.size());
            if (!rest.empty() && rest.find('/') == std::string::npos) names.push_back(rest);
        }
        return names;
    }

    bool exists(const std::string& p) const { return fs_.count(p) != 0; }
    bool is_dir(const std::string& p) const {
        auto it = fs_.find(p);
        return it != fs_.end() && it->second.dir;
    }
    std::string parent(const std::string& p) const {
        const size_t s = p.find_last_of('/');
        return s == 0 ? "/" : p.substr(0, s);
    }

    // ------------------------------------------------------------ commands
    using Args = std::vector<std::string>;
    using Fn = std::function<int(DemoShell&, const Args&, std::string&)>;

    static std::vector<std::string> split(const std::string& s) {
        std::vector<std::string> v;
        std::string cur;
        char quote = 0;
        bool any = false;
        for (char c : s) {
            if (quote) {
                if (c == quote) quote = 0;
                else cur += c;
            } else if (c == '"' || c == '\'') {
                quote = c;
                any = true;
            } else if (c == ' ') {
                if (!cur.empty() || any) v.push_back(cur);
                cur.clear();
                any = false;
            } else {
                cur += c;
            }
        }
        if (!cur.empty() || any) v.push_back(cur);
        return v;
    }

    void run(const std::string& line) {
        if (line.find('|') != std::string::npos) {
            say("demo sh: pipes need the real shell\n");
            return;
        }
        Args args = split(line);
        // > file  /  >> file
        std::string redirect;
        bool append = false;
        for (size_t i = 0; i < args.size(); ++i) {
            if ((args[i] == ">" || args[i] == ">>") && i + 1 < args.size()) {
                append = args[i] == ">>";
                redirect = args[i + 1];
                args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
                break;
            }
        }
        if (args.empty()) return;
        if (args[0] == "ll") args = [&] { Args a{"ls", "-l"}; a.insert(a.end(), args.begin() + 1, args.end()); return a; }();
        const auto& cmds = commands();
        auto it = cmds.find(args[0]);
        std::string text;
        if (it == cmds.end()) {
            say("demo sh: " + args[0] + ": command not found\n");
            return;
        }
        it->second.second(*this, args, text);
        if (!redirect.empty()) {
            const std::string p = resolve(redirect);
            if (is_dir(p) || !is_dir(parent(p))) {
                say("demo sh: " + redirect + ": cannot write\n");
                return;
            }
            Node& n = fs_[p];
            n.dir = false;
            n.data = append ? n.data + strip(text) : strip(text);
            return;
        }
        say(text);
    }

    static std::string strip(const std::string& s) {  // drop colour codes
        std::string r;
        bool esc = false;
        for (char c : s) {
            if (esc) {
                if (c >= 0x40 && c <= 0x7E && c != '[') esc = false;
            } else if (c == '\x1b') {
                esc = true;
            } else {
                r += c;
            }
        }
        return r;
    }

    std::string uptime_text() const {
        const auto s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count();
        char buf[48];
        if (s < 60) std::snprintf(buf, sizeof buf, "%lld sec", static_cast<long long>(s));
        else if (s < 3600) std::snprintf(buf, sizeof buf, "%lld min", static_cast<long long>(s / 60));
        else std::snprintf(buf, sizeof buf, "%lldh %02lldm", static_cast<long long>(s / 3600), static_cast<long long>(s / 60 % 60));
        return buf;
    }

    // name -> (one-line help, function)
    static const std::map<std::string, std::pair<const char*, Fn>>& commands() {
        static const std::map<std::string, std::pair<const char*, Fn>> kCmds = {
            {"help", {"this list", [](DemoShell& sh, const Args&, std::string& o) {
                 o += "\x1b[1;33mcommands\x1b[0m\n";
                 std::string row;
                 for (const auto& c : commands()) {
                     std::string n = c.first;
                     n.resize(9, ' ');
                     if (row.size() + n.size() > static_cast<size_t>(sh.cols_ - 1)) {
                         o += row + "\n";
                         row.clear();
                     }
                     row += n;
                 }
                 o += row + "\n\x1b[2mman <cmd> explains one. Tab completes, Up/Down\nhistory, Ctrl+C, Ctrl+L.\x1b[0m\n";
                 return 0;
             }}},
            {"man", {"explain a command", [](DemoShell&, const Args& a, std::string& o) {
                 if (a.size() < 2) { o += "usage: man <command>\n"; return 1; }
                 auto it = commands().find(a[1]);
                 o += it == commands().end() ? "no entry for " + a[1] + "\n" : a[1] + ": " + it->second.first + "\n";
                 return 0;
             }}},
            {"ls", {"list files (-l, -a)", [](DemoShell& sh, const Args& a, std::string& o) {
                 bool lng = false, all = false;
                 std::vector<std::string> paths;
                 for (size_t i = 1; i < a.size(); ++i) {
                     if (a[i][0] == '-') {
                         lng |= a[i].find('l') != std::string::npos;
                         all |= a[i].find('a') != std::string::npos;
                     } else {
                         paths.push_back(a[i]);
                     }
                 }
                 if (paths.empty()) paths.push_back(".");
                 for (const auto& p : paths) {
                     const std::string r = sh.resolve(p);
                     if (!sh.exists(r)) { o += "ls: " + p + ": no such file\n"; continue; }
                     std::vector<std::string> names;
                     std::string base = r;
                     if (sh.is_dir(r)) names = sh.children(r);
                     else { names.push_back(r.substr(r.find_last_of('/') + 1)); base = sh.parent(r); }
                     if (paths.size() > 1) o += p + ":\n";
                     std::string row;
                     for (const auto& n : names) {
                         if (!all && n[0] == '.') continue;
                         const Node& node = sh.fs_.at(join(base, n));
                         const std::string col = node.dir ? "\x1b[1;34m" : n.size() > 3 && n.substr(n.size() - 3) == ".sh" ? "\x1b[1;32m" : "";
                         const std::string shown = col + n + (col.empty() ? "" : "\x1b[0m") + (node.dir ? "/" : "");
                         if (lng) {
                             char buf[64];
                             std::snprintf(buf, sizeof buf, "%s pi %5zu ", node.dir ? "drwxr-xr-x" : "-rw-r--r--",
                                           node.dir ? static_cast<size_t>(4096) : node.data.size());
                             o += buf + shown + "\n";
                         } else {
                             const size_t w = n.size() + (node.dir ? 1 : 0) + 2;
                             if (visible_len(row) + w > static_cast<size_t>(sh.cols_)) { o += row + "\n"; row.clear(); }
                             row += shown + "  ";
                         }
                     }
                     if (!row.empty()) o += row + "\n";
                 }
                 return 0;
             }}},
            {"cd", {"change folder", [](DemoShell& sh, const Args& a, std::string& o) {
                 const std::string r = sh.resolve(a.size() > 1 ? a[1] : "~");
                 if (!sh.is_dir(r)) { o += "cd: " + (a.size() > 1 ? a[1] : "~") + ": not a folder\n"; return 1; }
                 sh.cwd_ = r;
                 return 0;
             }}},
            {"pwd", {"print current folder", [](DemoShell& sh, const Args&, std::string& o) { o += sh.cwd_ + "\n"; return 0; }}},
            {"cat", {"print files", [](DemoShell& sh, const Args& a, std::string& o) {
                 if (a.size() < 2) { o += "usage: cat <file>\n"; return 1; }
                 for (size_t i = 1; i < a.size(); ++i) {
                     const std::string r = sh.resolve(a[i]);
                     if (!sh.exists(r)) o += "cat: " + a[i] + ": no such file\n";
                     else if (sh.is_dir(r)) o += "cat: " + a[i] + ": is a folder\n";
                     else o += sh.fs_[r].data;
                 }
                 return 0;
             }}},
            {"echo", {"print text", [](DemoShell&, const Args& a, std::string& o) {
                 for (size_t i = 1; i < a.size(); ++i) o += (i > 1 ? " " : "") + a[i];
                 o += "\n";
                 return 0;
             }}},
            {"touch", {"make an empty file", [](DemoShell& sh, const Args& a, std::string& o) {
                 for (size_t i = 1; i < a.size(); ++i) {
                     const std::string r = sh.resolve(a[i]);
                     if (!sh.is_dir(sh.parent(r))) o += "touch: " + a[i] + ": no such folder\n";
                     else if (!sh.exists(r)) sh.fs_[r] = Node{false, ""};
                 }
                 return 0;
             }}},
            {"mkdir", {"make a folder", [](DemoShell& sh, const Args& a, std::string& o) {
                 for (size_t i = 1; i < a.size(); ++i) {
                     if (a[i] == "-p") continue;
                     const std::string r = sh.resolve(a[i]);
                     if (sh.exists(r)) o += "mkdir: " + a[i] + ": exists\n";
                     else if (!sh.is_dir(sh.parent(r))) o += "mkdir: " + a[i] + ": no such folder\n";
                     else sh.fs_[r] = Node{true, ""};
                 }
                 return 0;
             }}},
            {"rm", {"delete (-r for folders)", [](DemoShell& sh, const Args& a, std::string& o) {
                 bool rec = false;
                 for (size_t i = 1; i < a.size(); ++i) {
                     if (a[i][0] == '-') { rec |= a[i].find('r') != std::string::npos; continue; }
                     const std::string r = sh.resolve(a[i]);
                     if (!sh.exists(r) || r == "/") { o += "rm: " + a[i] + ": no such file\n"; continue; }
                     if (sh.is_dir(r) && !rec) { o += "rm: " + a[i] + ": is a folder (use -r)\n"; continue; }
                     const std::string pre = r + "/";
                     for (auto it = sh.fs_.begin(); it != sh.fs_.end();) {
                         if (it->first == r || it->first.rfind(pre, 0) == 0) it = sh.fs_.erase(it);
                         else ++it;
                     }
                     if (sh.cwd_ == r || sh.cwd_.rfind(pre, 0) == 0) sh.cwd_ = sh.parent(r);
                 }
                 return 0;
             }}},
            {"mv", {"move / rename", [](DemoShell& sh, const Args& a, std::string& o) {
                 if (a.size() != 3) { o += "usage: mv <from> <to>\n"; return 1; }
                 const std::string from = sh.resolve(a[1]);
                 std::string to = sh.resolve(a[2]);
                 if (!sh.exists(from) || from == "/") { o += "mv: " + a[1] + ": no such file\n"; return 1; }
                 if (sh.is_dir(to)) to = join(to, from.substr(from.find_last_of('/') + 1));
                 if (to == from || to.rfind(from + "/", 0) == 0 || !sh.is_dir(sh.parent(to))) { o += "mv: cannot move there\n"; return 1; }
                 std::map<std::string, Node> moved;
                 for (auto it = sh.fs_.begin(); it != sh.fs_.end();) {
                     if (it->first == from || it->first.rfind(from + "/", 0) == 0) {
                         moved[to + it->first.substr(from.size())] = it->second;
                         it = sh.fs_.erase(it);
                     } else {
                         ++it;
                     }
                 }
                 for (auto& m : moved) sh.fs_[m.first] = m.second;
                 return 0;
             }}},
            {"cp", {"copy a file", [](DemoShell& sh, const Args& a, std::string& o) {
                 if (a.size() != 3) { o += "usage: cp <from> <to>\n"; return 1; }
                 const std::string from = sh.resolve(a[1]);
                 std::string to = sh.resolve(a[2]);
                 if (!sh.exists(from) || sh.is_dir(from)) { o += "cp: " + a[1] + ": not a file\n"; return 1; }
                 if (sh.is_dir(to)) to = join(to, from.substr(from.find_last_of('/') + 1));
                 if (!sh.is_dir(sh.parent(to))) { o += "cp: no such folder\n"; return 1; }
                 sh.fs_[to] = sh.fs_[from];
                 return 0;
             }}},
            {"tree", {"folder tree", [](DemoShell& sh, const Args& a, std::string& o) {
                 const std::string root = sh.resolve(a.size() > 1 ? a[1] : ".");
                 if (!sh.is_dir(root)) { o += "tree: not a folder\n"; return 1; }
                 o += "\x1b[1;34m" + (a.size() > 1 ? a[1] : std::string(".")) + "\x1b[0m\n";
                 std::function<void(const std::string&, const std::string&)> walk = [&](const std::string& d, const std::string& pad) {
                     auto names = sh.children(d);
                     names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& n) { return n[0] == '.'; }), names.end());
                     for (size_t i = 0; i < names.size(); ++i) {
                         const bool last = i + 1 == names.size();
                         const std::string p = join(d, names[i]);
                         const bool dir = sh.is_dir(p);
                         o += pad + (last ? "\xE2\x94\x94\xE2\x94\x80 " : "\xE2\x94\x9C\xE2\x94\x80 ") +
                              (dir ? "\x1b[1;34m" + names[i] + "\x1b[0m" : names[i]) + "\n";
                         if (dir) walk(p, pad + (last ? "   " : "\xE2\x94\x82  "));
                     }
                 };
                 walk(root, "");
                 return 0;
             }}},
            {"clear", {"clear the screen", [](DemoShell&, const Args&, std::string& o) { o += "\x1b[H\x1b[2J"; return 0; }}},
            {"date", {"date and time", [](DemoShell&, const Args&, std::string& o) {
                 std::time_t t = std::time(nullptr);
                 std::tm tm{};
                 localtime_r(&t, &tm);
                 char buf[64];
                 std::strftime(buf, sizeof buf, "%a %b %e %H:%M:%S %Y", &tm);
                 o += std::string(buf) + "\n";
                 return 0;
             }}},
            {"uptime", {"time since start", [](DemoShell& sh, const Args&, std::string& o) { o += "up " + sh.uptime_text() + "\n"; return 0; }}},
            {"uname", {"system name (-a)", [](DemoShell&, const Args& a, std::string& o) {
                 o += a.size() > 1 && a[1] == "-a" ? "Linux zero 6.12.25-v8 aarch64 GNU/Linux (demo)\n" : "Linux\n";
                 return 0;
             }}},
            {"whoami", {"your user name", [](DemoShell&, const Args&, std::string& o) { o += "pi\n"; return 0; }}},
            {"hostname", {"device name", [](DemoShell&, const Args&, std::string& o) { o += "zero\n"; return 0; }}},
            {"history", {"commands you typed", [](DemoShell& sh, const Args&, std::string& o) {
                 for (size_t i = 0; i < sh.hist_.size(); ++i) {
                     char buf[32];
                     std::snprintf(buf, sizeof buf, "%4zu  ", i + 1);
                     o += buf + sh.hist_[i] + "\n";
                 }
                 return 0;
             }}},
            {"free", {"memory", [](DemoShell&, const Args&, std::string& o) {
                 o += "       total   used   free\nMem:    416M   97M   319M\nSwap:   511M    0B   511M\n";
                 return 0;
             }}},
            {"df", {"disk space", [](DemoShell&, const Args&, std::string& o) {
                 o += "Filesystem  Size  Used Avail Use%\n/dev/root    29G  3.1G   25G  11%\n";
                 return 0;
             }}},
            {"neofetch", {"system info, with style", [](DemoShell& sh, const Args&, std::string& o) {
                 static const char* const kArt[] = {
                     "\x1b[1;37m ________ ", "\x1b[1;37m|\x1b[1;34m  ____  \x1b[1;37m|",
                     "\x1b[1;37m|\x1b[1;34m |\x1b[1;33m>_\x1b[1;34m  | \x1b[1;37m|", "\x1b[1;37m|\x1b[1;34m |____| \x1b[1;37m|",
                     "\x1b[1;37m|\x1b[1;31m  ::::  \x1b[1;37m|", "\x1b[1;37m|\x1b[1;31m  ::::  \x1b[1;37m|",
                     "\x1b[1;37m|________|", "          ", "          ",
                 };
                 const std::string info[] = {
                     "\x1b[1;33mpi\x1b[0m@\x1b[1;31mzero\x1b[0m",
                     "-------",
                     "\x1b[1;31mOS\x1b[0m Pi OS",
                     "\x1b[1;31mHOST\x1b[0m CardputerZero",
                     "\x1b[1;31mCPU\x1b[0m Pi CM0 4x A53",
                     "\x1b[1;31mUP\x1b[0m " + sh.uptime_text(),
                     "\x1b[1;31mSHELL\x1b[0m demo sh",
                     "\x1b[1;31mLCD\x1b[0m 320x170",
                     "\x1b[40m  \x1b[41m  \x1b[42m  \x1b[43m  \x1b[44m  \x1b[45m  \x1b[46m  \x1b[47m  \x1b[0m",
                 };
                 for (int i = 0; i < 9; ++i) o += std::string(kArt[i]) + "\x1b[0m " + info[i] + "\n";
                 return 0;
             }}},
            {"calc", {"math: calc 2*(3+4)", [](DemoShell&, const Args& a, std::string& o) {
                 std::string e;
                 for (size_t i = 1; i < a.size(); ++i) e += a[i];
                 size_t p = 0;
                 bool ok = true;
                 std::function<double()> expr, term, factor, unary;
                 auto skip = [&] { while (p < e.size() && e[p] == ' ') ++p; };
                 unary = [&]() -> double {
                     skip();
                     if (p < e.size() && e[p] == '-') { ++p; return -unary(); }
                     if (p < e.size() && e[p] == '+') { ++p; return unary(); }
                     if (p < e.size() && e[p] == '(') {
                         ++p;
                         const double v = expr();
                         skip();
                         if (p < e.size() && e[p] == ')') ++p; else ok = false;
                         return v;
                     }
                     const size_t s = p;
                     while (p < e.size() && (std::isdigit(static_cast<unsigned char>(e[p])) || e[p] == '.')) ++p;
                     if (s == p) { ok = false; return 0; }
                     return std::atof(e.substr(s, p - s).c_str());
                 };
                 factor = [&]() -> double {
                     const double b = unary();
                     skip();
                     if (p < e.size() && e[p] == '^') { ++p; return std::pow(b, factor()); }
                     return b;
                 };
                 term = [&]() -> double {
                     double v = factor();
                     for (;;) {
                         skip();
                         if (p < e.size() && e[p] == '*') { ++p; v *= factor(); }
                         else if (p < e.size() && e[p] == '/') { ++p; v /= factor(); }
                         else if (p < e.size() && e[p] == '%') { ++p; v = std::fmod(v, factor()); }
                         else return v;
                     }
                 };
                 expr = [&]() -> double {
                     double v = term();
                     for (;;) {
                         skip();
                         if (p < e.size() && e[p] == '+') { ++p; v += term(); }
                         else if (p < e.size() && e[p] == '-') { ++p; v -= term(); }
                         else return v;
                     }
                 };
                 if (e.empty()) { o += "usage: calc 2*(3+4)/5\n"; return 1; }
                 const double v = expr();
                 skip();
                 if (!ok || p != e.size()) { o += "calc: can't read that\n"; return 1; }
                 char buf[64];
                 std::snprintf(buf, sizeof buf, "%.10g\n", v);
                 o += buf;
                 return 0;
             }}},
            {"fortune", {"a line for the day", [](DemoShell&, const Args&, std::string& o) {
                 static const char* const kLines[] = {
                     "Take your time.", "The moon looks different tonight.",
                     "Your code compiled first try. Suspicious.", "Print it flat. Trust the hinge.",
                     "Every day is a new save file.", "A bond, once made, is a kind of power.",
                     "Sleep stat is low. Consider it.",
                 };
                 o += std::string(kLines[std::time(nullptr) % 7]) + "\n";
                 return 0;
             }}},
            {"sudo", {"no", [](DemoShell&, const Args&, std::string& o) { o += "nice try. (demo shell)\n"; return 1; }}},
            {"apt", {"packages (device only)", [](DemoShell&, const Args&, std::string& o) {
                 o += "apt runs on the real CardputerZero.\n";
                 return 1;
             }}},
            {"nano", {"editor (device only)", [](DemoShell&, const Args&, std::string& o) {
                 o += "editors run on the device. Here:\necho text > file  /  cat file\n";
                 return 1;
             }}},
            {"exit", {"end the session", [](DemoShell& sh, const Args&, std::string& o) {
                 o += "logout\n";
                 sh.alive_ = false;
                 return 0;
             }}},
        };
        return kCmds;
    }

    enum class In { Normal, Esc, Csi, Ss3 } in_{In::Normal};
    std::string seq_;
    std::string out_;
    bool alive_{false};
    int cols_{40};
    std::string prompt_;
    size_t plen_{0};
    std::string line_;
    size_t pos_{0};
    size_t cur_row_{0};
    std::vector<std::string> hist_;
    int hist_i_{0};
    std::string draft_;
    std::map<std::string, Node> fs_;
    std::string home_{"/home/pi"};
    std::string cwd_{"/home/pi"};
    std::chrono::steady_clock::time_point started_;
};

#ifdef __EMSCRIPTEN__
// Inside Optic Suite the page can run a real bash in Optic's own Debian (window.VelvetAndroid's
// shellStart / shellWrite / shellResize); its output comes back through velvet_shell_data.
namespace {
std::string g_host_out;
bool g_host_dead = false;
}  // namespace
extern "C" EMSCRIPTEN_KEEPALIVE void velvet_shell_data(const char* data, int len) { g_host_out.append(data, static_cast<size_t>(len)); }
extern "C" EMSCRIPTEN_KEEPALIVE void velvet_shell_exit(int) { g_host_dead = true; }
EM_JS(int, host_shell_start, (int cols, int rows), {
    try { const h = window.VelvetAndroid; if (!h || !h.shellStart) return -1; return h.shellStart(cols, rows); } catch (e) { return -1; }
});
EM_JS(void, host_shell_write, (const char* p, int n), {
    let s = ''; for (let i = 0; i < n; i++) s += String.fromCharCode(HEAPU8[p + i]);
    try { window.VelvetAndroid.shellWrite(btoa(s)); } catch (e) {}
});
EM_JS(void, host_shell_resize, (int cols, int rows), { try { window.VelvetAndroid.shellResize(cols, rows); } catch (e) {} });

class HostShell : public Shell {
public:
    bool start(int cols, int rows) override {
        g_host_out.clear();
        g_host_dead = false;
        return host_shell_start(cols, rows) >= 0;
    }
    bool is_demo() const override { return false; }
    void write(const std::string& b) override { if (!b.empty()) host_shell_write(b.data(), static_cast<int>(b.size())); }
    std::string read() override { std::string out; out.swap(g_host_out); return out; }
    bool alive() override { return !g_host_dead; }
    void resize(int cols, int rows) override { host_shell_resize(cols, rows); }
    std::string name() const override { return "bash (Optic Debian)"; }
};
#endif

std::unique_ptr<Shell> make_shell(int cols, int rows) {
#ifdef __EMSCRIPTEN__
    {
        auto host = std::make_unique<HostShell>();
        if (host->start(cols, rows)) return host;
    }
#endif
#ifndef __EMSCRIPTEN__
    const char* demo = std::getenv("VELVET_DEMO_SHELL");
    if (!demo || std::strcmp(demo, "1") != 0) {
        auto pty = std::make_unique<PtyShell>();
        if (pty->start(cols, rows)) return pty;
    }
#endif
    auto sh = std::make_unique<DemoShell>();
    sh->start(cols, rows);
    return sh;
}

}  // namespace velvet
