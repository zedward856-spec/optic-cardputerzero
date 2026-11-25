#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "miniaudio.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "sound.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace velvet {

namespace {
constexpr int kRate = 48000;
constexpr float kPi = 3.14159265f;

using Buf = std::vector<float>;

// A tone: frequency glides from f0 to f1, shape 0 = sine, 1 = soft square, 2 = triangle.
void tone(Buf& b, float start_s, float dur_s, float f0, float f1, float amp, int shape, float decay = 6.0f) {
    const size_t at = static_cast<size_t>(start_s * kRate);
    const size_t n = static_cast<size_t>(dur_s * kRate);
    if (b.size() < at + n) b.resize(at + n, 0.0f);
    float ph = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / n;
        const float f = f0 + (f1 - f0) * t;
        ph += f / kRate;
        ph -= std::floor(ph);
        float v;
        if (shape == 0) v = std::sin(2 * kPi * ph);
        else if (shape == 1) v = std::tanh(4.0f * std::sin(2 * kPi * ph)) * 0.7f;  // rounded square
        else v = 4.0f * std::fabs(ph - 0.5f) - 1.0f;
        const float attack = std::min(1.0f, i / (0.002f * kRate));  // 2 ms, no click
        b[at + i] += v * amp * attack * std::exp(-decay * t);
    }
}

// Noise through a one-pole band that sweeps from lo to hi Hz; envelope rises then falls.
void swoosh(Buf& b, float dur_s, float lo, float hi, float amp) {
    const size_t n = static_cast<size_t>(dur_s * kRate);
    b.resize(std::max(b.size(), n), 0.0f);
    uint32_t seed = 0x2545F491;
    float lp = 0, lp2 = 0;
    for (size_t i = 0; i < n; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        const float noise = (seed & 0xFFFF) / 32768.0f - 1.0f;
        const float t = static_cast<float>(i) / n;
        const float fc = lo * std::pow(hi / lo, t);
        const float a = 1.0f - std::exp(-2 * kPi * fc / kRate);
        lp += a * (noise - lp);
        lp2 += a * 0.25f * (lp - lp2);  // band: low-pass minus a slower low-pass
        const float env = std::sin(kPi * std::pow(t, 0.6f));
        b[i] += (lp - lp2) * amp * env;
    }
}

void click(Buf& b, float amp) {
    uint32_t seed = 12345;
    const size_t n = kRate / 250;  // 4 ms
    b.resize(std::max(b.size(), n), 0.0f);
    float prev = 0;
    for (size_t i = 0; i < n; ++i) {
        seed = seed * 1664525 + 1013904223;
        const float noise = (seed >> 16) / 32768.0f - 1.0f;
        b[i] += (noise - prev) * 0.5f * amp * (1.0f - static_cast<float>(i) / n);  // high-passed
        prev = noise;
    }
}

