#include "wordmark.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

namespace wordmark {
namespace {

// ---- timing: CSS-style cubic-bezier easing and keyframe tracks (as Anim.kt) ----
struct Bez {
    float x1, y1, x2, y2;
    static float curve(float t, float a, float b) {
        const float u = 1 - t;
        return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
    }
    float at(float x) const {
        if (x <= 0) return 0;
        if (x >= 1) return 1;
        float lo = 0, hi = 1, t = .5f;
        for (int i = 0; i < 22; ++i) {
            if (curve(t, x1, x2) < x) lo = t; else hi = t;
            t = (lo + hi) / 2;
        }
        return curve(t, y1, y2);
    }
};

struct K {
    float t;
    float v[5];
    const Bez* ease;
    K(float pct, std::initializer_list<float> vals, const Bez* e = nullptr) : t(pct / 100.f), v{}, ease(e) {
        int i = 0;
        for (float f : vals) v[i++] = f;
    }
};

struct Track {
    const Bez* ease;
    std::vector<K> keys;
    int n;  // values per key
    void at(float p, float* out) const {
        if (p <= keys.front().t) { std::copy(keys.front().v, keys.front().v + n, out); return; }
        for (size_t i = 0; i + 1 < keys.size(); ++i) {
            const K& a = keys[i];
            const K& b = keys[i + 1];
            if (p > b.t) continue;
            const float span = b.t - a.t;
            const float e = span <= 0 ? 1 : (a.ease ? a.ease : ease)->at((p - a.t) / span);
            for (int j = 0; j < n; ++j) out[j] = a.v[j] + (b.v[j] - a.v[j]) * e;
            return;
        }
        std::copy(keys.back().v, keys.back().v + n, out);
    }
    float at1(float p) const { float v[5]; at(p, v); return v[0]; }
};

const Bez P{.2f, .8f, .2f, 1}, SHUT{.6f, 0, .9f, .4f}, GLIDE{.65f, 0, .35f, 1}, OUT{.16f, 1, .3f, 1},
    BACK{.7f, 0, .84f, 0}, LOCK{.22f, 1, .36f, 1}, UNLOCK{.64f, 0, .78f, 0}, EASE{.25f, .1f, .25f, 1},
    IN_OUT{.42f, 0, .58f, 1}, BEAM_GO{.5f, 0, .2f, 1};

struct Pt { float x, y; };
using Poly = std::vector<Pt>;
using Shape = std::vector<Poly>;  // several closed polygons, even-odd

Poly poly(std::initializer_list<float> xy) {
    Poly p;
    for (auto it = xy.begin(); it != xy.end(); it += 2) p.push_back({it[0], it[1]});
    return p;
}

Track T(const Bez& e, int n, std::initializer_list<K> k) { return Track{&e, std::vector<K>(k), n}; }

// tracks (same keyframes as WordmarkView.kt)
const Track RIG = T(EASE, 1, {K(0, {105}), K(16, {105}, &GLIDE), K(26, {0}), K(83, {0}, &GLIDE), K(91, {105}), K(100, {105})});

Track lid(float d) {
    return T(P, 5, {K(0, {0, 40 * d, .6f, 1, 0}), K(10, {0, 0, 1, 1, 0}), K(93.5f, {0, 0, 1, 1, 0}),
                    K(99, {0, 40 * d, 1.3f, 1, 0}), K(100, {0, 40 * d, 1.3f, 1, 0})});
}
const Track LID_OP = T(P, 1, {K(0, {0}), K(10, {1}), K(93.5f, {1}), K(99, {0}), K(100, {0})});

Track iris(float rot) {
    return T(P, 5, {K(0, {0, 0, 0, 0, rot}), K(6, {0, 0, 0, 0, rot}), K(16, {0, 0, 1, 1, 0}), K(60, {0, 0, 1, 1, 0}),
                    K(63.5f, {0, 0, .85f, .85f, 0}), K(67, {0, 0, 1, 1, 0}), K(91, {0, 0, 1, 1, 0}, &SHUT),
                    K(93.5f, {0, 0, 1, 0, 0}), K(94, {0, 0, 1, 0, 0}), K(100, {0, 0, 1, 0, 0})});
}
const Track IRIS_OP = T(P, 1, {K(0, {0}), K(6, {0}), K(16, {1}), K(91, {1}, &SHUT), K(93.5f, {1}), K(94, {0}), K(100, {0})});

struct Part { Poly poly; float ox, oy; Track tf, op; };
const Part EYE[] = {
    {poly({7.5f, 57.5f, 35, 30, 85, 30, 112.5f, 57.5f, 102.5f, 57.5f, 82.5f, 37.5f, 37.5f, 37.5f, 17.5f, 57.5f}), 60, 57.5f, lid(-1), LID_OP},
    {poly({17.5f, 62.5f, 37.5f, 82.5f, 82.5f, 82.5f, 102.5f, 62.5f, 112.5f, 62.5f, 85, 90, 35, 90, 7.5f, 62.5f}), 60, 62.5f, lid(1), LID_OP},
    {poly({40, 57.5f, 60, 37.5f, 80, 57.5f, 67.5f, 57.5f, 60, 50, 52.5f, 57.5f}), 60, 57.5f, iris(-90), IRIS_OP},
    {poly({52.5f, 62.5f, 60, 70, 67.5f, 62.5f, 80, 62.5f, 60, 82.5f, 40, 62.5f}), 60, 62.5f, iris(90), IRIS_OP},
};

const Shape O = {
    poly({140, 30, 160, 30, 170, 40, 170, 57.5f, 130, 57.5f, 130, 40}),
    poly({142.5f, 37.5f, 157.5f, 37.5f, 162.5f, 42.5f, 162.5f, 57.5f, 137.5f, 57.5f, 137.5f, 42.5f}),
    poly({170, 62.5f, 170, 80, 160, 90, 140, 90, 130, 80, 130, 62.5f}),
    poly({162.5f, 62.5f, 162.5f, 77.5f, 157.5f, 82.5f, 142.5f, 82.5f, 137.5f, 77.5f, 137.5f, 62.5f}),
};
const Shape PL = {
    poly({177.5f, 57.5f, 177.5f, 30, 210, 30, 217.5f, 37.5f, 217.5f, 57.5f}),
    poly({185, 57.5f, 185, 37.5f, 207.5f, 37.5f, 210, 40, 210, 57.5f}),
    poly({177.5f, 62.5f, 217.5f, 62.5f, 217.5f, 67.5f, 210, 75, 185, 75, 185, 90, 177.5f, 90}),
    poly({185, 62.5f, 210, 62.5f, 210, 65, 207.5f, 67.5f, 185, 67.5f}),
};
const Shape TL = {
    poly({225, 30, 262.5f, 30, 262.5f, 37.5f, 247.5f, 37.5f, 247.5f, 57.5f, 240, 57.5f, 240, 37.5f, 225, 37.5f}),
    poly({247.5f, 62.5f, 247.5f, 90, 240, 90, 240, 62.5f}),
};
const Shape IL = {poly({270, 57.5f, 270, 30, 277.5f, 30, 277.5f, 57.5f}), poly({270, 62.5f, 277.5f, 62.5f, 277.5f, 90, 270, 90})};
const Shape CL = {
    poly({295, 30, 325, 30, 325, 37.5f, 297.5f, 37.5f, 292.5f, 42.5f, 292.5f, 57.5f, 285, 57.5f, 285, 40}),
    poly({292.5f, 62.5f, 292.5f, 77.5f, 297.5f, 82.5f, 325, 82.5f, 325, 90, 295, 90, 285, 80, 285, 62.5f}),
};

Track slide(float a, float b, float c, float d) {
    return T(EASE, 1, {K(0, {-242.5f}), K(a, {-242.5f}, &OUT), K(b, {0}), K(c, {0}, &BACK), K(d, {-242.5f}), K(100, {-242.5f})});
}
const Track SUITE = slide(30, 45, 67.2f, 78.2f);
struct Letter { const Shape* shape; Track tr; };
const Letter LETTERS[] = {
    {&O, slide(29.2f, 44.2f, 68, 79)},     {&PL, slide(28.4f, 43.4f, 68.8f, 79.8f)},
    {&TL, slide(27.6f, 42.6f, 69.6f, 80.6f)}, {&IL, slide(26.8f, 41.8f, 70.4f, 81.4f)},
    {&CL, slide(26, 41, 71.2f, 82.2f)},
};

const Shape BEHIND = {poly({83.75f, 0, 83.75f, 30, 113.75f, 60, 83.75f, 90, 83.75f, 120, 350, 120, 350, 0})};
const Shape O_IN = {poly({142.5f, 37.5f, 157.5f, 37.5f, 162.5f, 42.5f, 162.5f, 77.5f, 157.5f, 82.5f, 142.5f, 82.5f, 137.5f, 77.5f, 137.5f, 42.5f})};
const Shape O_GAP = {poly({-999, -999, 9000, -999, 9000, 57.5f, -999, 57.5f}), poly({-999, 62.5f, 9000, 62.5f, 9000, 9000, -999, 9000})};

const Track RING = T(EASE, 2, {K(0, {2.6f, 0}), K(44, {2.6f, 0}, &LOCK), K(51, {1, 1}), K(64.5f, {1, 1}, &UNLOCK), K(67.5f, {2.2f, 0}), K(100, {2.2f, 0})});
Track grow(float start, float full) {
    return T(EASE, 1, {K(0, {0}), K(start, {0}, &LOCK), K(full, {1}), K(64.5f, {1}, &UNLOCK), K(67.5f, {0}), K(100, {0})});
}
struct Bar { float x, y, w, h; bool vertical; float ox, oy; Track tr; };
const Bar BARS[] = {
    {149.25f, 35, 1.5f, 25, true, 150, 35, grow(43.5f, 51)},  {149.25f, 60, 1.5f, 25, true, 150, 85, grow(43.5f, 51)},
    {135, 59.25f, 15, 1.5f, false, 135, 60, grow(43.5f, 51)}, {150, 59.25f, 15, 1.5f, false, 165, 60, grow(43.5f, 51)},
    {147.5f, 35, 5, 10, true, 150, 35, grow(43, 50)},         {147.5f, 75, 5, 10, true, 150, 85, grow(43, 50)},
};

const Poly BEAM = poly({-60, 60, -6, 58, 2, 60, -6, 62});
const Track BEAM_MOVE = T(EASE, 1, {K(0, {148}), K(52, {148}, &BEAM_GO), K(60, {329}), K(100, {329})});
const Track BEAM_STRETCH = T(IN_OUT, 2, {K(0, {.15f, 0}), K(51.5f, {.15f, 0}), K(52.5f, {.3f, 1}), K(56, {2.2f, 1}),
                                         K(59.5f, {.3f, 1}), K(61, {.15f, 0}), K(100, {.15f, 0})});

// ---- rasterizer: even-odd polygons, 4x4 samples per pixel, clipped by other shapes ----
constexpr int SS = 4;

struct Raster {
    float s, ox, oy;
    float* cov;
    int w, h;

