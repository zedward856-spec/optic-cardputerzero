// A small VT100 / xterm terminal emulator: turns a byte stream from a shell into a grid of
// cells. Covers what bash, readline, less, nano, top and htop use: cursor movement, erase,
// insert/delete, scroll regions, 16/256/true colour, the alternate screen, DSR/DA replies.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace velvet {

struct Cell {
    uint32_t ch{' '};
    int32_t fg{-1};  // -1 = default, 0..255 = palette, kRgb | 0xRRGGBB = true colour
    int32_t bg{-1};
    uint8_t attr{0};
};

class Terminal {
public:
    static constexpr int32_t kRgb = 0x1000000;
    enum Attr : uint8_t { kBold = 1, kDim = 2, kUnder = 4, kReverse = 8, kItalic = 16 };

    Terminal(int cols, int rows, int scrollback = 400);

    void feed(const char* data, size_t n);
    // New size in cells. Keeps the cursor line and what's above it (pulling lines back out
    // of the scrollback when it grows); long lines are cut, short ones padded.
    void resize(int cols, int rows);
    // Wipes everything above the cursor line and moves that line (the prompt) to the top.
    void clear_above();
    void feed(const std::string& s) { feed(s.data(), s.size()); }
    void reset();

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    // Cell at (x, y) of what is on screen now, taking the scrollback view into account.
    const Cell& view_cell(int x, int y) const;

    int cursor_x() const { return cx_; }
    int cursor_y() const { return cy_; }
    bool cursor_visible() const { return cursor_on_ && view_offset_ == 0; }
    bool alt_screen() const { return alt_; }
    const std::string& title() const { return title_; }

    // Scrollback: lines scrolled back (0 = live). Returns true if the view moved.
    bool scroll_view(int delta);
    int view_offset() const { return view_offset_; }
    int history_size() const { return static_cast<int>(history_.size()); }

    // Bytes the terminal must send back to the program (answers to DSR / DA queries).
    std::string take_reply();
    // Bitmask of rows that changed since the last call (bit y = row y).
    uint64_t take_dirty();
    void mark_row(int y) { mark(y); }

    // Encodes a Velvet/LVGL key code as the bytes a terminal sends; "" if it has none.
    std::string encode_key(uint32_t key) const;

private:
    using Line = std::vector<Cell>;

    void put(uint32_t cp);
    void control(uint8_t c);
    void esc_final(uint8_t c);
    void csi_final(uint8_t c);
    void sgr();
    void osc_done();
    void set_mode(bool on);
    void linefeed();
    void reverse_index();
    void scroll_up(int top, int bottom, int n, bool to_history);
    void scroll_down(int top, int bottom, int n);
    void erase(Line& line, int from, int to);
    Cell blank() const;
    void clamp_cursor();
    void mark(int y) { if (y >= 0 && y < 64) dirty_ |= 1ull << y; }
    void mark_all() { dirty_ = ~0ull; }
    void use_alt(bool on);
    int param(size_t i, int def) const;
    Line& line(int y) { return (*screen_)[y]; }

    int cols_, rows_, scrollback_;
    std::vector<Line> main_, alt_lines_;
    std::vector<Line>* screen_;
    std::deque<Line> history_;
    bool alt_{false};

    int cx_{0}, cy_{0};
    bool wrap_pending_{false};
    int top_{0}, bottom_{0};  // scroll region, inclusive
    Cell pen_{};
    struct Saved {
        int x{0}, y{0};
        Cell pen{};
    } saved_, saved_alt_;

    bool cursor_on_{true};
    bool app_cursor_{false};
    bool autowrap_{true};
    bool insert_{false};
    bool origin_{false};
    int view_offset_{0};
    uint64_t dirty_{~0ull};
    std::string title_;
    std::string reply_;
    uint32_t last_char_{' '};

    // parser
    enum class St { Ground, Esc, EscInter, Csi, Osc, OscEsc, Str, StrEsc } st_{St::Ground};
    std::vector<int> params_;
    std::string inter_;
    char priv_{0};
    bool have_digit_{false};
    bool g0_graphics_{false}, g1_graphics_{false}, shift_out_{false};
    std::string osc_;
    uint32_t utf_cp_{0};
    int utf_left_{0};
};

}  // namespace velvet