std::array<Buf, static_cast<size_t>(Sfx::Count)> make_sounds() {
    std::array<Buf, static_cast<size_t>(Sfx::Count)> s;
    auto& move = s[static_cast<size_t>(Sfx::Move)];
    click(move, 0.5f);
    tone(move, 0.0f, 0.035f, 2400, 2200, 0.22f, 0, 7);

    auto& open = s[static_cast<size_t>(Sfx::Open)];
    tone(open, 0.000f, 0.060f, 988, 988, 0.20f, 1, 3);    // B5
    tone(open, 0.045f, 0.140f, 1480, 1480, 0.20f, 1, 4);  // F#6
    tone(open, 0.045f, 0.160f, 2960, 2960, 0.06f, 0, 5);  // shimmer

    auto& back = s[static_cast<size_t>(Sfx::Back)];
    tone(back, 0.0f, 0.090f, 1175, 700, 0.24f, 2, 3);

    swoosh(s[static_cast<size_t>(Sfx::Swoosh)], 0.22f, 500, 6000, 0.9f);

    auto& err = s[static_cast<size_t>(Sfx::Error)];
    tone(err, 0.00f, 0.07f, 196, 190, 0.22f, 1, 1);
    tone(err, 0.10f, 0.11f, 185, 175, 0.22f, 1, 2);

    auto& ok = s[static_cast<size_t>(Sfx::Success)];
    const float notes[] = {784, 988, 1175, 1568};
    for (int i = 0; i < 4; ++i) tone(ok, i * 0.05f, i == 3 ? 0.22f : 0.07f, notes[i], notes[i], 0.17f, 1, i == 3 ? 4 : 2);

    tone(s[static_cast<size_t>(Sfx::ToggleOn)], 0.0f, 0.06f, 1320, 1760, 0.2f, 0, 3);
    tone(s[static_cast<size_t>(Sfx::ToggleOff)], 0.0f, 0.06f, 1760, 1320, 0.2f, 0, 3);

    auto& shut = s[static_cast<size_t>(Sfx::Shutter)];  // two clacks + a short noise burst
    click(shut, 1.0f);
    swoosh(shut, 0.07f, 2500, 9000, 0.6f);
    {
        Buf second;
        click(second, 0.9f);
        tone(second, 0.0f, 0.03f, 900, 500, 0.2f, 2, 8);
        const size_t at = static_cast<size_t>(0.09f * kRate);
        shut.resize(std::max(shut.size(), at + second.size()), 0.0f);
        for (size_t i = 0; i < second.size(); ++i) shut[at + i] += second[i];
    }

    auto& key = s[static_cast<size_t>(Sfx::Key)];
    click(key, 0.25f);
    tone(key, 0.0f, 0.012f, 3200, 3000, 0.06f, 0, 6);
    return s;
}
}  // namespace

struct Sound::Impl {
    struct Voice {
        const Buf* buf{nullptr};
        size_t pos{0};
        Sfx kind{Sfx::Count};
    };

    std::array<Buf, static_cast<size_t>(Sfx::Count)> sounds = make_sounds();
    std::mutex mu;
    std::array<Voice, 8> voices{};
    std::atomic<bool> enabled{true};
    std::atomic<float> gain{0.8f};
    ma_device device{};
    bool ok{false};

    // the music file being played, if any (see inapp:: below)
    ma_decoder* dec{nullptr};              // on the heap: a decoder must not be moved once set up
    bool mus_open{false};                  // guarded by mu
    std::atomic<bool> mus_paused{false}, mus_ended{false};
    std::atomic<uint64_t> mus_frames{0};   // output frames played from it
    std::vector<float> mus_buf;

    static void callback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
        auto* self = static_cast<Impl*>(dev->pUserData);
        auto* o = static_cast<float*>(out);
        const ma_uint32 ch = dev->playback.channels;
        const float g = self->gain.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(self->mu);
        for (ma_uint32 i = 0; i < frames; ++i) {
            float v = 0.0f;
            for (auto& voice : self->voices) {
                if (!voice.buf) continue;
                v += (*voice.buf)[voice.pos++];
                if (voice.pos >= voice.buf->size()) voice.buf = nullptr;
            }
            v = std::tanh(v * g);  // soft limit, never harsh clipping
            for (ma_uint32 c = 0; c < ch; ++c) o[i * ch + c] = v;
        }
        if (self->mus_open && ch == 2 && !self->mus_paused.load() && !self->mus_ended.load()) {
            if (self->mus_buf.size() < static_cast<size_t>(frames) * 2) self->mus_buf.resize(static_cast<size_t>(frames) * 2);
            ma_uint64 got = 0;
            ma_decoder_read_pcm_frames(self->dec, self->mus_buf.data(), frames, &got);
            for (ma_uint64 i = 0; i < got * 2; ++i) {
                const float x = o[i] + self->mus_buf[i];
                o[i] = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
            }
            self->mus_frames += got;
            if (got < frames) self->mus_ended = true;
        }
    }

    Impl() {
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = 2;
        cfg.sampleRate = kRate;
        cfg.periodSizeInMilliseconds = 20;
        cfg.dataCallback = callback;
        cfg.pUserData = this;
        if (ma_device_init(nullptr, &cfg, &device) != MA_SUCCESS) return;
        if (ma_device_start(&device) != MA_SUCCESS) {
            ma_device_uninit(&device);
            return;
        }
        ok = true;
    }

    ~Impl() {
        if (g_impl == this) g_impl = nullptr;
        if (ok) ma_device_uninit(&device);
        if (mus_open && dec) { ma_decoder_uninit(dec); delete dec; }
    }

    static Impl* g_impl;
};
Sound::Impl* Sound::Impl::g_impl = nullptr;

