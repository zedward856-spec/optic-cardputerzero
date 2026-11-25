#include "browser.h"
#include "netlink.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "lvgl.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef __EMSCRIPTEN__
#include <emscripten/fetch.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace velvet {

// ======================================================================== text helpers
size_t utf8_len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

namespace {
void put_utf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) s += static_cast<char>(cp);
    else if (cp < 0x800) {
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

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// &amp; &lt; &#39; &#x2014; ... -> UTF-8. Typographic marks become plain ASCII the font has.
std::string decode_entities(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '&') {
            out += in[i];
            continue;
        }
        const size_t semi = in.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += '&';
            continue;
        }
        const std::string name = in.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        if (!name.empty() && name[0] == '#') {
            cp = static_cast<uint32_t>(name.size() > 1 && (name[1] == 'x' || name[1] == 'X')
                                           ? std::strtoul(name.c_str() + 2, nullptr, 16)
                                           : std::strtoul(name.c_str() + 1, nullptr, 10));
        } else {
            static const struct { const char* n; uint32_t c; } kNames[] = {
                {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
                {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"rsquo", 0x2019}, {"lsquo", 0x2018},
                {"rdquo", 0x201D}, {"ldquo", 0x201C}, {"copy", 0xA9}, {"middot", 0xB7}, {"bull", 0x2022},
                {"deg", 0xB0}, {"times", 'x'}, {"rarr", 0x2192}, {"larr", 0x2190}, {"raquo", 0xBB}, {"laquo", 0xAB},
            };
            for (const auto& e : kNames) {
                if (name == e.n) cp = e.c;
            }
        }
        if (!cp) {
            out += in.substr(i, semi - i + 1);
        } else {
            switch (cp) {  // keep to glyphs the pixel fonts have
                case 0x2018: case 0x2019: cp = '\''; break;
                case 0x201C: case 0x201D: cp = '"'; break;
                case 0x2013: case 0x2014: cp = '-'; break;
                case 0xA0: cp = ' '; break;
                default: break;
            }
            if (cp == 0x2026) out += "...";
            else put_utf8(out, cp);
        }
        i = semi;
    }
    return out;
}

std::string attr(const std::string& tag, const char* name) {
    const std::string t = lower(tag);
    size_t p = 0;
    const std::string key = name;
    while ((p = t.find(key, p)) != std::string::npos) {
        const bool start_ok = p > 0 && (std::isspace(static_cast<unsigned char>(t[p - 1])));
        size_t q = p + key.size();
        while (q < t.size() && std::isspace(static_cast<unsigned char>(t[q]))) ++q;
        if (!start_ok || q >= t.size() || t[q] != '=') {
            p += key.size();
            continue;
        }
        ++q;
        while (q < t.size() && std::isspace(static_cast<unsigned char>(t[q]))) ++q;
        if (q >= t.size()) return {};
        if (tag[q] == '"' || tag[q] == '\'') {
            const char quote = tag[q];
            const size_t e = tag.find(quote, q + 1);
            return decode_entities(tag.substr(q + 1, e == std::string::npos ? std::string::npos : e - q - 1));
        }
        size_t e = q;
        while (e < tag.size() && !std::isspace(static_cast<unsigned char>(tag[e])) && tag[e] != '>') ++e;
        return decode_entities(tag.substr(q, e - q));
    }
    return {};
}
}  // namespace

std::string resolve_url(const std::string& base, const std::string& href) {
    if (href.empty()) return base;
    if (href.find("://") != std::string::npos || href.rfind("mailto:", 0) == 0 || href.rfind("velvet:", 0) == 0) return href;
    const size_t scheme_end = base.find("://");
    if (scheme_end == std::string::npos) return href;
    const std::string scheme = base.substr(0, scheme_end);
    if (href.rfind("//", 0) == 0) return scheme + ":" + href;
    const size_t host_end = base.find('/', scheme_end + 3);
    const std::string origin = host_end == std::string::npos ? base : base.substr(0, host_end);
    if (href[0] == '/') return origin + href;
    if (href[0] == '#') return base.substr(0, base.find('#')) + href;
    std::string path = host_end == std::string::npos ? "/" : base.substr(host_end);
    path = path.substr(0, path.find_first_of("?#"));
    path = path.substr(0, path.rfind('/') + 1);
    if (href[0] == '?') return origin + (host_end == std::string::npos ? "/" : base.substr(host_end, base.find_first_of("?#", host_end) - host_end)) + href;
    std::string joined = path + href;
    // collapse ./ and ../
    std::vector<std::string> parts;
    size_t i = 0;
    const std::string tail_q = joined.find_first_of("?#") == std::string::npos ? "" : joined.substr(joined.find_first_of("?#"));
    joined = joined.substr(0, joined.find_first_of("?#"));
    while (i <= joined.size()) {
        const size_t j = joined.find('/', i);
        const std::string seg = joined.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (seg != "." && !(seg.empty() && j != std::string::npos && i != 0)) {
            parts.push_back(seg);
        }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    std::string out;
    for (size_t k = 0; k < parts.size(); ++k) out += (k ? "/" : "") + parts[k];
    if (out.empty() || out[0] != '/') out = "/" + out;
    return origin + out + tail_q;
}

std::string normalize_input(const std::string& typed) {
    std::string t = typed;
    while (!t.empty() && t.back() == ' ') t.pop_back();
    while (!t.empty() && t.front() == ' ') t.erase(0, 1);
    if (t.empty()) return "https://duckduckgo.com/";
    if (t.rfind("velvet:", 0) == 0 || t.find("://") != std::string::npos) return t;
    if (t.find(' ') == std::string::npos && t.find('.') != std::string::npos) return "https://" + t;
    return search_url(t);
}

namespace {
std::string url_encode(const std::string& t) {
    std::string q;
    for (unsigned char c : t) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') q += static_cast<char>(c);
        else if (c == ' ') q += '+';
        else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            q += b;
        }
    }
    return q;
}
}  // namespace

