#include "ir.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>

namespace velvet {
namespace {

// ================================================================ protocols
// Timings and frame layouts are the ones Flipper Zero's firmware uses, read from
// flipperdevices/flipperzero-firmware, lib/infrared/encoder_decoder/:
//   nec/infrared_protocol_nec_i.h, nec/infrared_encoder_nec.c, nec/infrared_decoder_nec.c
//   samsung/infrared_protocol_samsung_i.h, samsung/infrared_encoder_samsung.c
//   sirc/infrared_protocol_sirc_i.h, sirc/infrared_encoder_sirc.c
//   rc5/infrared_protocol_rc5_i.h, rc5/infrared_encoder_rc5.c, rc5/infrared_decoder_rc5.c
//   rc6/infrared_protocol_rc6_i.h, rc6/infrared_encoder_rc6.c, rc6/infrared_decoder_rc6.c
//   kaseikyo/infrared_protocol_kaseikyo_i.h, kaseikyo/infrared_encoder_kaseikyo.c
//   rca/infrared_protocol_rca_i.h, rca/infrared_encoder_rca.c
//   pioneer/infrared_protocol_pioneer_i.h, pioneer/infrared_encoder_pioneer.c
// Only the numbers and the bit layouts come from there (so a .ir file means the same thing
// here as on a Flipper); the code below is written for this app.

// Pulse-distance protocols: a long start mark and space, then one short mark per bit followed
// by a short (0) or long (1) space, least significant bit first, and a closing mark.
struct Pdm {
    uint32_t pre_mark, pre_space, bit_mark, space0, space1;
};
constexpr Pdm kNec{9000, 4500, 560, 560, 1690};
constexpr Pdm kSamsung{4500, 4500, 550, 550, 1650};
constexpr Pdm kKaseikyo{8 * 432, 4 * 432, 432, 432, 3 * 432};
constexpr Pdm kRca{4000, 4000, 500, 1000, 2000};
constexpr Pdm kPioneer{8500, 4225, 500, 500, 1500};
constexpr uint32_t kPioneerGap = 500 + 26000;   // the 33rd (zero) bit's space, then the silence
constexpr uint32_t kSircPreMark = 2400, kSircUnit = 600, kSircPeriod = 45000;
constexpr int kSircFrames = 3;                  // a Sony set wants the frame three times
constexpr uint32_t kRc5Half = 889;              // half a bit
constexpr uint32_t kRc6PreMark = 2666, kRc6PreSpace = 889, kRc6Half = 444;

// Collects marks and spaces; neighbours of the same kind merge, a leading space is dropped.
struct Wave {
    std::vector<uint32_t> v;
    bool last_mark{false};
    void add(bool mark, uint32_t us) {
        if (!us) return;
        if (v.empty()) {
            if (!mark) return;
            v.push_back(us);
        } else if (mark == last_mark) {
            v.back() += us;
            return;
        } else {
            v.push_back(us);
        }
        last_mark = mark;
    }
    void mark(uint32_t us) { add(true, us); }
    void space(uint32_t us) { add(false, us); }
    uint32_t total() const {
        uint32_t s = 0;
        for (uint32_t d : v) s += d;
        return s;
    }
    std::vector<uint32_t> done() {
        if (!v.empty() && !last_mark) v.pop_back();   // what is sent ends with a mark
        return v;
    }
};

void pdm_frame(Wave& w, const Pdm& p, uint64_t bits, int n) {
    w.mark(p.pre_mark);
    w.space(p.pre_space);
    for (int i = 0; i < n; ++i) {
        w.mark(p.bit_mark);
        w.space((bits >> i) & 1 ? p.space1 : p.space0);
    }
    w.mark(p.bit_mark);
}

// A received mark is often a little long and the space after it as much too short.
bool near(uint32_t v, uint32_t ref, uint32_t min_tol = 150) {
    const uint32_t tol = std::max(ref / 4, min_tol);
    return v + tol >= ref && v <= ref + tol;
}

// Reads the first frame of a pulse-distance signal. False if it does not fit `p`.
bool pdm_decode(const std::vector<uint32_t>& t, const Pdm& p, uint64_t& bits, int& n) {
    bits = 0;
    n = 0;
    if (t.size() < 4 || !near(t[0], p.pre_mark) || !near(t[1], p.pre_space)) return false;
    for (size_t i = 2;; i += 2) {
        if (i >= t.size() || !near(t[i], p.bit_mark, 220)) return false;
        if (i + 1 >= t.size() || t[i + 1] > p.space1 * 2 + 1000) break;   // that was the closing mark
        const uint32_t s = t[i + 1];
        const bool one = (s > p.space1 ? s - p.space1 : p.space1 - s) < (s > p.space0 ? s - p.space0 : p.space0 - s);
        if (!near(s, one ? p.space1 : p.space0) || n >= 64) return false;
        if (one) bits |= uint64_t{1} << n;
        ++n;
    }
    return n > 0;
}

uint8_t byte_of(uint64_t bits, int i) { return static_cast<uint8_t>(bits >> (8 * i)); }

uint64_t kaseikyo_bits(uint32_t address, uint32_t command) {
    // address: id (2 bits) << 24 | vendor (16) << 8 | genre1 (4) << 4 | genre2 (4); command: 10 bits
    const uint8_t id = (address >> 24) & 3, genre1 = (address >> 4) & 0xF, genre2 = address & 0xF;
    const uint16_t vendor = (address >> 8) & 0xFFFF;
    uint8_t d[6];
    d[0] = static_cast<uint8_t>(vendor & 0xFF);
    d[1] = static_cast<uint8_t>(vendor >> 8);
    uint8_t vp = d[0] ^ d[1];
    vp = (vp & 0xF) ^ (vp >> 4);
    d[2] = static_cast<uint8_t>((vp & 0xF) | (genre1 << 4));
    d[3] = static_cast<uint8_t>((genre2 & 0xF) | ((command & 0xF) << 4));
    d[4] = static_cast<uint8_t>((id << 6) | ((command >> 4) & 0x3F));
    d[5] = d[2] ^ d[3] ^ d[4];
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t{d[i]} << (8 * i);
    return bits;
}

// Manchester helpers: the signal as a row of half-bit levels (1 = mark).
// Each timing from t[start] on must be 1..max_k halves long; a longer space ends the frame.
bool to_halves(const std::vector<uint32_t>& t, size_t start, uint32_t half, uint32_t max_k, std::vector<uint8_t>& lv) {
    for (size_t i = start; i < t.size(); ++i) {
        const bool mark = (i % 2) == 0;
        const uint32_t d = t[i];
        if (!mark && d > half * (max_k + 1)) break;
        const uint32_t k = (d + half / 2) / half;
        const uint32_t fit = k * half;
        if (k < 1 || k > max_k || (d > fit ? d - fit : fit - d) > half * 2 / 5) return false;
        for (uint32_t j = 0; j < k; ++j) lv.push_back(mark ? 1 : 0);
    }
    return true;
}

bool decode_sirc(const std::vector<uint32_t>& t, IrSignal& out) {
    if (t.size() < 4 || !near(t[0], kSircPreMark) || !near(t[1], kSircUnit)) return false;
    uint32_t bits = 0;
    int n = 0;
    for (size_t i = 2; i < t.size(); i += 2) {
        const bool one = near(t[i], 2 * kSircUnit);
        if (!one && !near(t[i], kSircUnit)) return false;
        if (n >= 20) return false;
        if (one) bits |= 1u << n;
        ++n;
        if (i + 1 >= t.size() || t[i + 1] > 2000) break;   // the gap before the frame comes again
        if (!near(t[i + 1], kSircUnit)) return false;
    }
    if (n != 12 && n != 15 && n != 20) return false;
    out.protocol = n == 12 ? "SIRC" : n == 15 ? "SIRC15" : "SIRC20";
    out.command = bits & 0x7F;
    out.address = bits >> 7;
    return true;
}

bool decode_rc5(const std::vector<uint32_t>& t, IrSignal& out) {
    // 14 bits: start (1), field (1: RC5, 0: RC5X), toggle, 5 address, 6 command, each most
    // significant first. A one is space-then-mark, so the first start bit shows only its mark.
    std::vector<uint8_t> lv{0};
    if (!to_halves(t, 0, kRc5Half, 2, lv) || lv.size() > 28 || lv.size() < 26) return false;
    lv.resize(28, 0);
    uint32_t bits = 0;   // bit 13 = first sent
    for (int i = 0; i < 14; ++i) {
        if (lv[2 * i] == lv[2 * i + 1]) return false;
        bits = (bits << 1) | (lv[2 * i + 1] ? 1u : 0u);
    }
    if (!(bits & 0x2000)) return false;
    out.protocol = (bits & 0x1000) ? "RC5" : "RC5X";
    out.address = (bits >> 6) & 0x1F;
    out.command = bits & 0x3F;
    return true;
}

bool decode_rc6(const std::vector<uint32_t>& t, IrSignal& out) {
    // Mode 0: leader, start bit (1), three mode bits (000), a double-width toggle bit, 8 address
    // and 8 command bits, most significant first. A one is mark-then-space.
    if (t.size() < 8 || !near(t[0], kRc6PreMark) || !near(t[1], kRc6PreSpace)) return false;
    std::vector<uint8_t> lv;
    if (!to_halves(t, 2, kRc6Half, 3, lv) || lv.size() > 44 || lv.size() < 40) return false;
    lv.resize(44, 0);
    if (!(lv[0] == 1 && lv[1] == 0)) return false;
    for (int i = 1; i <= 3; ++i)
        if (!(lv[2 * i] == 0 && lv[2 * i + 1] == 1)) return false;
    if (!(lv[8] == lv[9] && lv[10] == lv[11] && lv[8] != lv[10])) return false;
    uint32_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        const uint8_t a = lv[12 + 2 * i], b = lv[13 + 2 * i];
        if (a == b) return false;
        bits = (bits << 1) | (a ? 1u : 0u);
    }
    out.protocol = "RC6";
    out.address = bits >> 8;
    out.command = bits & 0xFF;
    return true;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// "04 00 00 00": up to four bytes, the least significant first.
uint32_t parse_hex_le(const std::string& s) {
    uint32_t v = 0;
    int n = 0;
    std::istringstream in(s);
    std::string b;
    while (n < 4 && in >> b) {
        v |= static_cast<uint32_t>(std::strtoul(b.c_str(), nullptr, 16) & 0xFF) << (8 * n);
        ++n;
    }
    return v;
}

std::string hex_le(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02X %02X %02X %02X", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF);
    return buf;
}

// One real file from the online database (Flipper-IRDB, TVs/Sony/SonyTV.ir), shortened to
// seven of its thirteen buttons: what the self-test parses and what the demo "downloads".
const char kSonyTvFile[] =
    "Filetype: IR signals file\nVersion: 1\n"
    "# \nname: On_off\ntype: parsed\nprotocol: SIRC\naddress: 01 00 00 00\ncommand: 15 00 00 00\n"
    "# \nname: Netflix\ntype: parsed\nprotocol: SIRC15\naddress: 1A 00 00 00\ncommand: 7C 00 00 00\n"
    "# \nname: Up\ntype: parsed\nprotocol: SIRC\naddress: 01 00 00 00\ncommand: 74 00 00 00\n"
    "# \nname: Down\ntype: parsed\nprotocol: SIRC\naddress: 01 00 00 00\ncommand: 75 00 00 00\n"
    "# \nname: Ok\ntype: parsed\nprotocol: SIRC\naddress: 01 00 00 00\ncommand: 65 00 00 00\n"
    "# \nname: Back\ntype: parsed\nprotocol: SIRC15\naddress: 97 00 00 00\ncommand: 23 00 00 00\n"
    "# \nname: Vol_up\ntype: parsed\nprotocol: SIRC\naddress: 01 00 00 00\ncommand: 12 00 00 00\n";

}  // namespace

// ================================================================ encode / decode
bool ir_can_send(const std::string& p) {
    for (const char* k : {"NEC", "NECext", "NEC42", "NEC42ext", "Samsung32", "SIRC", "SIRC15", "SIRC20", "RC5", "RC5X",
                          "RC6", "Kaseikyo", "RCA", "Pioneer"})
        if (p == k) return true;
    return false;
}

bool ir_encode(const IrSignal& s, std::vector<uint32_t>& out, uint32_t& carrier_hz, float& duty, std::string* why,
               bool toggle) {
    out.clear();
    carrier_hz = 38000;
    duty = 0.33f;
    Wave w;
    if (s.raw) {
        carrier_hz = s.frequency >= 20000 && s.frequency <= 500000 ? s.frequency : 38000;
        duty = s.duty > 0.01f && s.duty < 0.99f ? s.duty : 0.33f;
        for (size_t i = 0; i < s.data.size(); ++i) w.add(i % 2 == 0, s.data[i]);
        out = w.done();
        if (out.empty() && why) *why = "Nothing in this button to send";
        return !out.empty();
    }
    const std::string& p = s.protocol;
    const uint32_t a = s.address, c = s.command;
    if (p == "NEC") {
        pdm_frame(w, kNec, (a & 0xFF) | ((~a & 0xFF) << 8) | ((c & 0xFF) << 16) | ((~c & 0xFFu) << 24), 32);
    } else if (p == "NECext") {
        pdm_frame(w, kNec, (a & 0xFFFF) | ((c & 0xFFFF) << 16), 32);
    } else if (p == "NEC42") {   // 13 address bits, their inverse, 8 command bits, their inverse
        pdm_frame(w, kNec,
                  uint64_t{a & 0x1FFF} | (uint64_t{~a & 0x1FFF} << 13) | (uint64_t{c & 0xFF} << 26) | (uint64_t{~c & 0xFF} << 34), 42);
    } else if (p == "NEC42ext") {
        pdm_frame(w, kNec, uint64_t{a & 0x3FFFFFF} | (uint64_t{c & 0xFFFF} << 26), 42);
    } else if (p == "Samsung32") {
        pdm_frame(w, kSamsung, (a & 0xFF) | ((a & 0xFF) << 8) | ((c & 0xFF) << 16) | ((~c & 0xFFu) << 24), 32);
    } else if (p == "Kaseikyo") {
        pdm_frame(w, kKaseikyo, kaseikyo_bits(a, c), 48);
    } else if (p == "RCA") {     // 4 address bits, 8 command bits, then both inverted
        pdm_frame(w, kRca, (a & 0xF) | ((c & 0xFF) << 4) | ((~a & 0xF) << 12) | ((~c & 0xFFu) << 16), 24);
    } else if (p == "Pioneer") { // the frame twice, 40 kHz
        carrier_hz = 40000;
        const uint64_t bits = (a & 0xFF) | ((~a & 0xFF) << 8) | ((c & 0xFF) << 16) | ((~c & 0xFFu) << 24);
        pdm_frame(w, kPioneer, bits, 32);
        w.space(kPioneerGap);
        pdm_frame(w, kPioneer, bits, 32);
    } else if (p == "SIRC" || p == "SIRC15" || p == "SIRC20") {
        carrier_hz = 40000;
        const int n = p == "SIRC" ? 12 : p == "SIRC15" ? 15 : 20;
        const uint32_t bits = (c & 0x7F) | ((a & (n == 12 ? 0x1Fu : n == 15 ? 0xFFu : 0x1FFFu)) << 7);
        for (int f = 0; f < kSircFrames; ++f) {
            const uint32_t before = w.total();
            w.mark(kSircPreMark);
            w.space(kSircUnit);
            for (int i = 0; i < n; ++i) {
                w.mark((bits >> i) & 1 ? 2 * kSircUnit : kSircUnit);
                if (i + 1 < n) w.space(kSircUnit);
            }
            if (f + 1 < kSircFrames) w.space(kSircPeriod - (w.total() - before));
        }
    } else if (p == "RC5" || p == "RC5X") {
        carrier_hz = 36000;
        const uint32_t bits = (1u << 13) | (p == "RC5" ? 1u << 12 : 0u) | (toggle ? 1u << 11 : 0u) | ((a & 0x1F) << 6) | (c & 0x3F);
        for (int i = 13; i >= 0; --i) {
            const bool one = (bits >> i) & 1;
            w.add(!one, kRc5Half);
            w.add(one, kRc5Half);
        }
    } else if (p == "RC6") {
        carrier_hz = 36000;
        w.mark(kRc6PreMark);
        w.space(kRc6PreSpace);
        auto bit = [&](bool one, uint32_t half) {
            w.add(one, half);
            w.add(!one, half);
        };
        bit(true, kRc6Half);                                     // start
        for (int i = 0; i < 3; ++i) bit(false, kRc6Half);        // mode 0
        bit(toggle, 2 * kRc6Half);
        for (int i = 7; i >= 0; --i) bit((a >> i) & 1, kRc6Half);
        for (int i = 7; i >= 0; --i) bit((c >> i) & 1, kRc6Half);
    } else {
        if (why) *why = "Can't send this one (protocol " + (p.empty() ? std::string("?") : p.substr(0, 8)) + ")";
        return false;
    }
    out = w.done();
    return true;
}

bool ir_decode(const std::vector<uint32_t>& t, IrSignal& out) {
    uint64_t bits = 0;
    int n = 0;
    if (pdm_decode(t, kNec, bits, n)) {
        if (n == 32) {
            const uint8_t a = byte_of(bits, 0), ai = byte_of(bits, 1), c = byte_of(bits, 2), ci = byte_of(bits, 3);
            if (a == static_cast<uint8_t>(~ai) && c == static_cast<uint8_t>(~ci)) {
                out.protocol = "NEC";
                out.address = a;
                out.command = c;
            } else {
                out.protocol = "NECext";
                out.address = a | (uint32_t{ai} << 8);
                out.command = c | (uint32_t{ci} << 8);
            }
            return true;
        }
        if (n == 42) {
            const uint32_t a = bits & 0x1FFF, ai = (bits >> 13) & 0x1FFF, c = (bits >> 26) & 0xFF, ci = (bits >> 34) & 0xFF;
            if (a == (~ai & 0x1FFF) && c == (~ci & 0xFF)) {
                out.protocol = "NEC42";
                out.address = a;
                out.command = c;
            } else {
                out.protocol = "NEC42ext";
                out.address = a | (ai << 13);
                out.command = c | (ci << 8);
            }
            return true;
        }
    }
    if (pdm_decode(t, kSamsung, bits, n) && n == 32 && byte_of(bits, 0) == byte_of(bits, 1) &&
        byte_of(bits, 2) == static_cast<uint8_t>(~byte_of(bits, 3))) {
        out.protocol = "Samsung32";
        out.address = byte_of(bits, 0);
        out.command = byte_of(bits, 2);
        return true;
    }
    if (pdm_decode(t, kKaseikyo, bits, n) && n == 48) {
        const uint8_t d0 = byte_of(bits, 0), d1 = byte_of(bits, 1), d2 = byte_of(bits, 2), d3 = byte_of(bits, 3),
                      d4 = byte_of(bits, 4), d5 = byte_of(bits, 5);
        uint8_t vp = d0 ^ d1;
        vp = (vp & 0xF) ^ (vp >> 4);
        if (d5 == (d2 ^ d3 ^ d4) && (d2 & 0xF) == vp) {
            out.protocol = "Kaseikyo";
            out.address = (uint32_t{d4} >> 6 << 24) | (uint32_t{d1} << 16) | (uint32_t{d0} << 8) | (uint32_t{d2} >> 4 << 4) | (d3 & 0xFu);
            out.command = (uint32_t{d3} >> 4) | ((d4 & 0x3Fu) << 4);
            return true;
        }
    }
    if (pdm_decode(t, kRca, bits, n) && n == 24) {
        const uint32_t a = bits & 0xF, c = (bits >> 4) & 0xFF, ai = (bits >> 12) & 0xF, ci = (bits >> 16) & 0xFF;
        if (a == (~ai & 0xF) && c == (~ci & 0xFF)) {
            out.protocol = "RCA";
            out.address = a;
            out.command = c;
            return true;
        }
    }
    return decode_sirc(t, out) || decode_rc6(t, out) || decode_rc5(t, out);
}

std::string ir_describe(const IrSignal& s) {
    char buf[64];
    if (s.raw) std::snprintf(buf, sizeof buf, "raw, %zu pulses", (s.data.size() + 1) / 2);
    else std::snprintf(buf, sizeof buf, "%.10s %02X %02X", s.protocol.c_str(), s.address, s.command);
    return buf;
}

// ================================================================ the .ir file format
std::string ir_file_write(const std::vector<IrSignal>& signals, bool library) {
    std::string out = library ? "Filetype: IR library file\nVersion: 1\n" : "Filetype: IR signals file\nVersion: 1\n";
    char buf[64];
    for (const IrSignal& s : signals) {
        out += "#\nname: " + s.name + "\n";
        if (s.raw) {
            std::snprintf(buf, sizeof buf, "type: raw\nfrequency: %u\nduty_cycle: %.6f\ndata:", s.frequency,
                          static_cast<double>(s.duty));
            out += buf;
            for (uint32_t d : s.data) {
                std::snprintf(buf, sizeof buf, " %u", d);
                out += buf;
            }
            out += "\n";
        } else {
            out += "type: parsed\nprotocol: " + s.protocol + "\naddress: " + hex_le(s.address) + "\ncommand: " +
                   hex_le(s.command) + "\n";
        }
    }
    return out;
}

bool ir_file_parse(const std::string& text, std::vector<IrSignal>& out) {
    out.clear();
    bool header = false, have = false, typed = false;
    IrSignal cur;
    auto flush = [&] {
        if (have && typed && !cur.name.empty() && (cur.raw ? !cur.data.empty() : !cur.protocol.empty())) out.push_back(cur);
        cur = IrSignal{};
        have = typed = false;
    };
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = trim(line.substr(0, colon)), val = trim(line.substr(colon + 1));
        if (key == "Filetype") header = val.compare(0, 2, "IR") == 0;
        else if (key == "name") { flush(); cur.name = val.substr(0, 40); have = true; }
        else if (!have) continue;
        else if (key == "type") { cur.raw = val == "raw"; typed = cur.raw || val == "parsed"; }
        else if (key == "protocol") cur.protocol = val.substr(0, 24);
        else if (key == "address") cur.address = parse_hex_le(val);
        else if (key == "command") cur.command = parse_hex_le(val);
        else if (key == "frequency") cur.frequency = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
        else if (key == "duty_cycle") cur.duty = static_cast<float>(std::strtod(val.c_str(), nullptr));
        else if (key == "data") {
            const char* p = val.c_str();
            char* e = nullptr;
            while (cur.data.size() < 8192) {
                const unsigned long d = std::strtoul(p, &e, 10);
                if (e == p) break;
                cur.data.push_back(static_cast<uint32_t>(std::min<unsigned long>(d, 0x00FFFFFF)));
                p = e;
            }
        }
    }
    flush();
    return header;
}

