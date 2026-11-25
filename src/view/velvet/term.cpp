#include "term.h"

#include "keys.h"
#include "lvgl.h"

#include <algorithm>
#include <cstdio>

namespace velvet {

namespace {
// DEC special graphics (ESC ( 0): what ncurses uses for line drawing with TERM=xterm.
uint32_t dec_graphics(uint32_t c) {
    switch (c) {
        case '`': return 0x25C6;
        case 'a': return 0x2592;
        case 'f': return 0xB0;
        case 'g': return 0xB1;
        case 'j': return 0x2518;
        case 'k': return 0x2510;
        case 'l': return 0x250C;
        case 'm': return 0x2514;
        case 'n': return 0x253C;
        case 'o': case 'p': case 'q': case 'r': case 's': return 0x2500;
        case 't': return 0x251C;
        case 'u': return 0x2524;
        case 'v': return 0x2534;
        case 'w': return 0x252C;
        case 'x': return 0x2502;
        case 'y': return 0x2264;
        case 'z': return 0x2265;
        case '~': return 0xB7;
        default: return c;
    }
}

void utf8_append(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}
}  // namespace

Terminal::Terminal(int cols, int rows, int scrollback)
    : cols_(cols), rows_(std::min(rows, 64)), scrollback_(scrollback), screen_(&main_) {
    reset();
}

void Terminal::reset() {
    main_.assign(rows_, Line(cols_));
    alt_lines_.assign(rows_, Line(cols_));
    screen_ = &main_;
    alt_ = false;
    history_.clear();
    cx_ = cy_ = 0;
    wrap_pending_ = false;
    top_ = 0;
    bottom_ = rows_ - 1;
    pen_ = Cell{};
    saved_ = saved_alt_ = Saved{};
    cursor_on_ = true;
    app_cursor_ = false;
    autowrap_ = true;
    insert_ = false;
    origin_ = false;
    view_offset_ = 0;
    g0_graphics_ = g1_graphics_ = shift_out_ = false;
    st_ = St::Ground;
    utf_left_ = 0;
    mark_all();
}

void Terminal::resize(int cols, int rows) {
    rows = std::clamp(rows, 1, 64);
    cols = std::max(cols, 1);
    if (cols == cols_ && rows == rows_) return;
    auto fit = [cols](Line& l) { l.resize(cols); };

    // main screen: history + the used part of the screen, then keep the last `rows` lines
    int used = 0;
    for (int y = 0; y < rows_; ++y) {
        for (const Cell& c : main_[y]) {
            if (c.ch != ' ' || c.bg >= 0) {
                used = y + 1;
                break;
            }
        }
    }
    const int cur = alt_ ? saved_alt_.y : cy_;
    used = std::max(used, cur + 1);
    std::vector<Line> all(history_.begin(), history_.end());
    all.insert(all.end(), main_.begin(), main_.begin() + used);
    const int hist = static_cast<int>(history_.size());
    const int start = std::max(0, hist + used - rows);
    history_.assign(all.begin(), all.begin() + start);
    main_.assign(all.begin() + start, all.end());
    main_.resize(rows, Line(cols_));
    const int new_cur = hist + cur - start;
    for (auto& l : history_) fit(l);
    for (auto& l : main_) fit(l);

    alt_lines_.resize(rows, Line(cols));
    for (auto& l : alt_lines_) fit(l);

    cols_ = cols;
    rows_ = rows;
    if (alt_) {
        saved_alt_.y = std::clamp(new_cur, 0, rows - 1);
        cy_ = std::min(cy_, rows - 1);
    } else {
        cy_ = std::clamp(new_cur, 0, rows - 1);
    }
    cx_ = std::min(cx_, cols - 1);
    saved_.x = std::min(saved_.x, cols - 1);
    saved_.y = std::min(saved_.y, rows - 1);
    saved_alt_.x = std::min(saved_alt_.x, cols - 1);
    top_ = 0;
    bottom_ = rows - 1;
    wrap_pending_ = false;
    view_offset_ = 0;
    screen_ = alt_ ? &alt_lines_ : &main_;
    mark_all();
}

void Terminal::clear_above() {
    if (alt_ || cy_ == 0) return;  // full-screen programs draw their own screen
    main_[0] = main_[cy_];
    for (int y = 1; y < rows_; ++y) erase(main_[y], 0, cols_);
    cy_ = 0;
    view_offset_ = 0;
    mark_all();
}

const Cell& Terminal::view_cell(int x, int y) const {
    if (view_offset_ == 0 || alt_) return (*screen_)[y][x];
    const int hist = static_cast<int>(history_.size());
    const int idx = hist - view_offset_ + y;
    if (idx < hist) return history_[idx][x];
    return main_[idx - hist][x];
}

bool Terminal::scroll_view(int delta) {
    if (alt_) return false;
    const int v = std::clamp(view_offset_ + delta, 0, static_cast<int>(history_.size()));
    if (v == view_offset_) return false;
    view_offset_ = v;
    mark_all();
    return true;
}

std::string Terminal::take_reply() {
    std::string r;
    r.swap(reply_);
    return r;
}

uint64_t Terminal::take_dirty() {
    const uint64_t d = dirty_;
    dirty_ = 0;
    return d;
}

Cell Terminal::blank() const {
    Cell c;
    c.bg = pen_.bg;  // erase with the current background colour (xterm "bce")
    return c;
}

void Terminal::erase(Line& l, int from, int to) {
    from = std::max(from, 0);
    to = std::min(to, cols_);
    for (int x = from; x < to; ++x) l[x] = blank();
}

void Terminal::clamp_cursor() {
    cx_ = std::clamp(cx_, 0, cols_ - 1);
    cy_ = std::clamp(cy_, 0, rows_ - 1);
    wrap_pending_ = false;
}

int Terminal::param(size_t i, int def) const {
    if (i >= params_.size() || params_[i] <= 0) return def;
    return params_[i];
}

// ---------------------------------------------------------------- scrolling
void Terminal::scroll_up(int top, int bottom, int n, bool to_history) {
    n = std::min(n, bottom - top + 1);
    for (int i = 0; i < n; ++i) {
        if (to_history && !alt_ && top == 0) {
            history_.push_back(line(top));
            if (static_cast<int>(history_.size()) > scrollback_) history_.pop_front();
            else if (view_offset_ > 0) ++view_offset_;  // keep a scrolled-back view still
        }
        for (int y = top; y < bottom; ++y) line(y).swap(line(y + 1));
        erase(line(bottom), 0, cols_);
    }
    mark_all();
}

void Terminal::scroll_down(int top, int bottom, int n) {
    n = std::min(n, bottom - top + 1);
    for (int i = 0; i < n; ++i) {
        for (int y = bottom; y > top; --y) line(y).swap(line(y - 1));
        erase(line(top), 0, cols_);
    }
    mark_all();
}

void Terminal::linefeed() {
    if (cy_ == bottom_) scroll_up(top_, bottom_, 1, true);
    else if (cy_ < rows_ - 1) ++cy_;
    mark(cy_);
}

void Terminal::reverse_index() {
    if (cy_ == top_) scroll_down(top_, bottom_, 1);
    else if (cy_ > 0) --cy_;
    mark(cy_);
}

void Terminal::use_alt(bool on) {
    if (on == alt_) return;
    alt_ = on;
    screen_ = on ? &alt_lines_ : &main_;
    view_offset_ = 0;
    mark_all();
}

// ---------------------------------------------------------------- output
void Terminal::put(uint32_t cp) {
    if (cp < 0x80 && (shift_out_ ? g1_graphics_ : g0_graphics_)) cp = dec_graphics(cp);
    if (wrap_pending_) {
        wrap_pending_ = false;
        if (autowrap_) {
            cx_ = 0;
            linefeed();
        }
    }
    Line& l = line(cy_);
    if (insert_) {
        for (int x = cols_ - 1; x > cx_; --x) l[x] = l[x - 1];
    }
    Cell c = pen_;
    c.ch = cp;
    l[cx_] = c;
    last_char_ = cp;
    mark(cy_);
    if (cx_ == cols_ - 1) wrap_pending_ = true;
    else ++cx_;
}

void Terminal::control(uint8_t c) {
    switch (c) {
        case 0x08:  // BS
            if (cx_ > 0) --cx_;
            wrap_pending_ = false;
            break;
        case 0x09:  // HT
            cx_ = std::min(cols_ - 1, (cx_ / 8 + 1) * 8);
            wrap_pending_ = false;
            break;
        case 0x0A: case 0x0B: case 0x0C:
            linefeed();
            break;
        case 0x0D:
            cx_ = 0;
            wrap_pending_ = false;
            break;
        case 0x0E: shift_out_ = true; break;
        case 0x0F: shift_out_ = false; break;
        default: break;  // BEL and the rest: ignored
    }
}

void Terminal::feed(const char* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const auto b = static_cast<uint8_t>(data[i]);

        // strings (OSC title, DCS, ...) end with BEL or ESC backslash
        if (st_ == St::Osc || st_ == St::Str) {
            if (b == 0x07) {
                if (st_ == St::Osc) osc_done();
                st_ = St::Ground;
            } else if (b == 0x1B) {
                st_ = st_ == St::Osc ? St::OscEsc : St::StrEsc;
            } else if (st_ == St::Osc && osc_.size() < 256) {
                osc_ += static_cast<char>(b);
            }
            continue;
        }
        if (st_ == St::OscEsc || st_ == St::StrEsc) {
            if (st_ == St::OscEsc) osc_done();
            st_ = St::Ground;
            if (b == '\\') continue;
            // anything else: treat this byte as the start of a new sequence
        }

        if (b == 0x1B) {
            st_ = St::Esc;
            utf_left_ = 0;
            continue;
        }
        if (b == 0x18 || b == 0x1A) {  // CAN / SUB abort a sequence
            st_ = St::Ground;
            continue;
        }

        switch (st_) {
            case St::Ground:
                if (utf_left_ > 0) {
                    if ((b & 0xC0) == 0x80) {
                        utf_cp_ = (utf_cp_ << 6) | (b & 0x3F);
                        if (--utf_left_ == 0) put(utf_cp_);
                        continue;
                    }
                    utf_left_ = 0;
                    put(0xFFFD);
                }
                if (b < 0x20 || b == 0x7F) {
                    control(b);
                } else if (b < 0x80) {
                    put(b);
                } else if ((b & 0xE0) == 0xC0) {
                    utf_cp_ = b & 0x1F;
                    utf_left_ = 1;
                } else if ((b & 0xF0) == 0xE0) {
                    utf_cp_ = b & 0x0F;
                    utf_left_ = 2;
                } else if ((b & 0xF8) == 0xF0) {
                    utf_cp_ = b & 0x07;
                    utf_left_ = 3;
                } else {
                    put(0xFFFD);
                }
                break;

            case St::Esc:
                if (b == '[') {
                    st_ = St::Csi;
                    params_.clear();
                    inter_.clear();
                    priv_ = 0;
                    have_digit_ = false;
                } else if (b == ']') {
                    st_ = St::Osc;
                    osc_.clear();
                } else if (b == 'P' || b == 'X' || b == '^' || b == '_') {
                    st_ = St::Str;
                } else if (b >= 0x20 && b <= 0x2F) {
                    inter_.assign(1, static_cast<char>(b));
                    st_ = St::EscInter;
                } else if (b < 0x20) {
                    control(b);
                } else {
                    st_ = St::Ground;
                    esc_final(b);
                }
                break;

            case St::EscInter:
                if (b >= 0x20 && b <= 0x2F) {
                    inter_ += static_cast<char>(b);
                } else if (b >= 0x30 && b <= 0x7E) {
                    // charset designation: ESC ( 0 = line drawing, ESC ( B = ASCII
                    if (inter_ == "(") g0_graphics_ = b == '0';
                    else if (inter_ == ")") g1_graphics_ = b == '0';
                    st_ = St::Ground;
                } else if (b < 0x20) {
                    control(b);
                } else {
                    st_ = St::Ground;
                }
                break;

            case St::Csi:
                if (b >= '0' && b <= '9') {
                    if (!have_digit_) {
                        params_.push_back(0);
                        have_digit_ = true;
                    }
                    params_.back() = std::min(params_.back() * 10 + (b - '0'), 99999);
                } else if (b == ';' || b == ':') {
                    if (!have_digit_) params_.push_back(-1);
                    have_digit_ = false;
                } else if (b >= 0x3C && b <= 0x3F) {
                    priv_ = static_cast<char>(b);
                } else if (b >= 0x20 && b <= 0x2F) {
                    inter_ += static_cast<char>(b);
                } else if (b >= 0x40 && b <= 0x7E) {
                    st_ = St::Ground;
                    csi_final(b);
                } else if (b < 0x20) {
                    control(b);
                } else {
                    st_ = St::Ground;
                }
                break;

            default:
                st_ = St::Ground;
                break;
        }
    }
}