// The address a page sends you on to without showing anything itself: <meta http-equiv=refresh
// content="0;URL=..."> (also inside <noscript>), or the one-line script search engines use,
// location.replace("...") / location.href = "...". "" when the page has neither. Only pages too
// short to be a real page should be asked (the caller checks): real pages mention these too.
std::string redirect_target(const std::string& html) {
    std::string low = html;
    for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t at = low.find("http-equiv");
    while (at != std::string::npos) {
        const size_t end = low.find('>', at);
        if (end == std::string::npos) break;
        const std::string tag = low.substr(at, end - at);
        const size_t u = tag.find("url=");
        if (tag.find("refresh") != std::string::npos && u != std::string::npos) {
            size_t b0 = at + u + 4, e0 = b0;
            while (b0 < end && (html[b0] == '\'' || html[b0] == '"' || html[b0] == ' ')) ++b0;
            e0 = b0;
            while (e0 < end && html[e0] != '\'' && html[e0] != '"' && html[e0] != ' ' && html[e0] != '>') ++e0;
            if (e0 > b0) return decode_entities(html.substr(b0, e0 - b0));
        }
        at = low.find("http-equiv", end);
    }
    for (const char* key : {"location.replace(", "location.href=", "location.href =", "location="}) {
        const size_t k = low.find(key);
        if (k == std::string::npos) continue;
        size_t b0 = k + std::strlen(key);
        while (b0 < html.size() && html[b0] == ' ') ++b0;
        if (b0 >= html.size() || (html[b0] != '"' && html[b0] != '\'')) continue;
        const char q = html[b0++];
        const size_t e0 = html.find(q, b0);
        if (e0 != std::string::npos && e0 > b0) return html.substr(b0, e0 - b0);
    }
    return {};
}

std::string unwrap_redirect(const std::string& url) {
    // DuckDuckGo result links go through //duckduckgo.com/l/?uddg=<the target, percent-encoded>&rut=...
    // and that hop answers with a script-only page, which shows as an empty page here
    const size_t dl = url.find("duckduckgo.com/l/");
    if (dl != std::string::npos && dl <= 16) {
        const size_t u = url.find("uddg=", dl);
        if (u != std::string::npos) {
            const std::string enc = url.substr(u + 5, url.find('&', u) - (u + 5));
            std::string target;
            for (size_t j = 0; j < enc.size(); ++j) {
                if (enc[j] == '%' && j + 2 < enc.size()) {
                    target += static_cast<char>(std::strtol(enc.substr(j + 1, 2).c_str(), nullptr, 16));
                    j += 2;
                } else {
                    target += enc[j] == '+' ? ' ' : enc[j];
                }
            }
            if (target.rfind("http", 0) == 0) return target;
        }
    }
    // Bing result links go through https://www.bing.com/ck/a?...&u=a1<base64url of the target>
    const size_t ck = url.find("bing.com/ck/a");
    if (ck == std::string::npos || ck > 24) return url;
    size_t u = url.find("&u=a1", ck);
    if (u == std::string::npos) u = url.find("?u=a1", ck);
    if (u == std::string::npos) return url;
    std::string b = url.substr(u + 5, url.find('&', u + 5) - (u + 5));
    for (auto& c : b) c = c == '-' ? '+' : c == '_' ? '/' : c;
    static const std::string kAlpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    uint32_t buf = 0;
    int bits = 0;
    for (char c : b) {
        const size_t v = kAlpha.find(c);
        if (v == std::string::npos) break;
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    if (out.empty()) return url;
    if (out[0] == '/') return url.substr(0, url.find("/ck/a")) + out;
    return out.rfind("http", 0) == 0 ? out : url;
}

std::string full_home_html() {
    return "<!doctype html><meta name=viewport content='width=512'><title>CardputerZero</title>"
           "<style>body{font:16px/1.5 sans-serif;margin:24px;color:#15161c}h1{font:700 30px sans-serif;"
           "margin:0 0 8px}p{color:#555}a{color:#1f3fd6}li{margin:6px 0}</style>"
           "<h1>CardputerZero browser</h1><p>Press <b>TAB</b> and type words to search (DuckDuckGo), or an address.</p>"
           "<ul><li><a href='https://en.wikipedia.org/wiki/Special:Random'>Wikipedia: random article</a></li>"
           "<li><a href='https://news.ycombinator.com/'>Hacker News</a></li><li><a href='https://www.reddit.com/'>"
           "Reddit</a></li><li><a href='https://r6.tracker.network/'>R6 Tracker</a></li>"
           "<li><a href='https://text.npr.org/'>NPR text news</a></li></ul>";
}

std::string ddg_lite_for_chrome(const std::string& in) {
    std::string out;
    size_t i = 0;
    const std::string key = "href=\"//duckduckgo.com/l/?";
    while (i < in.size()) {
        const size_t k = in.find(key, i);
        if (k == std::string::npos) {
            out.append(in, i, std::string::npos);
            break;
        }
        out.append(in, i, k - i);
        const size_t end = in.find('"', k + 6);
        const std::string href = decode_entities(in.substr(k + 6, end - (k + 6)));
        std::string target = href;
        const size_t u = href.find("uddg=");
        if (u != std::string::npos) {
            const std::string enc = href.substr(u + 5, href.find('&', u) - (u + 5));
            target.clear();
            for (size_t j = 0; j < enc.size(); ++j) {
                if (enc[j] == '%' && j + 2 < enc.size()) {
                    target += static_cast<char>(std::strtol(enc.substr(j + 1, 2).c_str(), nullptr, 16));
                    j += 2;
                } else {
                    target += enc[j] == '+' ? ' ' : enc[j];
                }
            }
        }
        out += "href=\"" + target + "\"";
        i = end == std::string::npos ? in.size() : end + 1;
    }
    const size_t head = out.find("<head>");
    if (head != std::string::npos) out.insert(head + 6, "<base href=\"https://lite.duckduckgo.com/lite/\">");
    return out;
}

std::string search_url(const std::string& query) {
#ifdef __EMSCRIPTEN__
    return "https://en.wikipedia.org/w/rest.php/v1/search/page?limit=20&q=" + url_encode(query);
#else
    return kDdgLiteSearch + url_encode(query);  // made for simple browsers: answers a plain request
#endif
}

std::string form_url(const Doc& doc, int form, const std::vector<std::string>& values) {
    if (form < 0 || form >= static_cast<int>(doc.forms.size())) return doc.url;
    const Form& f = doc.forms[form];
    if (f.action.rfind("velvet:search", 0) == 0) {
        for (size_t i = 0; i < doc.fields.size(); ++i) {
            if (doc.fields[i].form == form && doc.fields[i].type == "text") return search_url(values[i]);
        }
        return "https://duckduckgo.com/";
    }
    std::string q;
    auto add = [&](const std::string& k, const std::string& v) {
        if (k.empty()) return;
        q += (q.empty() ? "" : "&") + url_encode(k) + "=" + url_encode(v);
    };
    for (const auto& [k, v] : f.hidden) add(k, v);
    for (size_t i = 0; i < doc.fields.size(); ++i) {
        if (doc.fields[i].form == form && doc.fields[i].type == "text") add(doc.fields[i].name, values[i]);
    }
    std::string action = f.action.substr(0, f.action.find('#'));
    return action + (action.find('?') == std::string::npos ? "?" : "&") + q;  // POST forms are sent as GET
}

namespace {
std::string json_string(const std::string& s, size_t& i) {  // s[i] is the opening quote
    std::string out;
    for (++i; i < s.size() && s[i] != '"'; ++i) {
        if (s[i] != '\\') {
            out += s[i];
            continue;
        }
        ++i;
        if (i >= s.size()) break;
        const char c = s[i];
        if (c == 'n') out += ' ';
        else if (c == 'u' && i + 4 < s.size()) {
            put_utf8(out, static_cast<uint32_t>(std::strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16)));
            i += 4;
        } else out += c;
    }
    return out;
}
std::string json_field(const std::string& s, size_t from, size_t to, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    size_t p = s.find(k, from);
    if (p == std::string::npos || p >= to) return {};
    p += k.size();
    while (p < s.size() && s[p] == ' ') ++p;
    if (p >= s.size() || s[p] != '"') return {};
    return json_string(s, p);
}
std::string html_escape(const std::string& t) {
    std::string o;
    for (char c : t) {
        if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '&') o += "&amp;";
        else if (c == '\'') o += "&#39;";
        else o += c;
    }
    return o;
}
}  // namespace