std::string ir_safe_name(const std::string& s) {
    std::string r;
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == ' ' || c == '_' || c == '-' || (c == '.' && !r.empty())) r += ch;
    }
    r = trim(r);
    if (r.size() > 28) r = trim(r.substr(0, 28));
    while (!r.empty() && r.back() == '.') r.pop_back();
    return r;
}

std::vector<std::vector<uint32_t>> ir_split_for_lirc(const std::vector<uint32_t>& t, std::vector<uint32_t>& gaps,
                                                     uint32_t max_us, size_t max_count) {
    std::vector<std::vector<uint32_t>> pieces;
    gaps.clear();
    const size_t n = t.size();
    size_t start = 0;
    while (start < n) {
        uint64_t sum = 0;
        size_t i = start;
        for (; i < n; ++i) {
            if (sum + t[i] > max_us || i - start + 1 > max_count) break;
            sum += t[i];
        }
        if (i >= n) {
            std::vector<uint32_t> piece(t.begin() + static_cast<std::ptrdiff_t>(start), t.end());
            if (piece.size() % 2 == 0) piece.pop_back();   // never ends with a space
            pieces.push_back(piece);
            gaps.push_back(0);
            break;
        }
        size_t cut = 0;   // the longest space up to where it stopped fitting
        for (size_t j = start + 1; j <= i; j += 2)
            if (!cut || t[j] >= t[cut]) cut = j;
        if (!cut) {       // one pulse longer than a whole write may be: clipped
            pieces.push_back({std::min(t[start], max_us)});
            gaps.push_back(start + 1 < n ? t[start + 1] : 0);
            start += 2;
            continue;
        }
        pieces.emplace_back(t.begin() + static_cast<std::ptrdiff_t>(start), t.begin() + static_cast<std::ptrdiff_t>(cut));
        gaps.push_back(t[cut]);
        start = cut + 1;
    }
    return pieces;
}