    Pt px(Pt p) const { return {p.x * s + ox, p.y * s + oy}; }
    Shape map(const Shape& sh, float dx, float dy) const {
        Shape o;
        for (auto& pl : sh) {
            Poly q;
            for (auto& p : pl) q.push_back(px({p.x + dx, p.y + dy}));
            o.push_back(std::move(q));
        }
        return o;
    }

    // even-odd spans of [sh] on pixel row y (a sample height)
    static void spans(const Shape& sh, float y, std::vector<float>& out) {
        out.clear();
        for (auto& pl : sh) {
            for (size_t i = 0, n = pl.size(); i < n; ++i) {
                const Pt a = pl[i], b = pl[(i + 1) % n];
                if ((a.y <= y) == (b.y <= y)) continue;
                out.push_back(a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y));
            }
        }
        std::sort(out.begin(), out.end());
    }
    static void intersect(std::vector<float>& a, const std::vector<float>& b) {
        std::vector<float> r;
        size_t i = 0, j = 0;
        while (i + 1 < a.size() && j + 1 < b.size()) {
            const float lo = std::max(a[i], b[j]), hi = std::min(a[i + 1], b[j + 1]);
            if (lo < hi) { r.push_back(lo); r.push_back(hi); }
            if (a[i + 1] < b[j + 1]) i += 2; else j += 2;
        }
        a.swap(r);
    }