bool search_results_html(const std::string& url, const std::string& body, std::string& html) {
    const size_t qp = url.find("/w/rest.php/v1/search/page");
    if (qp == std::string::npos) return false;
    std::string query;
    const size_t q = url.find("&q=");
    if (q != std::string::npos) {
        for (size_t i = q + 3; i < url.size() && url[i] != '&'; ++i) {
            if (url[i] == '+') query += ' ';
            else if (url[i] == '%' && i + 2 < url.size()) {
                query += static_cast<char>(std::strtol(url.substr(i + 1, 2).c_str(), nullptr, 16));
                i += 2;
            } else query += url[i];
        }
    }
    html = "<title>Search: " + html_escape(query) + "</title><h1>" + html_escape(query) +
           "</h1><form action='velvet:search'><input name='q' value='" + html_escape(query) +
           "'><input type='submit' value='SEARCH'></form><p>Wikipedia results</p><ul>";
    size_t p = 0;
    int n = 0;
    while ((p = body.find("\"key\":", p)) != std::string::npos) {
        const size_t next = body.find("\"key\":", p + 6);
        const size_t end = next == std::string::npos ? body.size() : next;
        size_t kp = body.find('"', p + 6);
        const std::string key = kp < end ? json_string(body, kp) : "";
        const std::string title = json_field(body, p, end, "title");
        const std::string desc = json_field(body, p, end, "description");
        const std::string excerpt = json_field(body, p, end, "excerpt");  // already HTML
        if (!key.empty()) {
            html += "<li><a href='https://en.wikipedia.org/wiki/" + html_escape(key) + "'>" + html_escape(title.empty() ? key : title) +
                    "</a><br>" + (desc.empty() ? "" : "<b>" + html_escape(desc) + "</b> ") + excerpt + "...</li>";
            ++n;
        }
        p = end;
    }
    if (!n) html += "<li>No results.</li>";
    html += "</ul>";
    return true;
}

// ======================================================================== HTML -> blocks
namespace {
struct Builder {
    Doc doc;
    Block cur;
    int link = -1;
    int heading = 0, bold = 0, pre = 0, quote = 0;
    int form = -1;

    void flush(bool gap) {
        bool empty = true;
        for (const auto& s : cur.spans) {
            if (s.text.find_first_not_of(' ') != std::string::npos) empty = false;
        }
        if (!empty) {
            while (!cur.spans.empty() && !cur.pre && cur.spans.back().text.find_first_not_of(' ') == std::string::npos) cur.spans.pop_back();
            doc.blocks.push_back(cur);
        }
        cur = Block{};
        cur.gap_before = gap;
        cur.pre = pre > 0;
        cur.level = heading;
        if (quote) cur.prefix = "> ";
    }
    uint8_t style() const {
        if (link >= 0) return kLink;
        if (heading) return kHeading;
        if (pre) return kPre;
        if (quote) return kQuote;
        if (bold) return kBold;
        return kText;
    }
    void text(const std::string& raw) {
        std::string t = decode_entities(raw);
        if (!pre) {  // HTML whitespace: any run becomes one space, none at a line start
            std::string c;
            bool ws = false;
            for (char ch : t) {
                if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
                    ws = true;
                } else {
                    if (ws) c += ' ';
                    ws = false;
                    c += ch;
                }
            }
            if (ws) c += ' ';
            const bool at_start = cur.spans.empty() || (!cur.spans.back().text.empty() && cur.spans.back().text.back() == ' ');
            if (at_start && !c.empty() && c[0] == ' ') c.erase(0, 1);
            if (c.empty()) return;
            t = c;
        }
        if (!cur.spans.empty() && cur.spans.back().style == style() && cur.spans.back().link == link) {
            cur.spans.back().text += t;
        } else {
            cur.spans.push_back(Span{t, style(), link});
        }
    }
};

bool is_block(const std::string& n) {
    static const char* const kBlocks[] = {"p", "div", "section", "article", "header", "footer", "nav", "main", "aside",
                                          "ul", "ol", "li", "h1", "h2", "h3", "h4", "h5", "h6", "tr", "table", "blockquote",
                                          "pre", "hr", "form", "dl", "dt", "dd", "figure", "figcaption", "center", "details",
                                          "summary", "address", "fieldset", "caption", "tbody", "thead"};
    for (const char* b : kBlocks) {
        if (n == b) return true;
    }
    return false;
}
}  // namespace