// ================================================================ receiving
namespace {
// The kernel's MODE2 sample layout (linux/lirc.h; checked against that header further down).
constexpr uint32_t kMode2Space = 0x00000000, kMode2Pulse = 0x01000000, kMode2Frequency = 0x02000000,
                   kMode2Timeout = 0x03000000, kMode2Overflow = 0x04000000, kMode2Mask = 0xFF000000,
                   kMode2Value = 0x00FFFFFF;
}  // namespace

bool IrCapture::end_burst() {
    if (!t_.empty() && t_.size() % 2 == 0) t_.pop_back();   // it ends with a pulse
    if (t_.size() >= kMinTimings) done_ = true;
    else t_.clear();
    return done_;
}

bool IrCapture::quiet() { return done_ || (!t_.empty() && end_burst()); }

bool IrCapture::feed(uint32_t sample) {
    if (done_) return true;
    const uint32_t mode = sample & kMode2Mask, us = sample & kMode2Value;
    if (mode == kMode2Pulse) {
        if (t_.size() % 2 == 1) t_.back() += us;   // two pulses in a row: one pulse
        else t_.push_back(us);
    } else if (mode == kMode2Space) {
        if (t_.empty()) return false;              // the silence before the button
        if (us >= kGapUs) return end_burst();
        if (t_.size() % 2 == 0) t_.back() += us;
        else t_.push_back(us);
    } else if (mode == kMode2Timeout) {
        if (!t_.empty()) return end_burst();
    } else if (mode == kMode2Overflow) {
        t_.clear();                                // the kernel lost samples: start over
    } else if (mode != kMode2Frequency) {
        return false;                              // a kind of sample not known here
    }
    if (t_.size() >= kMaxTimings) return end_burst();
    return false;
}