void Terminal::osc_done() {
    // ESC ] 0 ; title BEL  or  ESC ] 2 ; title BEL
    if (osc_.size() > 2 && (osc_[0] == '0' || osc_[0] == '2') && osc_[1] == ';') {
        title_ = osc_.substr(2);
    }
    osc_.clear();
}

void Terminal::esc_final(uint8_t c) {
    switch (c) {
        case '7':
            saved_ = {cx_, cy_, pen_};
            break;
        case '8':
            cx_ = saved_.x;
            cy_ = saved_.y;
            pen_ = saved_.pen;
            clamp_cursor();
            break;
        case 'D': linefeed(); break;
        case 'E':
            cx_ = 0;
            linefeed();
            break;
        case 'M': reverse_index(); break;
        case 'c': reset(); break;
        default: break;  // '=' '>' keypad modes, 'H' tab set, ...
    }
}

void Terminal::sgr() {
    if (params_.empty()) params_.push_back(0);
    for (size_t i = 0; i < params_.size(); ++i) {
        const int p = std::max(params_[i], 0);
        auto colour = [&](int32_t& out) {
            // 38;5;n  or  38;2;r;g;b
            if (i + 2 < params_.size() + 0 && params_[i + 1] == 5) {
                out = std::clamp(params_[i + 2], 0, 255);
                i += 2;
            } else if (i + 4 < params_.size() + 0 && params_[i + 1] == 2) {
                const int r = std::clamp(params_[i + 2], 0, 255);
                const int g = std::clamp(params_[i + 3], 0, 255);
                const int b = std::clamp(params_[i + 4], 0, 255);
                out = kRgb | (r << 16) | (g << 8) | b;
                i += 4;
            }
        };
        if (p == 0) pen_ = Cell{};
        else if (p == 1) pen_.attr |= kBold;
        else if (p == 2) pen_.attr |= kDim;
        else if (p == 3) pen_.attr |= kItalic;
        else if (p == 4) pen_.attr |= kUnder;
        else if (p == 7) pen_.attr |= kReverse;
        else if (p == 22) pen_.attr &= ~(kBold | kDim);
        else if (p == 23) pen_.attr &= ~kItalic;
        else if (p == 24) pen_.attr &= ~kUnder;
        else if (p == 27) pen_.attr &= ~kReverse;
        else if (p >= 30 && p <= 37) pen_.fg = p - 30;
        else if (p == 38) colour(pen_.fg);
        else if (p == 39) pen_.fg = -1;
        else if (p >= 40 && p <= 47) pen_.bg = p - 40;
        else if (p == 48) colour(pen_.bg);
        else if (p == 49) pen_.bg = -1;
        else if (p >= 90 && p <= 97) pen_.fg = p - 90 + 8;
        else if (p >= 100 && p <= 107) pen_.bg = p - 100 + 8;
    }
}