Doc parse_html(const std::string& html, const std::string& base_url) {
    Builder b;
    b.doc.url = base_url;
    std::string base = base_url;
    b.cur.level = 0;
    size_t i = 0;
    const size_t n = html.size();
    while (i < n) {
        if (html[i] != '<') {
            const size_t e = html.find('<', i);
            b.text(html.substr(i, e == std::string::npos ? std::string::npos : e - i));
            i = e == std::string::npos ? n : e;
            continue;
        }
        if (html.compare(i, 4, "<!--") == 0) {
            const size_t e = html.find("-->", i + 4);
            i = e == std::string::npos ? n : e + 3;
            continue;
        }
        // the tag ends at the first '>' that isn't inside a quoted attribute
        size_t e = i + 1;
        char quote = 0;
        for (; e < n; ++e) {
            const char c = html[e];
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                if (e > 0 && html[e - 1] == '=') quote = c;
            } else if (c == '>') {
                break;
            }
        }
        if (e >= n) break;
        const std::string tag = html.substr(i + 1, e - i - 1);
        i = e + 1;
        if (tag.empty() || tag[0] == '!' || tag[0] == '?') continue;
        const bool closing = tag[0] == '/';
        size_t s = closing ? 1 : 0;
        size_t k = s;
        while (k < tag.size() && (std::isalnum(static_cast<unsigned char>(tag[k])))) ++k;
        const std::string name = lower(tag.substr(s, k - s));

        // skip whole elements we don't show
        if (!closing && (name == "script" || name == "style" || name == "noscript" || name == "svg" || name == "template" ||
                         name == "iframe" || name == "select" || name == "textarea" || name == "button" || name == "nav")) {
            const std::string end = "</" + name;
            size_t p = i;
            while (true) {
                p = html.find("</", p);
                if (p == std::string::npos) break;
                if (lower(html.substr(p, end.size())) == end) break;
                p += 2;
            }
            if (name == "button" || name == "textarea") {
                // a button submits its form; anything else shows its label quietly
                if (p != std::string::npos) {
                    std::string inner = html.substr(i, p - i);
                    std::string plain;
                    bool in_tag = false;
                    for (char c : inner) {
                        if (c == '<') in_tag = true;
                        else if (c == '>') in_tag = false;
                        else if (!in_tag) plain += c;
                    }
                    std::string label = decode_entities(plain);
                    label.erase(0, label.find_first_not_of(" \n\t\r"));
                    while (!label.empty() && std::isspace(static_cast<unsigned char>(label.back()))) label.pop_back();
                    const std::string btype = lower(attr(tag, "type"));
                    if (b.form >= 0 && (name == "textarea" || btype.empty() || btype == "submit")) {
                        Field f;
                        f.form = b.form;
                        f.name = attr(tag, "name");
                        f.type = name == "textarea" ? "text" : "submit";
                        f.value = name == "textarea" ? label : (label.empty() ? "Go" : label);
                        f.placeholder = attr(tag, "placeholder");
                        b.doc.fields.push_back(f);
                        Span sp{f.type == "submit" ? f.value : f.placeholder, f.type == "submit" ? kButton : kInput, -1};
                        sp.field = static_cast<int>(b.doc.fields.size()) - 1;
                        b.cur.spans.push_back(sp);
                    } else if (!label.empty()) {
                        b.cur.spans.push_back(Span{"[" + label + "]", kDim, -1});
                    }
                }
            }
            const size_t q = p == std::string::npos ? n : html.find('>', p);
            i = q == std::string::npos ? n : q + 1;
            continue;
        }
        if (!closing && name == "title") {
            const size_t p = lower(html.substr(i, 2000)).find("</title");
            if (p != std::string::npos) {
                std::string t = decode_entities(html.substr(i, p));
                std::string c;
                for (char ch : t) {
                    if (ch == '\n' || ch == '\t' || ch == '\r') ch = ' ';
                    if (!(ch == ' ' && (c.empty() || c.back() == ' '))) c += ch;
                }
                b.doc.title = c;
                i += p;
            }
            continue;
        }
        if (!closing && name == "base") {
            const std::string h = attr(tag, "href");
            if (!h.empty()) base = resolve_url(base_url, h);
            continue;
        }
        if (name == "head") continue;

        if (name == "br") {
            const bool keep_pre = b.pre > 0;
            if (keep_pre) b.text("\n");
            else b.flush(false);
            continue;
        }
        if (name == "hr" && !closing) {
            b.flush(true);
            b.cur.spans.push_back(Span{"────────────", kRule, -1});
            b.flush(true);
            continue;
        }
        if (name == "img" && !closing) {
            const std::string alt = attr(tag, "alt");
            const std::string src = attr(tag, "src");
            const std::string ls = lower(src);
            if (src.empty() || ls.rfind("data:", 0) == 0 || ls.find(".svg") != std::string::npos) {
                if (!alt.empty()) b.cur.spans.push_back(Span{"[" + alt + "]", kDim, b.link});
                continue;
            }
            Img im;
            im.url = resolve_url(base, src);
            im.alt = alt;
            im.w = std::atoi(attr(tag, "width").c_str());
            im.h = std::atoi(attr(tag, "height").c_str());
            b.doc.images.push_back(im);
            Span sp{alt, kImage, b.link};
            sp.img = static_cast<int>(b.doc.images.size()) - 1;
            b.cur.spans.push_back(sp);
            continue;
        }
        if (name == "form") {
            if (closing) {
                b.form = -1;
            } else {
                Form f;
                const std::string action = attr(tag, "action");
                f.action = action.empty() ? b.doc.url : resolve_url(base, action);
                f.method = lower(attr(tag, "method"));
                b.doc.forms.push_back(f);
                b.form = static_cast<int>(b.doc.forms.size()) - 1;
            }
            continue;
        }
        if (name == "input" && !closing) {
            const std::string type = lower(attr(tag, "type"));
            const std::string fname = attr(tag, "name"), value = attr(tag, "value");
            if (type == "hidden") {
                if (b.form >= 0 && !fname.empty()) b.doc.forms[b.form].hidden.push_back({fname, value});
                continue;
            }
            if (type == "checkbox" || type == "radio" || type == "file" || type == "range" || type == "color") continue;
            Field f;
            f.form = b.form;
            f.name = fname;
            f.value = value;
            f.placeholder = attr(tag, "placeholder");
            f.type = (type == "submit" || type == "button" || type == "image" || type == "reset") ? "submit" : "text";
            if (f.type == "submit" && f.value.empty()) f.value = type == "image" ? attr(tag, "alt") : "Submit";
            if (f.type == "submit" && f.value.empty()) f.value = "Go";
            b.doc.fields.push_back(f);
            Span sp{f.type == "submit" ? f.value : (f.value.empty() ? f.placeholder : f.value), f.type == "submit" ? kButton : kInput, -1};
            sp.field = static_cast<int>(b.doc.fields.size()) - 1;
            b.cur.spans.push_back(sp);
            continue;
        }
        if (name == "a") {
            if (closing) {
                b.link = -1;
            } else {
                const std::string href = attr(tag, "href");
                if (!href.empty() && href.rfind("javascript:", 0) != 0) {
                    b.doc.links.push_back(resolve_url(base, href));
                    b.link = static_cast<int>(b.doc.links.size()) - 1;
                }
            }
            continue;
        }
        if (name == "b" || name == "strong") {
            b.bold += closing ? -1 : 1;
            b.bold = std::max(0, b.bold);
            continue;
        }
        if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
            b.flush(true);
            b.heading = closing ? 0 : name[1] - '0';
            b.cur.level = b.heading;
            continue;
        }
        if (name == "pre") {
            b.flush(true);
            b.pre += closing ? -1 : 1;
            b.pre = std::max(0, b.pre);
            b.cur.pre = b.pre > 0;
            continue;
        }
        if (name == "blockquote") {
            b.quote += closing ? -1 : 1;
            b.quote = std::max(0, b.quote);
            b.flush(true);
            continue;
        }
        if (name == "li" && !closing) {
            b.flush(false);
            b.cur.prefix = "• ";
            continue;
        }
        if (name == "td" || name == "th") {
            if (!closing) b.text(" ");
            continue;
        }
        if (is_block(name)) {
            const bool gap = name == "p" || name == "ul" || name == "ol" || name == "table" || name == "section" ||
                             name == "article" || name == "figure" || name == "dl";
            b.flush(gap || closing ? gap : false);
        }
    }
    b.flush(false);
    if (b.doc.title.empty()) b.doc.title = base_url;
    return b.doc;
}