// ================================================================ self-test (desktop)
#if defined(OPTIC_IR_SELFTEST) || (defined(USE_DESKTOP) && USE_DESKTOP)
int ir_self_test() {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const std::string& what) {
        std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
        ok ? ++pass : ++fail;
    };
    auto parsed = [](const char* proto, uint32_t a, uint32_t c) {
        IrSignal s;
        s.protocol = proto;
        s.address = a;
        s.command = c;
        return s;
    };
    // encode -> decode gives the same protocol, address and command; also with timings bent the
    // way a receiver bends them (marks longer, spaces shorter)
    auto round_trip = [&](const char* proto, uint32_t a, uint32_t c, bool toggle = false) {
        const IrSignal s = parsed(proto, a, c);
        std::vector<uint32_t> t;
        uint32_t hz = 0;
        float duty = 0;
        std::string why;
        char what[96];
        std::snprintf(what, sizeof what, "%s %X/%X%s", proto, a, c, toggle ? " (toggle)" : "");
        if (!ir_encode(s, t, hz, duty, &why, toggle)) { check(false, std::string(what) + ": encode: " + why); return; }
        IrSignal d;
        const bool ok = ir_decode(t, d) && d.protocol == proto && d.address == a && d.command == c;
        check(ok, std::string(what) + ": encode -> decode" + (ok ? "" : " gave " + ir_describe(d)));
        std::vector<uint32_t> bent = t;
        for (size_t i = 0; i < bent.size(); ++i) bent[i] = i % 2 == 0 ? bent[i] + 90 : bent[i] - 90;
        IrSignal e;
        const bool ok2 = ir_decode(bent, e) && e.protocol == proto && e.address == a && e.command == c;
        check(ok2, std::string(what) + ": decode with marks +90 us, spaces -90 us");
        check(t.size() % 2 == 1 && std::find(t.begin(), t.end(), 0u) == t.end(), std::string(what) + ": odd count, no zero");
    };
    std::printf("[ir self-test] round trips\n");
    round_trip("NEC", 0x04, 0x08);
    round_trip("NEC", 0xFF, 0x00);
    round_trip("NECext", 0x7F00, 0x6A95);
    round_trip("NEC42", 0x1A5, 0x3C);
    round_trip("NEC42ext", 0x2ABCDEF, 0x1234);
    round_trip("Samsung32", 0x07, 0x02);
    round_trip("Samsung32", 0xE0, 0xF1);
    round_trip("SIRC", 0x01, 0x15);
    round_trip("SIRC15", 0x1A, 0x7C);
    round_trip("SIRC20", 0x103A, 0x2B);
    round_trip("RC5", 0x00, 0x01);
    round_trip("RC5", 0x1F, 0x3F, true);
    round_trip("RC5", 0x05, 0x2A, true);
    round_trip("RC5X", 0x13, 0x0C);
    round_trip("RC6", 0x00, 0x0C);
    round_trip("RC6", 0x04, 0xA5, true);
    round_trip("RC6", 0xFF, 0xFF);
    round_trip("RC6", 0x00, 0x00, true);
    round_trip("Kaseikyo", 0x02200280, 0x13D);
    round_trip("RCA", 0x0F, 0x54);

    std::printf("[ir self-test] known waveforms\n");
    std::vector<uint32_t> t;
    uint32_t hz = 0;
    float duty = 0;
    {   // NEC 04/08: the address byte 0x04 goes out least significant bit first
        ir_encode(parsed("NEC", 0x04, 0x08), t, hz, duty);
        const std::vector<uint32_t> head{9000, 4500, 560, 560, 560, 560, 560, 1690, 560, 560};
        check(t.size() == 67 && std::equal(head.begin(), head.end(), t.begin()) && hz == 38000, "NEC 04/08: 67 timings, 38 kHz, starts 9000 4500 0 0 1 0");
        uint32_t sum = 0;
        for (uint32_t d : t) sum += d;
        check(sum == 9000 + 4500 + 32 * 560 + 16 * 560 + 16 * 1690 + 560, "NEC 04/08: lasts 67.98 ms");
    }
    {   // RC5 address 0, command 1, toggle 0, worked out by hand from the RC5 bit rules
        ir_encode(parsed("RC5", 0, 1), t, hz, duty);
        std::vector<uint32_t> want{889, 889, 1778};
        for (int i = 0; i < 20; ++i) want.push_back(889);
        want.push_back(1778);
        want.push_back(889);
        check(t == want && hz == 36000, "RC5 00/01: the hand-made waveform, 36 kHz");
    }
    {   // RC6 address 0, command 0, toggle 0: leader, start, then the double-width toggle
        ir_encode(parsed("RC6", 0, 0), t, hz, duty);
        const std::vector<uint32_t> head{2666, 889, 444, 888, 444, 444, 444, 444, 444, 888, 888, 444};
        check(t.size() >= head.size() && std::equal(head.begin(), head.end(), t.begin()) && hz == 36000, "RC6 00/00: leader, start, mode, wide toggle");
    }
    {   // SIRC: three frames 45 ms apart, 40 kHz
        ir_encode(parsed("SIRC", 0x01, 0x15), t, hz, duty);
        uint32_t first = 0;
        for (size_t i = 0; i < 26; ++i) first += t[i];
        check(t.size() == 3 * 26 - 1 && first == 45000 && hz == 40000 && t[0] == 2400 && t[2] == 1200, "SIRC 01/15: 3 frames, 45 ms period, 40 kHz");
    }
    {   // Pioneer: the frame twice
        ir_encode(parsed("Pioneer", 0xA5, 0x1C), t, hz, duty);
        check(t.size() == 2 * 67 + 1 && t[67] == 26500 && hz == 40000, "Pioneer A5/1C: 2 frames, 26.5 ms apart, 40 kHz");
    }
    {   // codes that are widely published, to check the bit order against something other than
        // this file's own decoder: Samsung TV power E0E040BF, Panasonic TV power 40040100BCBD
        // (both written most significant bit first), Sony TV power A90
        uint64_t bits = 0;
        int n = 0;
        auto msb_first = [](uint64_t v, int count) {
            uint64_t r = 0;
            for (int i = 0; i < count; ++i) r = (r << 1) | ((v >> i) & 1);
            return r;
        };
        ir_encode(parsed("Samsung32", 0x07, 0x02), t, hz, duty);
        check(pdm_decode(t, kSamsung, bits, n) && n == 32 && msb_first(bits, 32) == 0xE0E040BFull, "Samsung32 07/02 is the well-known E0E040BF");
        ir_encode(parsed("Kaseikyo", 0x00200280, 0x3D0), t, hz, duty);
        check(pdm_decode(t, kKaseikyo, bits, n) && n == 48 && msb_first(bits, 48) == 0x40040100BCBDull, "Kaseikyo 200280/3D0 is Panasonic's 40040100BCBD");
        ir_encode(parsed("SIRC", 0x01, 0x15), t, hz, duty);
        uint32_t sony = 0;
        for (size_t i = 2; i < 25; i += 2) sony = (sony << 1) | (t[i] == 1200 ? 1u : 0u);
        check(sony == 0xA90, "SIRC 01/15 is Sony's A90");
    }
    {
        std::string why;
        const bool refused = !ir_encode(parsed("Denon", 1, 2), t, hz, duty, &why);
        check(refused && why == "Can't send this one (protocol Denon)", "unknown protocol: \"" + why + "\"");
        check(ir_can_send("Kaseikyo") && !ir_can_send("Denon"), "ir_can_send");
        IrSignal junk;
        check(!ir_decode({9000, 2250, 560}, junk) && !ir_decode({300, 400, 300, 5000, 800, 120, 90, 2000, 1000}, junk), "noise and an NEC repeat mark do not decode");
    }

    std::printf("[ir self-test] .ir files\n");
    {
        std::vector<IrSignal> in;
        in.push_back(parsed("NEC", 0x04, 0x08));
        in.back().name = "Power";
        in.push_back(parsed("Kaseikyo", 0x02200280, 0x13D));
        in.back().name = "Vol up";
        IrSignal raw;
        raw.name = "Swing";
        raw.raw = true;
        raw.frequency = 36000;
        raw.duty = 0.25f;
        raw.data = {3400, 1700, 420, 1280, 420, 430, 420, 12000, 3400};
        in.push_back(raw);
        const std::string text = ir_file_write(in);
        std::vector<IrSignal> back;
        const bool ok = ir_file_parse(text, back) && back.size() == 3;
        check(ok, "write -> read: 3 signals");
        if (ok) {
            check(back[0].name == "Power" && !back[0].raw && back[0].protocol == "NEC" && back[0].address == 0x04 && back[0].command == 0x08, "parsed signal comes back the same");
            check(back[1].address == 0x02200280 && back[1].command == 0x13D, "4-byte address comes back the same");
            check(back[2].name == "Swing" && back[2].raw && back[2].frequency == 36000 && std::fabs(back[2].duty - 0.25f) < 1e-4f && back[2].data == raw.data, "raw signal comes back the same");
        }
        check(text.find("address: 80 02 20 02\n") != std::string::npos && text.find("data: 3400 1700 420") != std::string::npos, "the text is Flipper's layout");
    }
    {
        std::vector<IrSignal> sony;
        const bool ok = ir_file_parse(kSonyTvFile, sony);
        check(ok && sony.size() == 7, "Flipper-IRDB TVs/Sony/SonyTV.ir (excerpt): 7 buttons");
        if (sony.size() == 7) {
            check(sony[0].name == "On_off" && sony[0].protocol == "SIRC" && sony[0].address == 1 && sony[0].command == 0x15, "its first button is On_off, SIRC 01/15");
            check(sony[1].name == "Netflix" && sony[1].protocol == "SIRC15" && sony[1].address == 0x1A && sony[1].command == 0x7C, "its second is Netflix, SIRC15 1A/7C");
            bool all = true;
            for (const IrSignal& s : sony) all = all && ir_encode(s, t, hz, duty);
            check(all, "every button in it can be sent");
        }
        std::vector<IrSignal> none;
        check(!ir_file_parse("<html>404</html>", none) && none.empty(), "something that is not a .ir file is refused");
        const char lib[] = "Filetype: IR library file\r\nVersion: 1\r\n#\r\nname: Power\r\ntype: raw\r\nfrequency: 38000\r\nduty_cycle: 0.330000\r\ndata: 100 200 300\r\n";
        check(ir_file_parse(lib, none) && none.size() == 1 && none[0].data.size() == 3, "a library file with Windows line ends");
    }

    std::printf("[ir self-test] writing to the kernel in pieces\n");
    {
        std::vector<uint32_t> gaps;
        ir_encode(parsed("SIRC", 0x01, 0x15), t, hz, duty);
        check(ir_split_for_lirc(t, gaps).size() == 1 && gaps.size() == 1 && gaps[0] == 0, "a 3-frame Sony signal goes in one write");
        std::vector<uint32_t> big;
        for (int f = 0; f < 12; ++f) {
            for (int i = 0; i < 40; ++i) big.push_back(i % 2 == 0 ? 600 : 1200);
            big.push_back(500);
            big.push_back(90000);
        }
        big.pop_back();
        const auto pieces = ir_split_for_lirc(big, gaps);
        uint64_t sum = 0, want = 0;
        bool ok = pieces.size() == gaps.size() && pieces.size() > 1;
        for (size_t i = 0; i < pieces.size(); ++i) {
            uint64_t s = 0;
            for (uint32_t d : pieces[i]) s += d;
            ok = ok && pieces[i].size() % 2 == 1 && s <= 400000 && (i + 1 == pieces.size() ? gaps[i] == 0 : gaps[i] == 90000);
            sum += s + gaps[i];
        }
        for (uint32_t d : big) want += d;
        check(ok && sum == want, "1.4 s of signal: cut at the long gaps, nothing lost, each piece odd and under 0.4 s");
    }
    std::printf("[ir self-test] samples from the receiver\n");
    {
        ir_encode(parsed("NEC", 0x04, 0x08), t, hz, duty);
        auto frame = [&](IrCapture& c) {   // the NEC frame as the kernel would hand it over
            bool got = false;
            for (size_t i = 0; i < t.size(); ++i) got = c.feed((i % 2 == 0 ? kMode2Pulse : kMode2Space) | t[i]) || got;
            return got;
        };
        IrCapture a;
        a.feed(kMode2Space | 500000);              // the silence before
        check(!a.started(), "the silence before a button is not part of it");
        check(!frame(a) && a.started() && !a.done(), "a frame alone is not finished yet");
        check(a.feed(kMode2Timeout | 75000) && a.done() && a.timings() == t, "a timeout sample ends it: the same 67 timings");
        IrSignal d;
        check(ir_decode(a.timings(), d) && d.protocol == "NEC" && d.address == 4 && d.command == 8, "and they decode as NEC 04/08");

        IrCapture b;                               // noise first, then the button, then a held-key repeat
        b.feed(kMode2Pulse | 300);
        b.feed(kMode2Space | 200);
        b.feed(kMode2Pulse | 100);
        check(!b.feed(kMode2Timeout | 75000) && !b.started(), "a 3-sample burst is dropped as noise");
        frame(b);
        check(b.feed(kMode2Space | 96000) && b.timings() == t, "a long space ends it too");
        check(b.feed(kMode2Pulse | 9000) && b.timings() == t, "what comes after is left out");

        IrCapture c;                               // samples lost half way: start again
        for (size_t i = 0; i < 20; ++i) c.feed((i % 2 == 0 ? kMode2Pulse : kMode2Space) | t[i]);
        c.feed(kMode2Overflow);
        check(!c.started(), "an overflow sample throws the half frame away");
        frame(c);
        c.feed(kMode2Frequency | 38000);
        check(!c.done() && c.quiet() && c.timings() == t, "no more samples for a while ends it as well");

        IrCapture e;                               // pulses split in two, a space split in two
        e.feed(kMode2Pulse | 4000);
        e.feed(kMode2Pulse | 5000);
        e.feed(kMode2Space | 2000);
        e.feed(kMode2Space | 2500);
        for (size_t i = 2; i < t.size(); ++i) e.feed((i % 2 == 0 ? kMode2Pulse : kMode2Space) | t[i]);
        check(e.quiet() && e.timings() == t, "split pulses and spaces are joined");

        IrCapture f;                               // something that never stops
        bool full = false;
        for (int i = 0; i < 3000 && !full; ++i) full = f.feed((i % 2 == 0 ? kMode2Pulse : kMode2Space) | 500);
        check(full && f.timings().size() % 2 == 1 && f.timings().size() <= IrCapture::kMaxTimings, "an endless signal is cut at 1024 timings");
    }
    check(ir_safe_name("../../etc/passwd") == "etcpasswd" && ir_safe_name("  Sony_Bravia.ir ") == "Sony_Bravia.ir" && ir_safe_name("...").empty(), "file names are made safe");

    std::printf("[ir self-test] %d passed, %d failed\n", pass, fail);
    return fail;
}
#endif