void Terminal::set_mode(bool on) {
    for (size_t i = 0; i < params_.size(); ++i) {
        const int p = params_[i];
        if (priv_ == '?') {
            switch (p) {
                case 1: app_cursor_ = on; break;
                case 6:
                    origin_ = on;
                    cx_ = 0;
                    cy_ = on ? top_ : 0;
                    break;
                case 7: autowrap_ = on; break;
                case 25:
                    cursor_on_ = on;
                    mark(cy_);
                    break;
                case 47:
                case 1047:
                    if (on) {
                        use_alt(true);
                        for (auto& l : alt_lines_) erase(l, 0, cols_);
                    } else {
                        use_alt(false);
                    }
                    break;
                case 1048:
                    if (on) saved_ = {cx_, cy_, pen_};
                    else {
                        cx_ = saved_.x;
                        cy_ = saved_.y;
                        pen_ = saved_.pen;
                        clamp_cursor();
                    }
                    break;
                case 1049:
                    if (on && !alt_) {
                        saved_alt_ = {cx_, cy_, pen_};
                        use_alt(true);
                        for (auto& l : alt_lines_) erase(l, 0, cols_);
                    } else if (!on && alt_) {
                        use_alt(false);
                        cx_ = saved_alt_.x;
                        cy_ = saved_alt_.y;
                        pen_ = saved_alt_.pen;
                        clamp_cursor();
                    }
                    break;
                default: break;  // 12 blink, 1000+ mouse, 2004 bracketed paste: ignored
            }
        } else if (priv_ == 0 && p == 4) {
            insert_ = on;
        }
    }
}