Doc parse_text(const std::string& text, const std::string& url) {
    Doc d;
    d.url = url;
    d.title = url.substr(url.rfind('/') + 1);
    size_t i = 0;
    while (i <= text.size()) {
        const size_t e = text.find('\n', i);
        Block b;
        b.pre = true;
        b.gap_before = false;
        std::string line = text.substr(i, e == std::string::npos ? std::string::npos : e - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        b.spans.push_back(Span{line.empty() ? " " : line, kPre, -1});
        d.blocks.push_back(b);
        if (e == std::string::npos) break;
        i = e + 1;
    }
    return d;
}

// ======================================================================== layout
std::vector<Line> layout_doc(const Doc& doc, int cols) {
    std::vector<Line> out;
    auto blank = [&] {
        if (!out.empty() && !out.back().spans.empty()) out.push_back(Line{});
    };
    for (const Block& blk : doc.blocks) {
        if (blk.gap_before) blank();
        const int indent = static_cast<int>(utf8_len(blk.prefix));
        Line line;
        int col = 0;
        if (!blk.prefix.empty()) {
            line.spans.push_back(Span{blk.prefix, kDim, -1});
            col = indent;
        }
        auto new_line = [&] {
            out.push_back(line);
            line = Line{};
            col = 0;
            if (indent) {
                line.spans.push_back(Span{std::string(indent, ' '), kText, -1});
                col = indent;
            }
        };
        auto add = [&](const std::string& word, const Span& like) {
            if (!line.spans.empty() && line.spans.back().style == like.style && line.spans.back().link == like.link) {
                line.spans.back().text += word;
            } else {
                Span s = like;
                s.text = word;
                line.spans.push_back(s);
            }
            col += static_cast<int>(utf8_len(word));
        };
        for (const Span& sp : blk.spans) {
            if (blk.pre) {
                // keep spacing; break at newlines and the screen edge
                std::string chunk;
                for (size_t i = 0; i < sp.text.size(); ++i) {
                    const char c = sp.text[i];
                    if (c == '\n') {
                        add(chunk, sp);
                        chunk.clear();
                        new_line();
                        continue;
                    }
                    chunk += c;
                    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80 && col + static_cast<int>(utf8_len(chunk)) >= cols) {
                        add(chunk, sp);
                        chunk.clear();
                        new_line();
                    }
                }
                add(chunk, sp);
                continue;
            }
            // word wrap
            size_t i = 0;
            const std::string& t = sp.text;
            while (i < t.size()) {
                const bool space = t[i] == ' ';
                size_t j = i;
                while (j < t.size() && (t[j] == ' ') == space) ++j;
                std::string word = t.substr(i, j - i);
                const int w = static_cast<int>(utf8_len(word));
                if (space) {
                    if (col > indent && col + 1 <= cols) add(" ", sp);
                } else {
                    if (col + w > cols && col > indent) {
                        while (!line.spans.empty() && !line.spans.back().text.empty() && line.spans.back().text.back() == ' ') {
                            line.spans.back().text.pop_back();
                            --col;
                        }
                        new_line();
                    }
                    while (static_cast<int>(utf8_len(word)) > cols - col) {  // a word wider than the screen
                        size_t cut = 0, chars = 0;
                        while (cut < word.size() && static_cast<int>(chars) < cols - col) {
                            ++cut;
                            while (cut < word.size() && (static_cast<unsigned char>(word[cut]) & 0xC0) == 0x80) ++cut;
                            ++chars;
                        }
                        add(word.substr(0, cut), sp);
                        word = word.substr(cut);
                        new_line();
                    }
                    add(word, sp);
                }
                i = j;
            }
        }
        out.push_back(line);
    }
    while (!out.empty() && out.back().spans.empty()) out.pop_back();
    return out;
}

// ======================================================================== rendered layout
namespace {
// The pixel fonts are ASCII: accented Latin letters fold to their plain letter (n for n-tilde,
// ss for sharp s), anything else the font lacks becomes '?'.
std::string font_safe(const lv_font_t* f, const std::string& s) {
    static const char kLat1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";   // U+00C0..U+00FF
    static const char kLatA[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";  // U+0100..U+017F
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        const size_t start = i;
        uint32_t cp = static_cast<unsigned char>(s[i]);
        int len = 1;
        if (cp >= 0xF0) { cp &= 0x07; len = 4; }
        else if (cp >= 0xE0) { cp &= 0x0F; len = 3; }
        else if (cp >= 0xC0) { cp &= 0x1F; len = 2; }
        for (int k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += len;
        lv_font_glyph_dsc_t g{};
        if (cp < 0x80 || lv_font_get_glyph_dsc(f, &g, cp, 0)) {
            out.append(s, start, i - start);
        } else if (cp == 0xDF) {
            out += "ss";
        } else if (cp >= 0xC0 && cp <= 0xFF) {
            out += kLat1[cp - 0xC0];
        } else if (cp >= 0x100 && cp <= 0x17F) {
            out += kLatA[cp - 0x100];
        } else if (cp == 0x2018 || cp == 0x2019) {
            out += '\'';
        } else if (cp == 0x201C || cp == 0x201D) {
            out += '"';
        } else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) {
            out += '-';
        } else if (cp == 0x2026) {
            out += "...";
        } else if (cp == 0x200B || cp == 0x200E || cp == 0x200F || cp == 0xFEFF || (cp >= 0x300 && cp <= 0x36F)) {
            // invisible marks and combining accents: drop
        } else {
            out += '?';
        }
    }
    return out;
}
}  // namespace