// ================================================================ the backend
namespace {

using Clock = std::chrono::steady_clock;

// What the real and the demo backend share: the snapshot and everything that only moves
// buttons between it and the store of saved remotes.
class IrBase : public Ir {
public:
    IrState state() override {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        tick();
        return st_;
    }
    void open(const std::string& remote) override {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        st_.open.clear();
        st_.buttons.clear();
        if (!remote.empty() && store_load(remote, st_.buttons)) st_.open = remote;
    }
    void delete_button(int i) override {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        if (st_.open.empty() || i < 0 || i >= static_cast<int>(st_.buttons.size())) return;
        const std::string name = st_.buttons[i].name;
        st_.buttons.erase(st_.buttons.begin() + i);
        const bool ok = store_save(st_.open, st_.buttons);
        st_.remotes = store_list();
        note(ok ? "Deleted " + name : "Couldn't write the file");
    }
    void delete_remote(const std::string& remote) override {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        store_remove(remote);
        if (st_.open == remote) open("");
        st_.remotes = store_list();
        note("Deleted " + remote);
    }
    bool exists(const std::string& remote) override {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        for (const auto& r : store_list())
            if (r.name == remote) return true;
        return false;
    }
    void record_keep(const std::string& remote, const std::string& button, bool as_raw) override {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        if (st_.rec != IrRec::Caught || remote.empty()) return;
        std::vector<IrSignal> all;
        store_load(remote, all);
        IrSignal s = st_.caught;
        s.name = button.empty() ? "Button" : button;
        s.raw = as_raw || !st_.caught_decoded;
        if (!s.raw) s.data.clear();
        all.push_back(s);
        if (!store_save(remote, all)) {
            note("Couldn't write the file");
            return;
        }
        st_.rec = IrRec::Idle;
        st_.remotes = store_list();
        open(remote);
        note("Saved " + s.name);
    }

protected:
    virtual void tick() {}
    virtual std::vector<IrRemoteInfo> store_list() = 0;
    virtual bool store_load(const std::string& name, std::vector<IrSignal>& out) = 0;
    virtual bool store_save(const std::string& name, const std::vector<IrSignal>& buttons) = 0;
    virtual void store_remove(const std::string& name) = 0;