void Terminal::csi_final(uint8_t c) {
    if (have_digit_ || !params_.empty()) {
        // a trailing ';' means one more empty parameter; already handled above
    }
    const int n = param(0, 1);
    if (!inter_.empty()) {
        if (inter_ == "!" && c == 'p') {  // DECSTR soft reset
            pen_ = Cell{};
            cursor_on_ = true;
            app_cursor_ = insert_ = origin_ = false;
            autowrap_ = true;
            top_ = 0;
            bottom_ = rows_ - 1;
        }
        return;  // cursor style (SP q) and friends
    }
    if (priv_ == '>' || priv_ == '<' || priv_ == '=') {
        if (c == 'c' && priv_ == '>') reply_ += "\x1b[>0;10;1c";
        return;  // xterm key-modifier options etc.
    }
    switch (c) {
        case '@': {
            Line& l = line(cy_);
            const int k = std::min(n, cols_ - cx_);
            for (int x = cols_ - 1; x >= cx_ + k; --x) l[x] = l[x - k];
            erase(l, cx_, cx_ + k);
            mark(cy_);
            break;
        }
        case 'A':
            cy_ = std::max(cy_ - n, cy_ >= top_ ? top_ : 0);
            wrap_pending_ = false;
            break;
        case 'B': case 'e':
            cy_ = std::min(cy_ + n, cy_ <= bottom_ ? bottom_ : rows_ - 1);
            wrap_pending_ = false;
            break;
        case 'C': case 'a':
            cx_ = std::min(cx_ + n, cols_ - 1);
            wrap_pending_ = false;
            break;
        case 'D':
            cx_ = std::max(cx_ - n, 0);
            wrap_pending_ = false;
            break;
        case 'E':
            cx_ = 0;
            cy_ = std::min(cy_ + n, bottom_);
            wrap_pending_ = false;
            break;
        case 'F':
            cx_ = 0;
            cy_ = std::max(cy_ - n, top_);
            wrap_pending_ = false;
            break;
        case 'G': case '`':
            cx_ = n - 1;
            clamp_cursor();
            break;
        case 'd':
            cy_ = n - 1 + (origin_ ? top_ : 0);
            clamp_cursor();
            break;
        case 'H': case 'f':
            cy_ = param(0, 1) - 1 + (origin_ ? top_ : 0);
            cx_ = param(1, 1) - 1;
            clamp_cursor();
            break;
        case 'I':
            for (int i = 0; i < n; ++i) control(0x09);
            break;
        case 'Z':
            cx_ = std::max(0, ((cx_ - 1) / 8) * 8 - (n - 1) * 8);
            wrap_pending_ = false;
            break;
        case 'J': {
            const int mode = param(0, 0);
            if (mode == 0) {
                erase(line(cy_), cx_, cols_);
                for (int y = cy_ + 1; y < rows_; ++y) erase(line(y), 0, cols_);
            } else if (mode == 1) {
                erase(line(cy_), 0, cx_ + 1);
                for (int y = 0; y < cy_; ++y) erase(line(y), 0, cols_);
            } else {
                for (int y = 0; y < rows_; ++y) erase(line(y), 0, cols_);
                if (mode == 3 && !alt_) {
                    history_.clear();
                    view_offset_ = 0;
                }
            }
            mark_all();
            break;
        }
        case 'K': {
            const int mode = param(0, 0);
            if (mode == 0) erase(line(cy_), cx_, cols_);
            else if (mode == 1) erase(line(cy_), 0, cx_ + 1);
            else erase(line(cy_), 0, cols_);
            mark(cy_);
            break;
        }
        case 'L':
            if (cy_ >= top_ && cy_ <= bottom_) scroll_down(cy_, bottom_, n);
            cx_ = 0;
            break;
        case 'M':
            if (cy_ >= top_ && cy_ <= bottom_) scroll_up(cy_, bottom_, n, false);
            cx_ = 0;
            break;
        case 'P': {
            Line& l = line(cy_);
            const int k = std::min(n, cols_ - cx_);
            for (int x = cx_; x < cols_ - k; ++x) l[x] = l[x + k];
            erase(l, cols_ - k, cols_);
            mark(cy_);
            break;
        }
        case 'S': scroll_up(top_, bottom_, n, false); break;
        case 'T': scroll_down(top_, bottom_, n); break;
        case 'X':
            erase(line(cy_), cx_, cx_ + n);
            mark(cy_);
            break;
        case 'b':
            for (int i = 0; i < std::min(n, 1000); ++i) put(last_char_);
            break;
        case 'm':
            if (priv_ == 0) sgr();
            break;
        case 'h': set_mode(true); break;
        case 'l': set_mode(false); break;
        case 'r': {
            const int t = param(0, 1) - 1;
            const int b = param(1, rows_) - 1;
            if (priv_ == 0 && t < b && b < rows_) {
                top_ = t;
                bottom_ = b;
                cx_ = 0;
                cy_ = origin_ ? top_ : 0;
                wrap_pending_ = false;
            }
            break;
        }
        case 's':
            if (priv_ == 0) saved_ = {cx_, cy_, pen_};
            break;
        case 'u':
            if (priv_ == 0) {
                cx_ = saved_.x;
                cy_ = saved_.y;
                pen_ = saved_.pen;
                clamp_cursor();
            }
            break;
        case 'n': {
            const int q = param(0, 0);
            if (q == 5) reply_ += "\x1b[0n";
            else if (q == 6) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "\x1b[%s%d;%dR", priv_ == '?' ? "?" : "", cy_ + 1, cx_ + 1);
                reply_ += buf;
            }
            break;
        }
        case 'c':
            if (priv_ == 0 && param(0, 0) == 0) reply_ += "\x1b[?1;2c";
            break;
        case 't':
            if (param(0, 0) == 18) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "\x1b[8;%d;%dt", rows_, cols_);
                reply_ += buf;
            }
            break;
        default: break;
    }
}