Sound::Sound() : impl_(std::make_unique<Impl>()) { Impl::g_impl = impl_.get(); }


Sound::~Sound() = default;

bool Sound::available() const { return impl_->ok; }
bool Sound::enabled() const { return impl_->enabled.load(); }
void Sound::set_enabled(bool on) { impl_->enabled = on; }
void Sound::set_gain(float g) { impl_->gain = std::max(0.0f, std::min(1.0f, g)); }

void Sound::play(Sfx s) {
    if (!impl_->ok || !impl_->enabled.load()) return;
    const Buf* b = &impl_->sounds[static_cast<size_t>(s)];
    std::lock_guard<std::mutex> lock(impl_->mu);
    // Fast repeats (holding a direction, typing) restart the same sound instead of stacking.
    Impl::Voice* slot = nullptr;
    for (auto& v : impl_->voices) {
        if (v.buf && v.kind == s && v.pos < b->size() / 2) {
            slot = &v;
            break;
        }
    }
    if (!slot) {
        for (auto& v : impl_->voices) {
            if (!v.buf) {
                slot = &v;
                break;
            }
        }
    }
    if (!slot) {  // all busy: take the one closest to finishing
        slot = &impl_->voices[0];
        for (auto& v : impl_->voices) {
            if (v.buf->size() - v.pos < slot->buf->size() - slot->pos) slot = &v;
        }
    }
    *slot = Impl::Voice{b, 0, s};
}

struct SoundAccess {
    static Sound::Impl* impl() { return Sound::Impl::g_impl; }
};

namespace inapp {
namespace {
bool decodable(const std::string& path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string e = path.substr(dot + 1);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == "mp3" || e == "flac" || e == "wav";
}
}  // namespace

bool open(const std::string& path) {
    auto* im = SoundAccess::impl();
    if (!im || !im->ok || !decodable(path)) return false;
    stop();
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, kRate);
    auto* d = new ma_decoder;
    if (ma_decoder_init_file(path.c_str(), &cfg, d) != MA_SUCCESS) { delete d; return false; }
    std::lock_guard<std::mutex> lock(im->mu);
    im->dec = d;
    im->mus_frames = 0;
    im->mus_paused = false;
    im->mus_ended = false;
    im->mus_open = true;
    return true;
}

void stop() {
    auto* im = SoundAccess::impl();
    if (!im) return;
    std::lock_guard<std::mutex> lock(im->mu);
    if (im->mus_open) {
        ma_decoder_uninit(im->dec);
        delete im->dec;
        im->dec = nullptr;
        im->mus_open = false;
    }
    im->mus_ended = false;
}

void pause(bool paused) {
    if (auto* im = SoundAccess::impl()) im->mus_paused = paused;
}

void seek(double seconds) {
    auto* im = SoundAccess::impl();
    if (!im) return;
    std::lock_guard<std::mutex> lock(im->mu);
    if (!im->mus_open) return;
    const ma_uint64 f = static_cast<ma_uint64>((seconds < 0 ? 0 : seconds) * kRate);
    if (ma_decoder_seek_to_pcm_frame(im->dec, f) == MA_SUCCESS) {
        im->mus_frames = f;
        im->mus_ended = false;
    }
}

double length(const std::string& path) {
    if (!decodable(path)) return 0;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, kRate);
    ma_decoder d;
    if (ma_decoder_init_file(path.c_str(), &cfg, &d) != MA_SUCCESS) return 0;
    ma_uint64 n = 0;
    ma_decoder_get_length_in_pcm_frames(&d, &n);
    ma_decoder_uninit(&d);
    return static_cast<double>(n) / kRate;
}

double pos() {
    auto* im = SoundAccess::impl();
    return im ? static_cast<double>(im->mus_frames.load()) / kRate : 0;
}

bool ended() {
    auto* im = SoundAccess::impl();
    return im && im->mus_ended.load();
}
}  // namespace inapp

}  // namespace velvet