namespace {
uint32_t next_cp(const std::string& s, size_t& i) {
    uint32_t cp = static_cast<unsigned char>(s[i]);
    int len = 1;
    if (cp >= 0xF0) { cp &= 0x07; len = 4; }
    else if (cp >= 0xE0) { cp &= 0x0F; len = 3; }
    else if (cp >= 0xC0) { cp &= 0x1F; len = 2; }
    for (int k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += len;
    return cp;
}
int32_t text_w(const lv_font_t* f, const std::string& s) {
    int32_t w = 0;
    size_t i = 0;
    while (i < s.size()) {
        const uint32_t cp = next_cp(s, i);
        size_t j = i;
        const uint32_t nx = j < s.size() ? next_cp(s, j) : 0;
        w += lv_font_get_glyph_width(f, cp, nx);
    }
    return w;
}
}  // namespace

PageLayout layout_page(const Doc& doc, int32_t width, const PageFonts& f, const std::vector<std::pair<int, int>>& sizes) {
    PageLayout L;
    L.width = width;
    constexpr int32_t kMargin = 10;
    const int32_t maxx = width - kMargin;
    int32_t y = 8;
    bool first = true;
    for (const Block& blk : doc.blocks) {
        const lv_font_t* font = blk.pre ? f.mono : blk.level == 1 ? f.h1 : blk.level == 2 ? f.h2 : blk.level >= 3 ? f.h3 : f.body;
        const int32_t lh = font->line_height + (blk.level ? 4 : 2);
        if (blk.gap_before && !first) y += blk.level ? 10 : 8;
        first = false;
        int32_t indent = 0;
        const size_t block_start = L.boxes.size();
        if (blk.prefix.rfind("•", 0) == 0) {
            indent = 16;
            PBox b;
            b.kind = kBoxBullet;
            b.x = kMargin + 5;
            b.y = y + lh / 2 - 2;
            b.w = b.h = 4;
            L.boxes.push_back(b);
        } else if (blk.prefix == "> ") {
            indent = 12;
        }
        const int32_t left = kMargin + indent;
        int32_t x = left, line_y = y, line_h = lh;
        const int32_t start_y = y;
        PBox run;
        bool has_run = false;
        auto flush_run = [&] {
            if (has_run && !run.text.empty()) L.boxes.push_back(run);
            has_run = false;
        };
        auto start_run = [&](const Span& sp) {
            run = PBox{};
            run.kind = kBoxText;
            run.style = sp.style;
            run.link = sp.link;
            run.level = blk.level;
            run.font = font;
            run.x = x;
            run.y = line_y;
            run.h = lh;
            has_run = true;
        };
        auto new_line = [&] {
            flush_run();
            y = line_y + line_h;
            line_y = y;
            line_h = lh;
            x = left;
        };
        for (const Span& sp : blk.spans) {
            if (sp.style == kImage) {
                flush_run();
                int w = 0, h = 0;
                if (sp.img >= 0 && sp.img < static_cast<int>(sizes.size()) && sizes[sp.img].first > 0) {
                    w = sizes[sp.img].first;
                    h = sizes[sp.img].second;
                } else if (sp.img >= 0 && doc.images[sp.img].w > 0 && doc.images[sp.img].h > 0) {
                    w = doc.images[sp.img].w;
                    h = doc.images[sp.img].h;
                } else {
                    w = 120;
                    h = 68;  // not loaded yet: a placeholder
                }
                const int32_t avail = maxx - left;
                if (w > avail) {
                    h = h * avail / w;
                    w = avail;
                }
                if (x > left) new_line();
                PBox b;
                b.kind = kBoxImage;
                b.img = sp.img;
                b.link = sp.link;
                b.text = sp.text;
                b.x = x;
                b.y = line_y;
                b.w = w;
                b.h = h;
                L.boxes.push_back(b);
                y = line_y + h + 4;
                line_y = y;
                line_h = lh;
                x = left;
                continue;
            }
            if (sp.style == kInput || sp.style == kButton) {
                flush_run();
                const bool button = sp.style == kButton;
                const int32_t w = button ? text_w(f.body, sp.text) + 18 : std::min<int32_t>(240, maxx - left);
                if (x + w > maxx && x > left) new_line();
                PBox b;
                b.kind = button ? kBoxButton : kBoxInput;
                b.field = sp.field;
                b.text = font_safe(f.body, sp.text);
                b.font = f.body;
                b.x = x;
                b.y = line_y;
                b.w = w;
                b.h = f.body->line_height + 6;
                L.boxes.push_back(b);
                line_h = std::max(line_h, b.h + 4);
                x += w + 6;
                continue;
            }
            const std::string t = font_safe(font, sp.text);
            if (blk.pre) {  // keep spacing, break only at newlines; may run past the page width
                size_t i = 0;
                while (i <= t.size()) {
                    const size_t e = t.find('\n', i);
                    const std::string seg = t.substr(i, e == std::string::npos ? std::string::npos : e - i);
                    if (!has_run || run.style != sp.style || run.link != sp.link) {
                        flush_run();
                        start_run(sp);
                    }
                    run.text += seg;
                    run.w += text_w(font, seg);
                    x += text_w(font, seg);
                    L.width = std::max(L.width, x + kMargin);
                    if (e == std::string::npos) break;
                    new_line();
                    i = e + 1;
                }
                continue;
            }
            size_t i = 0;
            while (i < t.size()) {
                const bool space = t[i] == ' ';
                size_t j = i;
                while (j < t.size() && (t[j] == ' ') == space) ++j;
                std::string word = space ? " " : t.substr(i, j - i);
                i = j;
                const int32_t w = text_w(font, word);
                if (space) {
                    if (x == left) continue;
                } else if (x + w > maxx && x > left) {
                    if (has_run) {
                        while (!run.text.empty() && run.text.back() == ' ') {
                            run.text.pop_back();
                            run.w -= text_w(font, " ");
                        }
                    }
                    new_line();
                }
                if (!space && w > maxx - left) {  // longer than a whole line: break it
                    std::string part;
                    size_t k = 0;
                    while (k < word.size()) {
                        size_t kk = k;
                        next_cp(word, kk);
                        const std::string ch = word.substr(k, kk - k);
                        if (x + text_w(font, part + ch) > maxx && !part.empty()) {
                            if (!has_run || run.style != sp.style || run.link != sp.link) {
                                flush_run();
                                start_run(sp);
                            }
                            run.text += part;
                            run.w += text_w(font, part);
                            new_line();
                            part.clear();
                        }
                        part += ch;
                        k = kk;
                    }
                    word = part;
                }
                if (!has_run || run.style != sp.style || run.link != sp.link) {
                    flush_run();
                    start_run(sp);
                }
                const int32_t ww = text_w(font, word);
                run.text += word;
                run.w += ww;
                x += ww;
            }
        }
        flush_run();
        y = line_y + line_h;
        if (blk.pre) {
            PBox bg;
            bg.kind = kBoxPreBg;
            bg.x = kMargin - 3;
            bg.y = start_y - 2;
            bg.w = std::max(maxx - kMargin + 6, L.width - 2 * kMargin + 6);
            bg.h = y - start_y + 4;
            L.boxes.insert(L.boxes.begin() + static_cast<long>(block_start), bg);
            y += 4;
        } else if (blk.prefix == "> ") {
            PBox bar;
            bar.kind = kBoxQuoteBar;
            bar.x = kMargin + 2;
            bar.y = start_y;
            bar.w = 3;
            bar.h = y - start_y;
            L.boxes.insert(L.boxes.begin() + static_cast<long>(block_start), bar);
        }
        bool rule = false;
        for (const Span& sp : blk.spans) rule |= sp.style == kRule;
        if (rule) {  // <hr>: a real line instead of the text one
            L.boxes.resize(block_start);
            PBox r;
            r.kind = kBoxRule;
            r.x = kMargin;
            r.y = start_y + lh / 2;
            r.w = maxx - kMargin;
            r.h = 1;
            L.boxes.push_back(r);
        }
    }
    L.height = y + 12;
    return L;
}

// ======================================================================== images
lv_draw_buf_t* decode_image(const std::string& bytes, int max_w, int max_h) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()),
                                              &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return nullptr;
    }
    int tw = std::min(w, max_w), th = static_cast<int>(static_cast<int64_t>(h) * tw / w);
    if (th > max_h) {
        th = max_h;
        tw = static_cast<int>(static_cast<int64_t>(w) * th / h);
    }
    tw = std::max(tw, 1);
    th = std::max(th, 1);
    lv_draw_buf_t* buf = lv_draw_buf_create(static_cast<uint32_t>(tw), static_cast<uint32_t>(th), LV_COLOR_FORMAT_ARGB8888, 0);
    if (!buf) {
        stbi_image_free(px);
        return nullptr;
    }
    const uint32_t stride = buf->header.stride / 4;
    auto* out = reinterpret_cast<uint32_t*>(buf->data);
    for (int y = 0; y < th; ++y) {
        const int y0 = static_cast<int>(static_cast<int64_t>(y) * h / th), y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * h / th));
        for (int x = 0; x < tw; ++x) {
            const int x0 = static_cast<int>(static_cast<int64_t>(x) * w / tw), x1 = std::max(x0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * w / tw));
            uint32_t r = 0, g = 0, b = 0, a = 0, cnt = 0;  // average the source pixels under this one
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    const unsigned char* p = px + (static_cast<size_t>(sy) * w + sx) * 4;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    a += p[3];
                    ++cnt;
                }
            }
            out[y * stride + x] = ((a / cnt) << 24) | ((r / cnt) << 16) | ((g / cnt) << 8) | (b / cnt);
        }
    }
    stbi_image_free(px);
    return buf;
}