// ---------------------------------------------------------------- input
std::string Terminal::encode_key(uint32_t key) const {
    const bool app = app_cursor_;
    switch (key) {
        case LV_KEY_UP: return app ? "\x1bOA" : "\x1b[A";
        case LV_KEY_DOWN: return app ? "\x1bOB" : "\x1b[B";
        case LV_KEY_RIGHT: return app ? "\x1bOC" : "\x1b[C";
        case LV_KEY_LEFT: return app ? "\x1bOD" : "\x1b[D";
        case LV_KEY_HOME: return app ? "\x1bOH" : "\x1b[H";
        case LV_KEY_END: return app ? "\x1bOF" : "\x1b[F";
        case LV_KEY_ENTER: return "\r";
        case LV_KEY_BACKSPACE: return "\x7f";
        case LV_KEY_ESC: return "\x1b";
        case LV_KEY_NEXT: return "\t";
        case LV_KEY_PREV:
        case kKeyBackTab: return "\x1b[Z";
        case LV_KEY_DEL: return "\x1b[3~";
        case kKeyInsert: return "\x1b[2~";
        case kKeyPgUp: return "\x1b[5~";
        case kKeyPgDn: return "\x1b[6~";
        default: break;
    }
    if (key >= kKeyF1 && key < kKeyF1 + 12) {
        static const char* const kF[] = {"\x1bOP", "\x1bOQ", "\x1bOR", "\x1bOS", "\x1b[15~", "\x1b[17~",
                                         "\x1b[18~", "\x1b[19~", "\x1b[20~", "\x1b[21~", "\x1b[23~", "\x1b[24~"};
        return kF[key - kKeyF1];
    }
    if (is_ctrl_key(key)) {
        uint32_t c = key & 0xFFFF;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c >= 'a' && c <= 'z') return std::string(1, static_cast<char>(c - 'a' + 1));
        switch (c) {
            case '@': case ' ': case '2': return std::string(1, '\0');
            case '[': case '3': return "\x1b";
            case '\\': case '4': return "\x1c";
            case ']': case '5': return "\x1d";
            case '^': case '6': return "\x1e";
            case '_': case '/': case '7': case '-': return "\x1f";
            case '?': case '8': return "\x7f";
            default: return {};
        }
    }
    if (is_alt_key(key)) {
        std::string s = "\x1b";
        utf8_append(s, key & 0xFFFFFF);
        return s;
    }
    if (key >= 0x20 && key < 0x110000 && key != 0x7F) {
        std::string s;
        utf8_append(s, key);
        return s;
    }
    return {};
}

}  // namespace velvet