    void note(const std::string& s) {
        st_.note = s;
        ++st_.note_id;
    }
    // Saves a downloaded remote under a name that is free, opens it. Call with mu_ held.
    void keep_download(const std::string& file_name, const std::vector<IrSignal>& buttons) {
        std::string base = file_name;
        if (base.size() > 3 && base.compare(base.size() - 3, 3, ".ir") == 0) base.resize(base.size() - 3);
        std::replace(base.begin(), base.end(), '_', ' ');
        base = ir_safe_name(base);
        if (base.size() > 24) base = ir_safe_name(base.substr(0, 24));
        if (base.empty()) base = "Remote";
        std::string name = base;
        for (int k = 2; exists(name) && k < 100; ++k) name = base + " " + std::to_string(k);
        if (!store_save(name, buttons)) {
            st_.db_error = "Couldn't write the file";
            return;
        }
        st_.remotes = store_list();
        open(name);
        ++st_.saved_id;
        note("Saved as " + name);
    }

    std::recursive_mutex mu_;
    IrState st_;
};

// ---------------------------------------------------------------- demo
// Made-up remotes, a made-up recording and canned database folders: for screenshots, for trying
// the page on a PC (OPTIC_IR_DEMO=1) and for the web build. Touches no device, file or network.
class DemoIr final : public IrBase {
public:
    DemoIr() {
        auto b = [](const char* name, const char* proto, uint32_t a, uint32_t c) {
            IrSignal s;
            s.name = name;
            s.protocol = proto;
            s.address = a;
            s.command = c;
            return s;
        };
        store_["Living room TV"] = {b("Power", "NEC", 0x04, 0x08), b("Vol +", "NEC", 0x04, 0x02), b("Vol -", "NEC", 0x04, 0x03),
                                    b("Mute", "NEC", 0x04, 0x09), b("Input", "NEC", 0x04, 0x0B),
                                    b("Netflix", "NECext", 0xFB04, 0x56A9), b("Ambilight", "XMP-1", 0x10, 0x44)};
        IrSignal fan;
        fan.name = "Power";
        fan.raw = true;
        fan.data = {1280, 400, 1280, 400, 440, 1240, 1280, 400, 1280, 400, 440, 1240, 440, 1240, 440, 1240, 440, 1240, 440, 1240, 440, 1240, 1280};
        IrSignal fan2 = fan;
        fan2.name = "Speed";
        std::swap(fan2.data[4], fan2.data[6]);
        store_["Desk fan"] = {fan, fan2};
        store_["Projector"] = {b("On", "Kaseikyo", 0x02200280, 0x13D), b("Off", "Kaseikyo", 0x02200280, 0x13E),
                               b("Source", "RC6", 0x00, 0x38)};
        st_.demo = true;
        st_.scanned = true;
        st_.tx = "lirc1";
        st_.rx = "lirc0";
        st_.devices = "send: lirc1  receive: lirc0";
        st_.remotes = store_list();
    }
    void send(int i) override {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        if (i < 0 || i >= static_cast<int>(st_.buttons.size())) return;
        std::vector<uint32_t> t;
        uint32_t hz = 0;
        float duty = 0;
        std::string why;
        note(ir_encode(st_.buttons[i], t, hz, duty, &why) ? "Sent " + st_.buttons[i].name : why);
    }
    void record_start() override {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        st_.rec = IrRec::Waiting;
        st_.rec_left_s = 30;
        rec_at_ = Clock::now() + std::chrono::milliseconds(1500);
    }
    void record_cancel() override {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        st_.rec = IrRec::Idle;
    }
    void db_list(const std::string& path) override {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        st_.db_path = path;
        st_.db_error.clear();
        st_.db_busy = false;
        st_.db.clear();
        const size_t depth = path.empty() ? 0 : 1 + static_cast<size_t>(std::count(path.begin(), path.end(), '/'));
        if (depth == 0) {
            for (const char* k : {"ACs", "Audio_and_Video_Receivers", "Blu-Ray", "Cable_Boxes", "Fans", "LED_Lighting",
                                  "Projectors", "SoundBars", "Streaming_Devices", "TVs"})
                st_.db.push_back({k, true});
        } else if (depth == 1) {
            for (const char* k : {"Hisense", "LG", "Panasonic", "Philips", "Samsung", "Sharp", "Sony", "TCL", "Toshiba", "Vizio"})
                st_.db.push_back({k, true});
        } else {
            const std::string brand = path.substr(path.rfind('/') + 1);
            for (const char* k : {"TV.ir", "_Bravia.ir", "_Bravia_KDL-46W905A.ir", "_RM-YD092.ir", "_RMT_TX200U.ir", "_XBR.ir"})
                st_.db.push_back({brand + k, false});
        }
    }
    void db_fetch(const std::string& path) override {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        std::vector<IrSignal> b;
        ir_file_parse(kSonyTvFile, b);
        keep_download(path.substr(path.rfind('/') + 1), b);
    }

protected:
    void tick() override {
        if (st_.rec != IrRec::Waiting) return;
        const auto now = Clock::now();
        if (now < rec_at_) {
            st_.rec_left_s = 29;
            return;
        }
        IrSignal s;
        s.protocol = "NEC";
        s.address = 0x04;
        s.command = 0x1A;
        uint32_t hz = 0;
        float duty = 0;
        ir_encode(s, s.data, hz, duty);
        st_.caught = s;
        st_.caught_decoded = true;
        st_.rec = IrRec::Caught;
    }
    std::vector<IrRemoteInfo> store_list() override {
        std::vector<IrRemoteInfo> v;
        for (const auto& kv : store_) v.push_back({kv.first, static_cast<int>(kv.second.size())});
        return v;
    }
    bool store_load(const std::string& name, std::vector<IrSignal>& out) override {
        const auto it = store_.find(name);
        if (it == store_.end()) return false;
        out = it->second;
        return true;
    }
    bool store_save(const std::string& name, const std::vector<IrSignal>& buttons) override {
        store_[name] = buttons;
        return true;
    }
    void store_remove(const std::string& name) override { store_.erase(name); }

private:
    std::map<std::string, std::vector<IrSignal>> store_;
    Clock::time_point rec_at_{};
};

}  // namespace
}  // namespace velvet

// ================================================================ the real thing (Linux)
#if defined(__linux__) && !defined(__EMSCRIPTEN__)

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <linux/lirc.h>
#include <poll.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "json_lite.h"