// ======================================================================== built-in pages
bool builtin_page(const std::string& url, std::string& html) {
    if (url == "velvet:help") {
        html =
            "<title>Browser keys</title><h1>KEYS</h1><h2>Reading</h2><ul>"
            "<li>F / X: scroll up / down</li><li>Z / C: previous / next link</li><li>OK: open the link</li>"
            "<li>SPACE or PgDn: next screen, PgUp: previous</li><li>ESC: back, hold ESC: leave the browser</li>"
            "<li>R: reload, T: tabs, N: new tab, G or /: type an address</li></ul>"
            "<h2>Toolbar (TAB)</h2><ul><li>Type an address or words, OK to go</li>"
            "<li>fn + up/down: address bar or buttons, fn + left/right: pick a button</li>"
            "<li>TAB or ESC: hide it again</li></ul><p><a href='https://duckduckgo.com/'>DuckDuckGo</a></p>";
        return true;
    }
    return false;
}

// ======================================================================== fetching
namespace {
#ifdef __EMSCRIPTEN__
// Browser demo: fetch() rules apply, so only sites that allow other pages to read them load.
// Wikipedia articles go through Wikipedia's open REST API, which does.
class WebFetcher final : public Fetcher {
public:
    void get(const std::string& url, Done cb) override {
        std::string real = url;
        const std::string wiki = "wikipedia.org/wiki/";
        const size_t w = url.find(wiki);
        if (w != std::string::npos && url.find("Special:Random") == std::string::npos) {
            const size_t host_start = url.find("://") + 3;
            const std::string host = url.substr(host_start, w + 13 - host_start);
            real = "https://" + host + "/api/rest_v1/page/html/" + url.substr(w + wiki.size());
        } else if (w != std::string::npos) {
            real = "https://en.wikipedia.org/api/rest_v1/page/random/html";
        }
        auto* req = new Req{this, gen_, url, std::move(cb)};
        emscripten_fetch_attr_t a;
        emscripten_fetch_attr_init(&a);
        std::strcpy(a.requestMethod, "GET");
        a.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
        a.userData = req;
        a.onsuccess = [](emscripten_fetch_t* f) { finish(f, true); };
        a.onerror = [](emscripten_fetch_t* f) { finish(f, false); };
        emscripten_fetch(&a, real.c_str());
    }
    void cancel_all() override { ++gen_; }

private:
    struct Req {
        WebFetcher* self;
        uint32_t gen;
        std::string url;
        Done cb;
    };
    static void finish(emscripten_fetch_t* f, bool ok) {
        auto* r = static_cast<Req*>(f->userData);
        if (r->gen == r->self->gen_) {
            std::string body(f->data ? f->data : "", f->data ? static_cast<size_t>(f->numBytes) : 0);
            if (ok && f->status >= 200 && f->status < 400) {
                std::string type = "text/html";
                // the real Content-Type, from the response headers
                const size_t hl = emscripten_fetch_get_response_headers_length(f);
                if (hl) {
                    std::string hdr(hl + 1, '\0');
                    emscripten_fetch_get_response_headers(f, hdr.data(), hl + 1);
                    const std::string low = lower(hdr);
                    const size_t c = low.find("content-type:");
                    if (c != std::string::npos) {
                        size_t v = c + 13;
                        while (v < low.size() && low[v] == ' ') ++v;
                        type = low.substr(v, low.find_first_of("\r\n", v) - v);
                    }
                }
                if (r->url.find("/w/rest.php/") != std::string::npos) type = "application/json";
                r->cb(true, r->url, type, body, "");
            } else {
                r->cb(false, r->url, "", "",
                      f->status == 0 ? "This site doesn't allow other pages to read it (CORS), so the browser demo "
                                       "can't load it. On the CardputerZero it loads normally."
                                     : "HTTP " + std::to_string(f->status));
            }
        }
        delete r;
        emscripten_fetch_close(f);
    }
    uint32_t gen_{0};
};
#else
// CardputerZero / PC: curl in worker threads (up to 4 at once); answers come back through an lv_timer.
class CurlFetcher final : public Fetcher {
public:
    CurlFetcher() { timer_ = lv_timer_create(poll_cb, 30, this); }
    ~CurlFetcher() override {
        lv_timer_delete(timer_);
        std::lock_guard<std::mutex> lock(st_->mu);
        st_->alive = false;
        st_->queue.clear();
    }
    void get(const std::string& url, Done cb) override {
        const uint32_t id = ++next_id_;
        cbs_.push_back({id, gen_, std::move(cb)});
        {
            std::lock_guard<std::mutex> lock(st_->mu);
            st_->queue.push_back({id, url});
        }
        pump(st_);
    }
    void cancel_all() override {
        ++gen_;
        cbs_.clear();
        std::lock_guard<std::mutex> lock(st_->mu);
        st_->queue.clear();
    }

private:
    struct Result {
        uint32_t id{0};
        bool ok{false};
        std::string url, type, body, err;
    };
    struct State {
        std::mutex mu;
        bool alive{true};
        int running{0};
        std::vector<std::pair<uint32_t, std::string>> queue;
        std::vector<Result> done;
    };
    struct Pending {
        uint32_t id, gen;
        Done cb;
    };