    // [grad] (optional) gives an extra alpha for a pixel x
    template <class G>
    void fill(const Shape& sh, float alpha, std::initializer_list<const Shape*> clips, G grad) {
        float y0 = 1e9f, y1 = -1e9f;
        for (auto& pl : sh) for (auto& p : pl) { y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
        const int r0 = std::max(0, static_cast<int>(std::floor(y0))), r1 = std::min(h - 1, static_cast<int>(std::ceil(y1)));
        std::vector<float> sp, cp;
        const float a = alpha / (SS * SS);
        for (int row = r0; row <= r1; ++row) {
            float* line = cov + row * w;
            for (int k = 0; k < SS; ++k) {
                const float y = row + (k + .5f) / SS;
                spans(sh, y, sp);
                for (const Shape* c : clips) { spans(*c, y, cp); intersect(sp, cp); }
                for (size_t i = 0; i + 1 < sp.size(); i += 2) {
                    int c0 = static_cast<int>(std::ceil(sp[i] * SS - .5f)), c1 = static_cast<int>(std::ceil(sp[i + 1] * SS - .5f));
                    c0 = std::max(c0, 0); c1 = std::min(c1, w * SS);
                    for (int c = c0; c < c1; ++c) line[c / SS] += a * grad((c + .5f) / SS);
                }
            }
        }
    }
    void fill(const Shape& sh, float alpha, std::initializer_list<const Shape*> clips = {}) {
        fill(sh, alpha, clips, [](float) { return 1.f; });
    }
};

Shape rect(float x, float y, float w, float h) { return {poly({x, y, x + w, y, x + w, y + h, x, y + h})}; }

}  // namespace

Suite suite(float p) {
    const float rig = RIG.at1(p);
    // DejaVu Sans Mono 15: 9.03 advance + 3 tracking, baseline 107.5
    return {130 + SUITE.at1(p) + rig, 107.5f, 9.03f + 3, 83.75f + rig};
}

void render(float p, float s, float ox, float oy, float* cov, int w, int h) {
    Raster R{s, ox, oy, cov, w, h};
    const float rig = RIG.at1(p);
    const Shape behind = R.map(BEHIND, rig, 0);

    // letters slide out from behind the eye; the crosshair locks on the O
    for (auto& L : LETTERS) {
        const float dx = rig + L.tr.at1(p);
        R.fill(R.map(*L.shape, dx, 0), 1, {&behind});
        if (L.shape != &O) continue;
        const Shape oin = R.map(O_IN, dx, 0);
        float ring[2];
        RING.at(p, ring);
        if (ring[1] > .002f) {
            // a 2.5-wide circle of radius 10.5 around (150, 60), scaled about its centre
            Shape ann(2);
            for (int i = 0; i < 48; ++i) {
                const float a = i * 6.2831853f / 48, c = std::cos(a), sn = std::sin(a);
                ann[0].push_back(R.px({150 + dx + c * 11.75f * ring[0], 60 + sn * 11.75f * ring[0]}));
                ann[1].push_back(R.px({150 + dx + c * 9.25f * ring[0], 60 + sn * 9.25f * ring[0]}));
            }
            const Shape gap = R.map(O_GAP, dx, 0);
            R.fill(ann, ring[1], {&behind, &oin, &gap});
        }
        for (auto& b : BARS) {
            const float k = b.tr.at1(p);
            if (k <= .001f) continue;
            float x = b.x, y = b.y, bw = b.w, bh = b.h;
            if (b.vertical) { y = b.oy + (y - b.oy) * k; bh *= k; } else { x = b.ox + (x - b.ox) * k; bw *= k; }
            R.fill(R.map(rect(x, y, bw, bh), dx, 0), 1, {&behind, &oin});
        }
    }

    // the eye: lids and iris halves, each scaled/rotated about its own origin
    for (auto& e : EYE) {
        const float op = e.op.at1(p);
        if (op <= .002f) continue;
        float t[5];
        e.tf.at(p, t);
        const float r = t[4] * 3.14159265f / 180, cr = std::cos(r), sr = std::sin(r);
        Shape sh(1);
        for (auto& q : e.poly) {
            const float x = (q.x - e.ox) * t[2], y = (q.y - e.oy) * t[3];
            sh[0].push_back(R.px({x * cr - y * sr + t[0] + e.ox + rig, x * sr + y * cr + t[1] + e.oy}));
        }
        R.fill(sh, op);
    }

    // the beam scanning the word: a gradient streak, stretched about its tip
    float st[2];
    BEAM_STRETCH.at(p, st);
    if (st[1] > .002f) {
        const float mv = BEAM_MOVE.at1(p);
        Shape sh(1);
        for (auto& q : BEAM) sh[0].push_back(R.px({rig + mv + 2 + (q.x - 2) * st[0], q.y}));
        const Shape clip = R.map(rect(150, 30, 185, 60), rig, 0);
        R.fill(sh, st[1], {&clip}, [&](float x) {
            // back to beam units: 0 at -60, .8 at -16.6, 1 at the tip (2)
            const float u = ((x - ox) / s - rig - mv - 2) / st[0] + 2;
            const float f = std::clamp((u + 60) / 62, 0.f, 1.f);
            return f < .7f ? f / .7f * .8f : .8f + (f - .7f) / .3f * .2f;
        });
    }
}

}  // namespace wordmark