namespace velvet {
namespace {

namespace fs = std::filesystem;

static_assert(kMode2Space == LIRC_MODE2_SPACE && kMode2Pulse == LIRC_MODE2_PULSE && kMode2Frequency == LIRC_MODE2_FREQUENCY &&
              kMode2Timeout == LIRC_MODE2_TIMEOUT && kMode2Mask == LIRC_MODE2_MASK && kMode2Value == LIRC_VALUE_MASK,
              "the MODE2 layout above is the kernel's");
#ifdef LIRC_MODE2_OVERFLOW
static_assert(kMode2Overflow == LIRC_MODE2_OVERFLOW, "the MODE2 layout above is the kernel's");
#endif
constexpr int kRecSeconds = 30;
constexpr long kDbCacheSeconds = 24 * 3600;
const char kDbApi[] = "https://api.github.com/repos/Lucaslhm/Flipper-IRDB/contents/";
const char kDbRaw[] = "https://raw.githubusercontent.com/Lucaslhm/Flipper-IRDB/main/";

std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h && *h ? h : "/tmp";
}

std::string read_text(const std::string& path, size_t max = 4u << 20) {
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[8192];
        size_t n;
        while (out.size() < max && (n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

// The names of the kernel's LIRC devices ("lirc0", ...), from /sys/class/rc/rc*/ and /dev.
// Only directory listings: nothing is opened.
std::vector<std::string> lirc_names() {
    std::vector<std::string> names;
    auto is_lirc = [](const char* n) {
        if (std::strncmp(n, "lirc", 4) != 0 || !n[4]) return false;
        for (const char* p = n + 4; *p; ++p)
            if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
        return true;
    };
    auto add = [&](const char* n) {
        if (is_lirc(n) && std::find(names.begin(), names.end(), n) == names.end()) names.emplace_back(n);
    };
    if (DIR* rc = opendir("/sys/class/rc")) {
        while (const dirent* e = readdir(rc)) {
            if (std::strncmp(e->d_name, "rc", 2) != 0) continue;
            if (DIR* one = opendir((std::string("/sys/class/rc/") + e->d_name).c_str())) {
                while (const dirent* l = readdir(one)) add(l->d_name);
                closedir(one);
            }
        }
        closedir(rc);
    }
    if (DIR* dev = opendir("/dev")) {
        while (const dirent* e = readdir(dev)) add(e->d_name);
        closedir(dev);
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Percent-encodes a database path for a URL ('/' stays).
std::string url_path(const std::string& s) {
    std::string r;
    char buf[4];
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') r += ch;
        else {
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            r += buf;
        }
    }
    return r;
}

class LinuxIr final : public IrBase {
public:
    LinuxIr() {
        dir_ = home_dir() + "/.local/share/optic-suite/remotes";
        cache_ = home_dir() + "/.cache/optic-suite/irdb";
        st_.remotes = store_list();
        worker_ = std::thread([this] { run(); });
        post([this] { scan(); });
    }
    ~LinuxIr() override {
        {
            std::lock_guard<std::mutex> lk(qmu_);
            stop_ = true;
            q_.clear();
        }
        ++rec_gen_;
        if (const pid_t c = child_.load(); c > 0) kill(c, SIGTERM);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void send(int i) override {
        IrSignal s;
        {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            if (i < 0 || i >= static_cast<int>(st_.buttons.size()) || st_.sending) return;
            s = st_.buttons[i];
            st_.sending = true;
        }
        post([this, s] {
            const std::string result = transmit(s);
            std::lock_guard<std::recursive_mutex> lk(mu_);
            st_.sending = false;
            note(result);
        });
    }
    void record_start() override {
        {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            if (st_.rec == IrRec::Waiting) return;
            st_.rec = IrRec::Waiting;
            st_.rec_left_s = kRecSeconds;
            st_.rec_error.clear();
        }
        const int gen = ++rec_gen_;
        post([this, gen] { record(gen); });
    }
    void record_cancel() override {
        ++rec_gen_;
        std::lock_guard<std::recursive_mutex> lk(mu_);
        st_.rec = IrRec::Idle;
    }
    void db_list(const std::string& path) override {
        {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            st_.db_busy = true;
            st_.db_path = path;
            st_.db.clear();
            st_.db_error.clear();
        }
        post([this, path] { list_folder(path); });
    }
    void db_fetch(const std::string& path) override {
        {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            if (st_.db_busy) return;
            st_.db_busy = true;
            st_.db_error.clear();
        }
        post([this, path] { fetch_file(path); });
    }

protected:
    // ---- saved remotes: one Flipper .ir file each ----
    std::string file_of(const std::string& name) const { return dir_ + "/" + name + ".ir"; }
    std::vector<IrRemoteInfo> store_list() override {
        std::vector<IrRemoteInfo> v;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir_, ec)) {
            const fs::path& p = e.path();
            if (p.extension() != ".ir" || !e.is_regular_file(ec)) continue;
            const std::string name = p.stem().string();
            if (name.empty() || name != ir_safe_name(name)) continue;   // only names this page can type
            std::vector<IrSignal> b;
            ir_file_parse(read_text(p.string()), b);
            v.push_back({name, static_cast<int>(b.size())});
        }
        std::sort(v.begin(), v.end(), [](const IrRemoteInfo& a, const IrRemoteInfo& b) {
            return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        return v;
    }
    bool store_load(const std::string& name, std::vector<IrSignal>& out) override {
        if (name.empty() || name != ir_safe_name(name)) return false;
        std::error_code ec;
        if (!fs::exists(file_of(name), ec)) return false;
        ir_file_parse(read_text(file_of(name)), out);
        return true;
    }
    bool store_save(const std::string& name, const std::vector<IrSignal>& buttons) override {
        if (name.empty() || name != ir_safe_name(name)) return false;
        std::error_code ec;
        fs::create_directories(dir_, ec);
        const std::string tmp = file_of(name) + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << ir_file_write(buttons);
            if (!f.good()) return false;
        }
        fs::rename(tmp, file_of(name), ec);
        return !ec;
    }
    void store_remove(const std::string& name) override {
        if (name.empty() || name != ir_safe_name(name)) return;
        std::error_code ec;
        fs::remove(file_of(name), ec);
    }

private:
    // ---- the worker ----
    void post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lk(qmu_);
            q_.push_back(std::move(job));
        }
        cv_.notify_one();
    }
    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(qmu_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_) return;
                job = std::move(q_.front());
                q_.pop_front();
            }
            job();
        }
    }