    // starts queued requests while fewer than 4 run
    static void pump(const std::shared_ptr<State>& st) {
        std::lock_guard<std::mutex> lock(st->mu);
        while (st->alive && st->running < 4 && !st->queue.empty()) {
            auto job = st->queue.front();
            st->queue.erase(st->queue.begin());
            ++st->running;
            std::thread([st, job] {
                Result r = fetch(job.second);
                r.id = job.first;
                {
                    std::lock_guard<std::mutex> l2(st->mu);
                    --st->running;
                    if (st->alive) st->done.push_back(std::move(r));
                }
                pump(st);
            }).detach();
        }
    }

    static Result fetch(const std::string& url) {
        Result r;
        r.url = url;
        const std::string proxy = tor_proxy();
        char path[] = "/tmp/velvet-page-XXXXXX";
        const int tmp = mkstemp(path);
        if (tmp < 0) {
            r.err = "no temp file";
            return r;
        }
        close(tmp);
        int out[2];
        if (pipe(out) != 0) {
            unlink(path);
            r.err = "pipe failed";
            return r;
        }
        const pid_t pid = fork();
        if (pid == 0) {
            dup2(out[1], 1);
            const int devnull = open("/dev/null", O_RDWR);
            dup2(devnull, 2);
            dup2(devnull, 0);
            close(out[0]);
            // SYSTEM's TOR: through its SOCKS proxy, names resolved on the far side (proxy is
            // read before the fork; tor_proxy() is only a flag)
            if (!proxy.empty())
                execlp("curl", "curl", "-sSL", "--compressed", "--max-time", "60", "--max-filesize", "6000000",
                       "--proxy", proxy.c_str(),
                       "-A", "Mozilla/5.0 (Linux; CardputerZero) Velvet/1.0", "-o", path, "-w",
                       "%{url_effective}\n%{content_type}\n%{http_code}", "--", url.c_str(), static_cast<char*>(nullptr));
            execlp("curl", "curl", "-sSL", "--compressed", "--max-time", "25", "--max-filesize", "6000000",
                   "-A", "Mozilla/5.0 (Linux; CardputerZero) Velvet/1.0", "-o", path, "-w",
                   "%{url_effective}\n%{content_type}\n%{http_code}", "--", url.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        close(out[1]);
        std::string meta;
        char buf[512];
        ssize_t n;
        while ((n = read(out[0], buf, sizeof buf)) > 0) meta.append(buf, static_cast<size_t>(n));
        close(out[0]);
        int st = 0;
        if (pid > 0) waitpid(pid, &st, 0);
        const int code = pid > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        const size_t a = meta.find('\n'), b = meta.find('\n', a + 1);
        if (a != std::string::npos && b != std::string::npos) {
            r.url = meta.substr(0, a);
            r.type = lower(meta.substr(a + 1, b - a - 1));
            const int http = std::atoi(meta.c_str() + b + 1);
            if (code == 0 && http >= 200 && http < 400) r.ok = true;
            else r.err = code == 127 ? "curl is not installed" : code != 0 ? "can't connect (curl error " + std::to_string(code) + ")" : "HTTP " + std::to_string(http);
        } else {
            r.err = code == 127 ? "curl is not installed" : "can't connect";
        }
        if (FILE* f = std::fopen(path, "rb")) {
            char chunk[16384];
            size_t got;
            while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0 && r.body.size() < 6000000) r.body.append(chunk, got);
            std::fclose(f);
        }
        unlink(path);
        return r;
    }

    static void poll_cb(lv_timer_t* t) {
        auto* self = static_cast<CurlFetcher*>(lv_timer_get_user_data(t));
        std::vector<Result> done;
        {
            std::lock_guard<std::mutex> lock(self->st_->mu);
            done.swap(self->st_->done);
        }
        for (auto& r : done) {
            auto it = std::find_if(self->cbs_.begin(), self->cbs_.end(), [&](const Pending& p) { return p.id == r.id; });
            if (it == self->cbs_.end()) continue;
            Pending p = std::move(*it);
            self->cbs_.erase(it);
            if (p.gen == self->gen_) p.cb(r.ok, r.url, r.type, r.body, r.err);  // may start new requests
        }
    }

    std::shared_ptr<State> st_ = std::make_shared<State>();
    lv_timer_t* timer_{};
    uint32_t gen_{0}, next_id_{0};
    std::vector<Pending> cbs_;
};
#endif
}  // namespace

std::unique_ptr<Fetcher> make_fetcher() {
#ifdef __EMSCRIPTEN__
    return std::make_unique<WebFetcher>();
#else
    return std::make_unique<CurlFetcher>();
#endif
}

std::string font_safe_text(const lv_font_t* f, const std::string& s) { return font_safe(f, s); }

}  // namespace velvet
