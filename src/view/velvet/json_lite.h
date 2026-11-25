// A tiny JSON reader: enough for `tailscale status --json` and Optic Suite's answers.
#pragma once
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace velvet {
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t{Null};
    bool b{false};
    double n{0};
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    const J& operator[](const char* k) const {
        static const J none;
        for (auto& kv : o) if (kv.first == k) return kv.second;
        return none;
    }
};
struct JParse {
    const char* p;
    void ws() { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p; }
    std::string str() {
        std::string r;
        ++p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                ++p;
                if (*p == 'n') r += '\n'; else if (*p == 't') r += '\t';
                else if (*p == 'u') { p += 4; r += '?'; }
                else r += *p;
            } else r += *p;
            ++p;
        }
        if (*p) ++p;
        return r;
    }
    J val() {
        ws();
        J j;
        if (*p == '{') {
            j.t = J::Obj; ++p;
            for (;;) {
                ws(); if (*p == '}') { ++p; break; }
                if (*p != '"') break;
                std::string k = str(); ws(); if (*p == ':') ++p;
                j.o.emplace_back(k, val()); ws();
                if (*p == ',') ++p;
            }
        } else if (*p == '[') {
            j.t = J::Arr; ++p;
            for (;;) {
                ws(); if (*p == ']') { ++p; break; }
                if (!*p) break;
                j.a.push_back(val()); ws();
                if (*p == ',') ++p;
            }
        } else if (*p == '"') { j.t = J::Str; j.s = str(); }
        else if (!std::strncmp(p, "true", 4)) { j.t = J::Bool; j.b = true; p += 4; }
        else if (!std::strncmp(p, "false", 5)) { j.t = J::Bool; p += 5; }
        else if (!std::strncmp(p, "null", 4)) { p += 4; }
        else { char* e; j.t = J::Num; j.n = std::strtod(p, &e); p = e == p ? p + 1 : e; }
        return j;
    }
};


inline J json_parse(const std::string& s) { JParse p{s.c_str()}; return p.val(); }

}  // namespace velvet