    // ---- the devices ----
    // Asks each /dev/lircN what it can do: the first that sends pulses is the transmitter, the
    // first that receives raw timings (MODE2) the receiver.
    void scan() {
        std::string tx, rx, denied;
        uint32_t tx_feat = 0, rx_feat = 0;
        for (const std::string& n : lirc_names()) {
            const std::string path = "/dev/" + n;
            const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                if (errno == EACCES && denied.empty()) denied = n;
                std::fprintf(stderr, "[optic] ir: %s: %s\n", path.c_str(), std::strerror(errno));
                continue;
            }
            uint32_t feat = 0;
            if (ioctl(fd, LIRC_GET_FEATURES, &feat) == 0) {
                std::fprintf(stderr, "[optic] ir: %s features 0x%08x%s%s\n", path.c_str(), feat,
                             (feat & LIRC_CAN_SEND_PULSE) ? " send" : "", (feat & LIRC_CAN_REC_MODE2) ? " receive" : "");
                if ((feat & LIRC_CAN_SEND_PULSE) && tx.empty()) { tx = n; tx_feat = feat; }
                if ((feat & LIRC_CAN_REC_MODE2) && rx.empty()) { rx = n; rx_feat = feat; }
            }
            ::close(fd);
        }
        std::lock_guard<std::recursive_mutex> lk(mu_);
        st_.tx = tx;
        st_.rx = rx;
        tx_feat_ = tx_feat;
        rx_feat_ = rx_feat;
        st_.scanned = true;
        if (tx.empty() && rx.empty()) st_.devices = denied.empty() ? "No transmitter or receiver found" : "No permission for /dev/" + denied;
        else st_.devices = (tx.empty() ? std::string("No transmitter found") : "send: " + tx) + "  " +
                           (rx.empty() ? std::string("No receiver found") : "receive: " + rx);
    }

    // Sends one press. Returns the line to show.
    std::string transmit(const IrSignal& s) {
        std::vector<uint32_t> t;
        uint32_t hz = 38000;
        float duty = 0.33f;
        std::string why;
        if (!ir_encode(s, t, hz, duty, &why, toggle_)) return why;
        toggle_ = !toggle_;
        std::string tx;
        uint32_t feat = 0;
        {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            tx = st_.tx;
            feat = tx_feat_;
        }
        if (tx.empty()) return "No transmitter found";
        const int fd = ::open(("/dev/" + tx).c_str(), O_WRONLY | O_CLOEXEC);
        if (fd < 0) return "Can't open " + tx + ": " + std::strerror(errno);
        uint32_t v = LIRC_MODE_PULSE;
        ioctl(fd, LIRC_SET_SEND_MODE, &v);
        v = hz;
        const bool carrier_ok = ioctl(fd, LIRC_SET_SEND_CARRIER, &v) >= 0;
        if (feat & LIRC_CAN_SET_SEND_DUTY_CYCLE) {
            v = static_cast<uint32_t>(std::max(1L, std::min(99L, std::lround(duty * 100))));
            ioctl(fd, LIRC_SET_SEND_DUTY_CYCLE, &v);
        }
        // The kernel takes at most about half a second of signal in one write and waits until
        // it is out; longer signals go in pieces, the gap between them slept here.
        std::vector<uint32_t> gaps;
        const auto pieces = ir_split_for_lirc(t, gaps);
        std::string err;
        for (size_t i = 0; i < pieces.size() && err.empty(); ++i) {
            const size_t bytes = pieces[i].size() * sizeof(uint32_t);
            const ssize_t n = ::write(fd, pieces[i].data(), bytes);
            if (n < 0) err = std::string("Couldn't send: ") + std::strerror(errno);
            else if (static_cast<size_t>(n) != bytes) err = "Couldn't send all of it";
            else if (gaps[i]) usleep(std::min<uint32_t>(gaps[i], 1000000));
        }
        ::close(fd);
        if (!err.empty()) return err;
        return "Sent " + s.name + (carrier_ok ? "" : " (carrier not set)");
    }

    // Waits for one button: timings from the first pulse to the first long silence.
    // `gen` is this request's number: a cancel or a newer request makes it stale.
    void record(int gen) {
        if (gen != rec_gen_) return;
        std::string rx;
        uint32_t feat = 0;
        {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            rx = st_.rx;
            feat = rx_feat_;
        }
        auto fail = [this, gen](const std::string& why) {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            if (gen != rec_gen_ || st_.rec != IrRec::Waiting) return;
            st_.rec = IrRec::Failed;
            st_.rec_error = why;
        };
        if (rx.empty()) return fail("No receiver found");
        const int fd = ::open(("/dev/" + rx).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return fail("Can't open " + rx + ": " + std::strerror(errno));
        uint32_t v = LIRC_MODE_MODE2;
        if (ioctl(fd, LIRC_SET_REC_MODE, &v) < 0) {
            ::close(fd);
            return fail(rx + " gives no raw timings");
        }
        if (feat & LIRC_CAN_SET_REC_TIMEOUT) {   // have the kernel report the end of a burst sooner
            v = IrCapture::kGapUs;
            ioctl(fd, LIRC_SET_REC_TIMEOUT, &v);
        }
        uint32_t buf[256];
        while (::read(fd, buf, sizeof buf) > 0) {}   // whatever came before now is not the button

        IrCapture cap;
        const auto t0 = Clock::now();
        auto last = t0;
        while (!cap.done() && gen == rec_gen_) {
            const auto now = Clock::now();
            const int left = kRecSeconds - static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - t0).count());
            if (left <= 0) break;
            {
                std::lock_guard<std::recursive_mutex> lk(mu_);
                st_.rec_left_s = left;
            }
            pollfd p{fd, POLLIN, 0};
            const int r = poll(&p, 1, 50);
            if (r < 0 && errno != EINTR) break;
            if (r <= 0 || !(p.revents & POLLIN)) {
                if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
                // nothing more for a while after a burst: that was the button
                if (cap.started() && now - last > std::chrono::microseconds(2 * IrCapture::kGapUs)) cap.quiet();
                continue;
            }
            const ssize_t n = ::read(fd, buf, sizeof buf);
            if (n <= 0) continue;
            last = Clock::now();
            for (size_t i = 0; i < static_cast<size_t>(n) / sizeof(uint32_t) && !cap.done(); ++i) cap.feed(buf[i]);
        }
        ::close(fd);
        std::lock_guard<std::recursive_mutex> lk(mu_);
        if (gen != rec_gen_ || st_.rec != IrRec::Waiting) return;   // cancelled meanwhile
        if (!cap.done()) {
            st_.rec = IrRec::Nothing;
            return;
        }
        IrSignal s;
        s.frequency = 38000;   // a demodulating receiver does not tell the carrier
        s.duty = 0.33f;
        st_.caught_decoded = ir_decode(cap.timings(), s);
        s.data = cap.timings();
        st_.caught = s;
        st_.rec = IrRec::Caught;
    }

    // ---- the online database ----
    // Runs argv (no shell). Returns the exit status (-1: did not run) and what it printed.
    int run_cmd(const std::vector<std::string>& argv, std::string& out) {
        int fd[2];
        if (pipe(fd)) return -1;
        std::vector<char*> a;
        for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        const pid_t pid = fork();
        if (pid == 0) {
            dup2(fd[1], 1);
            const int nul = ::open("/dev/null", O_RDWR);
            if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
            ::close(fd[0]);
            ::close(fd[1]);
            execvp(a[0], a.data());
            _exit(127);
        }
        ::close(fd[1]);
        int st = -1;
        if (pid > 0) {
            child_ = pid;
            char buf[1024];
            ssize_t n;
            while ((n = ::read(fd[0], buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
            int ws = 0;
            if (waitpid(pid, &ws, 0) == pid && WIFEXITED(ws)) st = WEXITSTATUS(ws);
            child_ = 0;
        }
        ::close(fd[0]);
        return st;
    }
    // Downloads url to dest. Returns the HTTP status, 0 when GitHub was not reached, -1 without curl.
    int http_get(const std::string& url, const std::string& dest, bool api) {
        std::error_code ec;
        fs::create_directories(cache_, ec);
        std::vector<std::string> argv{"curl", "-sS", "-L", "--connect-timeout", "8", "--max-time", "25",
                                      "--max-filesize", "4000000", "-A", "optic-suite", "-o", dest, "-w", "%{http_code}"};
        if (api) argv.insert(argv.end(), {"-H", "Accept: application/vnd.github+json"});
        argv.push_back(url);
        std::string out;
        const int st = run_cmd(argv, out);
        if (st == 127 || st < 0) return -1;
        const int code = std::atoi(out.c_str());
        return st == 0 || code >= 400 ? code : 0;
    }
    static std::string http_error(int code) {
        if (code == 403 || code == 429) return "GitHub's hourly limit is used up, try later";
        if (code == 404) return "That is not in the database any more";
        if (code == -1) return "curl is not installed";
        if (code == 0) return "Can't reach GitHub";
        return "GitHub answered " + std::to_string(code);
    }
    std::string cache_file(const std::string& path) const {
        std::string n;
        for (char ch : path) n += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
        if (n.size() > 80) n.resize(80);
        char h[24];
        std::snprintf(h, sizeof h, "-%08zx", std::hash<std::string>{}(path) & 0xFFFFFFFF);
        return cache_ + "/list_" + n + h + ".json";
    }
    void list_folder(const std::string& path) {
        const std::string file = cache_file(path), tmp = file + ".tmp";
        std::error_code ec;
        bool have = fs::exists(file, ec);
        long age = kDbCacheSeconds;
        struct stat sb {};
        if (have && stat(file.c_str(), &sb) == 0) age = static_cast<long>(time(nullptr) - sb.st_mtime);
        std::string error, warn;
        if (!have || age < 0 || age >= kDbCacheSeconds) {
            const int code = http_get(kDbApi + url_path(path), tmp, true);
            if (code == 200) {
                fs::rename(tmp, file, ec);
                have = !ec;
            } else {
                fs::remove(tmp, ec);
                if (have) warn = code == 403 || code == 429 ? "Hourly limit used up: an older list" : "Can't reach GitHub: an older list";
                else error = http_error(code);
            }
        }
        std::vector<IrDbEntry> items;
        if (error.empty()) {
            const J j = json_parse(read_text(file));
            if (j.t != J::Arr) {
                error = "GitHub's answer made no sense";
                fs::remove(file, ec);
            }
            for (const J& e : j.a) {
                const std::string& name = e["name"].s;
                const bool folder = e["type"].s == "dir";
                if (name.empty() || name[0] == '.' || name.find('/') != std::string::npos) continue;
                if (!folder && (name.size() < 4 || name.compare(name.size() - 3, 3, ".ir") != 0)) continue;
                items.push_back({name, folder});
            }
            if (error.empty() && items.empty()) error = "Nothing in this folder";
        }
        std::lock_guard<std::recursive_mutex> lk(mu_);
        if (st_.db_path != path) return;   // the page has moved on
        st_.db = std::move(items);
        st_.db_error = error;
        st_.db_busy = false;
        if (!warn.empty()) note(warn);
    }
    void fetch_file(const std::string& path) {
        const std::string tmp = cache_ + "/download.tmp";
        const int code = http_get(kDbRaw + url_path(path), tmp, false);
        std::vector<IrSignal> buttons;
        std::string error;
        if (code != 200) error = http_error(code);
        else if (!ir_file_parse(read_text(tmp), buttons) || buttons.empty()) error = "No buttons in that file";
        std::error_code ec;
        fs::remove(tmp, ec);
        std::lock_guard<std::recursive_mutex> lk(mu_);
        st_.db_busy = false;
        if (!error.empty()) {
            note(error);
            return;
        }
        keep_download(path.substr(path.rfind('/') + 1), buttons);
    }

    std::string dir_, cache_;
    uint32_t tx_feat_{0}, rx_feat_{0};
    bool toggle_{false};   // RC5 / RC6: flips with every press
    std::thread worker_;
    std::mutex qmu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool stop_{false};
    std::atomic<int> rec_gen_{0};
    std::atomic<pid_t> child_{0};
};

}  // namespace

std::unique_ptr<Ir> make_ir() {
    // OPTIC_IR_DEMO=1: the demo, always. OPTIC_IR_DEMO=0: the real thing, even on a PC without infrared.
    const char* d = std::getenv("OPTIC_IR_DEMO");
    if (d && *d == '1') return std::make_unique<DemoIr>();
#if defined(USE_DESKTOP) && USE_DESKTOP
    if (!(d && *d == '0') && lirc_names().empty()) return std::make_unique<DemoIr>();   // a PC without infrared
#endif
    return std::make_unique<LinuxIr>();
}

}  // namespace velvet

#else

namespace velvet {
std::unique_ptr<Ir> make_ir() { return std::make_unique<DemoIr>(); }
}  // namespace velvet

#endif

#ifdef OPTIC_IR_SELFTEST_MAIN
int main() { return velvet::ir_self_test() ? 1 : 0; }
#endif
