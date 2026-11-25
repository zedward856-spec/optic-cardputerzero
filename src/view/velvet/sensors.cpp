#include "legacy.h"
#include "sensors.h"

#include "lvgl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#define VELVET_LINUX 1
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <filesystem>
#endif

namespace velvet {

namespace {
constexpr float kRadToDeg = 57.29578f;

// Synthetic motion: a slow, believable wobble plus a turning compass.
Motion simulated(const char* source) {
    const float t = lv_tick_get() / 1000.0f;
    Motion m;
    m.valid = true;
    const float pitch = 0.20f * std::sin(t * 0.7f), roll = 0.28f * std::sin(t * 0.43f + 1.0f);
    m.ax = -9.81f * std::sin(roll);
    m.ay = 9.81f * std::sin(pitch);
    m.az = 9.81f * std::cos(pitch) * std::cos(roll);
    m.gx = 11.0f * std::cos(t * 0.7f);
    m.gy = 9.0f * std::cos(t * 0.43f + 1.0f);
    m.gz = 6.0f * std::sin(t * 0.2f);
    m.has_heading = true;
    m.heading = std::fmod(t * 18.0f + 360.0f, 360.0f);
    m.source = source;
    return m;
}

// Persona-ish colour bars with a moving diagonal sweep, for the desktop camera.
void test_pattern(std::vector<uint32_t>& out) {
    static const uint32_t kBars[] = {0xffffff, 0xffd23f, 0x46d9ff, 0x3ee6a0, 0xd65bff, 0xff2e63, 0x1b3fd0, 0x04060f};
    const int shift = static_cast<int>(lv_tick_get() / 30) % 64;
    out.resize(Camera::kW * Camera::kH);
    for (int y = 0; y < Camera::kH; ++y) {
        for (int x = 0; x < Camera::kW; ++x) {
            uint32_t c = kBars[x * 8 / Camera::kW];
            if (y > 120) c = ((x + y + shift * 2) / 16) % 2 ? 0x0a1747 : 0xffffff;
            else if (((x + y + shift) % 64) < 6) c = 0xff2e63;
            out[y * Camera::kW + x] = c;
        }
    }
}

std::string stamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}
}  // namespace

// ======================================================================== browser bridge
#ifdef __EMSCRIPTEN__
namespace {
struct WebBridge {
    Motion motion;
    uint32_t motion_at{0};
    std::vector<uint32_t> frame;
    bool fresh{false};
    int cam_state{0};  // 0 off, 1 starting, 2 live, -1 denied / none
    int snap_state{0};  // 0 idle, 1 busy, 2 ok, -2 failed
};
WebBridge g_web;
}  // namespace

// Called by the page with the phone's sensors (already turned into the device frame).
extern "C" EMSCRIPTEN_KEEPALIVE void velvet_motion(float ax, float ay, float az, float gx, float gy, float gz,
                                                   float heading) {
    Motion& m = g_web.motion;
    m.valid = true;
    m.ax = ax;
    m.ay = ay;
    m.az = az;
    m.gx = gx;
    m.gy = gy;
    m.gz = gz;
    m.has_heading = heading >= 0;
    m.heading = heading;
    m.source = "PHONE";
    g_web.motion_at = lv_tick_get();
}

// A viewfinder frame from the phone camera: kW x kH RGBA bytes.
extern "C" EMSCRIPTEN_KEEPALIVE void velvet_camera_frame(const uint8_t* rgba) {
    g_web.frame.resize(Camera::kW * Camera::kH);
    for (int i = 0; i < Camera::kW * Camera::kH; ++i) {
        g_web.frame[i] = (rgba[i * 4] << 16) | (rgba[i * 4 + 1] << 8) | rgba[i * 4 + 2];
    }
    g_web.fresh = true;
    g_web.cam_state = 2;
}

extern "C" EMSCRIPTEN_KEEPALIVE void velvet_camera_state(int state) { g_web.cam_state = state; }
extern "C" EMSCRIPTEN_KEEPALIVE void velvet_snap_done(int ok) { g_web.snap_state = ok ? 2 : -2; }

namespace {
class WebSensors final : public Sensors {
public:
    WebSensors() { emscripten_run_script("window.velvetMotion && window.velvetMotion(1)"); }
    ~WebSensors() override { emscripten_run_script("window.velvetMotion && window.velvetMotion(0)"); }
    Motion read() override {
        if (g_web.motion.valid && lv_tick_get() - g_web.motion_at < 1500) return g_web.motion;
        return simulated("SIMULATED");  // a computer has no motion sensor
    }
};

class WebCamera final : public Camera {
public:
    void start() override {
        g_web.cam_state = 1;
        g_web.fresh = false;
        emscripten_run_script("window.velvetCamera && window.velvetCamera(1)");
    }
    void stop() override {
        emscripten_run_script("window.velvetCamera && window.velvetCamera(0)");
        g_web.cam_state = 0;
    }
    bool frame(std::vector<uint32_t>& out) override {
        if (!g_web.fresh) return false;
        g_web.fresh = false;
        out = g_web.frame;
        return true;
    }
    std::string status() override {
        switch (g_web.cam_state) {
            case 2: return "PHONE CAM";
            case 1: return "STARTING";
            case -1: return "NO CAMERA ACCESS";
            default: return "OFF";
        }
    }
    bool live() override { return g_web.cam_state == 2; }
    void snap() override {
        g_web.snap_state = 1;
        emscripten_run_script("window.velvetSnap ? window.velvetSnap() : Module._velvet_snap_done(0)");
    }
    bool snap_result(bool& ok, std::string& message) override {
        if (g_web.snap_state == 2 || g_web.snap_state == -2) {
            ok = g_web.snap_state == 2;
            message = ok ? "PHOTO SAVED" : "COULD NOT SAVE";
            g_web.snap_state = 0;
            return true;
        }
        return false;
    }
};
}  // namespace
#endif

// ======================================================================== CardputerZero
#ifdef VELVET_LINUX
namespace {
bool read_text(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[128] = {};
    const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    out.assign(buf, n);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return true;
}

bool read_num(const std::string& path, double& v) {
    std::string s;
    if (!read_text(path, s) || s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end != s.c_str();
}

// Finds an IIO device whose name contains `want` (e.g. "bmi270", "bmm150").
std::string find_iio(const char* want) {
    const char* base = "/sys/bus/iio/devices";
    DIR* d = opendir(base);
    if (!d) return {};
    std::string found;
    while (dirent* e = readdir(d)) {
        if (std::strncmp(e->d_name, "iio:device", 10) != 0) continue;
        const std::string dir = std::string(base) + "/" + e->d_name;
        std::string name;
        if (read_text(dir + "/name", name) && name.find(want) != std::string::npos) {
            found = dir;
            break;
        }
    }
    closedir(d);
    return found;
}

// BMI270 + BMM150 through the kernel IIO drivers: in_<chan>_<axis>_raw * in_<chan>_scale.
class IioSensors final : public Sensors {
public:
    IioSensors() {
        imu_ = find_iio("bmi270");
        if (imu_.empty()) imu_ = find_iio("bmi2");
        mag_ = find_iio("bmm150");
    }
    bool found() const { return !imu_.empty(); }

    Motion read() override {
        Motion m;
        double acc[3], gyr[3], s_acc = 1, s_gyr = 1;
        const char* ax[3] = {"x", "y", "z"};
        read_num(imu_ + "/in_accel_scale", s_acc);
        read_num(imu_ + "/in_anglvel_scale", s_gyr);
        for (int i = 0; i < 3; ++i) {
            if (!read_num(imu_ + "/in_accel_" + ax[i] + "_raw", acc[i])) return m;
            if (!read_num(imu_ + "/in_anglvel_" + ax[i] + "_raw", gyr[i])) gyr[i] = 0;
        }
        // NOTE: axis directions follow the chip's mounting; adjust here once tested on hardware.
        m.valid = true;
        m.ax = static_cast<float>(acc[0] * s_acc);
        m.ay = static_cast<float>(acc[1] * s_acc);
        m.az = static_cast<float>(acc[2] * s_acc);
        m.gx = static_cast<float>(gyr[0] * s_gyr * kRadToDeg);
        m.gy = static_cast<float>(gyr[1] * s_gyr * kRadToDeg);
        m.gz = static_cast<float>(gyr[2] * s_gyr * kRadToDeg);
        m.source = "BMI270";
        double mag[3], s_mag = 1;
        if (!mag_.empty()) {
            read_num(mag_ + "/in_magn_scale", s_mag);
            bool ok = true;
            for (int i = 0; i < 3; ++i) ok = ok && read_num(mag_ + "/in_magn_" + ax[i] + "_raw", mag[i]);
            if (ok) {
                // tilt-compensated heading (uncalibrated: hard-iron offsets not removed yet)
                const double roll = std::atan2(m.ay, m.az);
                const double pitch = std::atan2(-m.ax, std::sqrt(m.ay * m.ay + m.az * m.az));
                const double mx = mag[0] * s_mag, my = mag[1] * s_mag, mz = mag[2] * s_mag;
                const double xh = mx * std::cos(pitch) + mz * std::sin(pitch);
                const double yh = mx * std::sin(roll) * std::sin(pitch) + my * std::cos(roll) -
                                  mz * std::sin(roll) * std::cos(pitch);
                double h = std::atan2(-yh, xh) * kRadToDeg;
                if (h < 0) h += 360;
                m.has_heading = true;
                m.heading = static_cast<float>(h);
                m.source = "BMI270+BMM150";
            }
        }
        return m;
    }

private:
    std::string imu_, mag_;
};

class SimSensors final : public Sensors {
public:
    explicit SimSensors(const char* why) : why_(why) {}
    Motion read() override { return simulated(why_); }

private:
    const char* why_;
};

std::string which(const char* a, const char* b) {
    for (const char* name : {a, b}) {
        for (const char* dir : {"/usr/bin/", "/usr/local/bin/", "/bin/"}) {
            const std::string p = std::string(dir) + name;
            if (access(p.c_str(), X_OK) == 0) return p;
        }
    }
    return {};
}

std::string photo_dir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    // The SD card mounts at /media/SD Card on the CardputerZero.
    for (const char* sd : {"/media/SD Card", "/media/sd", "/mnt/sd"}) {
        if (fs::is_directory(sd, ec) && access(sd, W_OK) == 0) {
            adopt_old_dir(std::string(sd) + "/DCIM/Velvet", std::string(sd) + "/DCIM/Optic");
            const std::string d = std::string(sd) + "/DCIM/Optic";
            fs::create_directories(d, ec);
            if (!ec) return d;
        }
    }
    const char* home = std::getenv("HOME");
    const std::string pics = std::string(home ? home : "/tmp") + "/Pictures";
    adopt_old_dir(pics + "/Velvet", pics + "/Optic");
    const std::string d = pics + "/Optic";
    fs::create_directories(d, ec);
    return d;
}

// IMX219 through rpicam-vid (YUV420 frames on a pipe) for the viewfinder, and
// rpicam-still for the photo itself (full 8 MP, saved as JPEG).
class RpiCamera final : public Camera {
public:
    RpiCamera(std::string vid, std::string still) : vid_(std::move(vid)), still_(std::move(still)) {}
    ~RpiCamera() override { stop(); }

    void start() override {
        if (running_) return;
        if (reader_.joinable()) reader_.join();  // a stream that ended on its own
        if (!spawn()) {
            state_ = "NO CAMERA";
            return;
        }
        state_ = "STARTING";
        running_ = true;
        reader_ = std::thread([this] { read_loop(); });
    }

    void stop() override {
        running_ = false;
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        if (reader_.joinable()) reader_.join();
        if (snapper_.joinable()) snapper_.join();
    }

    bool frame(std::vector<uint32_t>& out) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (!fresh_) return false;
        fresh_ = false;
        out = frame_;
        return true;
    }

    std::string status() override {
        std::lock_guard<std::mutex> lock(mu_);
        return state_;
    }
    bool live() override {
        std::lock_guard<std::mutex> lock(mu_);
        return state_ == "LIVE";
    }

    void snap() override {
        if (snapping_) return;
        snapping_ = true;
        if (snapper_.joinable()) snapper_.join();
        snapper_ = std::thread([this] {
            // the sensor can only be opened once: pause the viewfinder for the still
            const bool was = running_;
            stop_stream();
            const std::string path = photo_dir() + "/optic-" + stamp() + ".jpg";
            const pid_t p = fork();
            if (p == 0) {
                const int devnull = open("/dev/null", O_RDWR);
                dup2(devnull, 1);
                dup2(devnull, 2);
                // the CardputerZero's sensor is mounted upside down: turned 180 (as M5's own camera app shows it)
                execl(still_.c_str(), still_.c_str(), "-n", "-t", "600", "--hflip", "--vflip", "-o", path.c_str(),
                      static_cast<char*>(nullptr));
                _exit(127);
            }
            int st = 1;
            if (p > 0) waitpid(p, &st, 0);
            const bool ok = p > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                snap_ok_ = ok;
                const std::string name = path.substr(path.find_last_of('/') + 1);
                snap_msg_ = ok ? (path.rfind("/media/", 0) == 0 ? "SD: " : "SAVED ") + name : "PHOTO FAILED";
                snap_done_ = true;
            }
            if (was) {
                std::lock_guard<std::mutex> lock(mu_);
                restart_ = true;  // the UI thread restarts the stream
            }
            snapping_ = false;
        });
    }

    bool snap_result(bool& ok, std::string& message) override {
        bool restart = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            restart = restart_;
            restart_ = false;
        }
        if (restart) {
            running_ = false;
            if (reader_.joinable()) reader_.join();
            start();
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (!snap_done_) return false;
        snap_done_ = false;
        ok = snap_ok_;
        message = snap_msg_;
        return true;
    }

private:
    static constexpr int kSrcW = 320, kSrcH = 240;

    bool spawn() {
        int pipefd[2];
        if (pipe(pipefd) != 0) return false;
        const std::string w = std::to_string(kSrcW), h = std::to_string(kSrcH);
        pid_ = fork();
        if (pid_ < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            return false;
        }
        if (pid_ == 0) {
            dup2(pipefd[1], 1);
            const int devnull = open("/dev/null", O_RDWR);
            dup2(devnull, 2);
            close(pipefd[0]);
            // optic-camstream when it is installed: the same frames straight from libcamera, at 30 a
            // second and with the sensor's full field of view. rpicam-vid takes about 6 s to its
            // first frame on a CardputerZero (it loads encoder and preview libraries the viewfinder
            // never uses); optic-camstream about 0.3 s (measured 2026-11-25).
            for (const char* cs : { "/usr/share/APPLaunch/bin/optic-camstream", "/usr/local/bin/optic-camstream" })
                if (access(cs, X_OK) == 0)
                    execl(cs, cs, w.c_str(), h.c_str(), "30", "full", "flip", static_cast<char*>(nullptr));
            execl(vid_.c_str(), vid_.c_str(), "-n", "-t", "0", "--width", w.c_str(), "--height", h.c_str(),
                  "--framerate", "30", "--hflip", "--vflip", "--codec", "yuv420", "-o", "-", static_cast<char*>(nullptr));
            _exit(127);
        }
        close(pipefd[1]);
        fd_ = pipefd[0];
        return true;
    }

    void stop_stream() {
        running_ = false;
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    void read_loop() {
        const size_t ysz = kSrcW * kSrcH, csz = ysz / 4, fsz = ysz + 2 * csz;
        std::vector<uint8_t> buf(fsz);
        std::vector<uint32_t> rgb(kW * kH);
        const int top = (kSrcH - kH) / 2;
        while (running_) {
            size_t got = 0;
            while (got < fsz && running_) {
                const ssize_t n = ::read(fd_, buf.data() + got, fsz - got);
                if (n <= 0) {
                    std::lock_guard<std::mutex> lock(mu_);
                    if (running_) state_ = "NO CAMERA";
                    return;
                }
                got += static_cast<size_t>(n);
            }
            const uint8_t* Y = buf.data();
            const uint8_t* U = Y + ysz;
            const uint8_t* V = U + csz;
            for (int y = 0; y < kH; ++y) {
                const int sy = y + top;
                for (int x = 0; x < kW; ++x) {
                    const int yy = Y[sy * kSrcW + x] - 16;
                    const int u = U[(sy / 2) * (kSrcW / 2) + x / 2] - 128;
                    const int v = V[(sy / 2) * (kSrcW / 2) + x / 2] - 128;
                    const int r = std::clamp((298 * yy + 409 * v + 128) >> 8, 0, 255);
                    const int g = std::clamp((298 * yy - 100 * u - 208 * v + 128) >> 8, 0, 255);
                    const int b = std::clamp((298 * yy + 516 * u + 128) >> 8, 0, 255);
                    rgb[y * kW + x] = (r << 16) | (g << 8) | b;
                }
            }
            std::lock_guard<std::mutex> lock(mu_);
            frame_.swap(rgb);
            rgb.resize(kW * kH);
            fresh_ = true;
            state_ = "LIVE";
        }
    }

    std::string vid_, still_;
    std::atomic<bool> running_{false};
    std::atomic<bool> snapping_{false};
    pid_t pid_{-1};
    int fd_{-1};
    std::thread reader_, snapper_;
    std::mutex mu_;
    std::vector<uint32_t> frame_;
    bool fresh_{false};
    std::string state_{"OFF"};
    bool snap_done_{false}, snap_ok_{false}, restart_{false};
    std::string snap_msg_;
};
}  // namespace
#endif

// ======================================================================== demo camera
namespace {
class DemoCamera final : public Camera {
public:
    explicit DemoCamera(const char* why) : why_(why) {}
    void start() override { on_ = true; }
    void stop() override { on_ = false; }
    bool frame(std::vector<uint32_t>& out) override {
        if (!on_) return false;
        test_pattern(out);
        return true;
    }
    std::string status() override { return why_; }
    bool live() override { return on_; }
    void snap() override { pending_ = true; }
    bool snap_result(bool& ok, std::string& message) override {
        if (!pending_) return false;
        pending_ = false;
        ok = true;
        message = "DEMO: NOT SAVED";
        return true;
    }

private:
    const char* why_;
    bool on_{false}, pending_{false};
};
}  // namespace

std::unique_ptr<Sensors> make_sensors() {
#ifdef __EMSCRIPTEN__
    return std::make_unique<WebSensors>();
#elif defined(VELVET_LINUX)
    auto iio = std::make_unique<IioSensors>();
    if (iio->found()) return iio;
    return std::make_unique<SimSensors>("SIMULATED");
#else
    return nullptr;
#endif
}

std::unique_ptr<Camera> make_camera() {
#ifdef __EMSCRIPTEN__
    return std::make_unique<WebCamera>();
#elif defined(VELVET_LINUX)
    const std::string vid = which("rpicam-vid", "libcamera-vid");
    const std::string still = which("rpicam-still", "libcamera-still");
    if (!vid.empty() && !still.empty()) return std::make_unique<RpiCamera>(vid, still);
    return std::make_unique<DemoCamera>("TEST PATTERN");
#else
    return std::make_unique<DemoCamera>("TEST PATTERN");
#endif
}

std::string photos_dir() {
#ifdef __EMSCRIPTEN__
    std::error_code ec;
    std::filesystem::create_directories("/photos", ec);
    return "/photos";
#else
    return photo_dir();
#endif
}

#ifdef __EMSCRIPTEN__
// The page hands each photo it saved to the phone over here too, so the GALLERY can show it.
extern "C" EMSCRIPTEN_KEEPALIVE void velvet_photo_add(const unsigned char* data, int len, double stamp) {
    char name[64];
    std::snprintf(name, sizeof name, "/photos/optic-%.0f.jpg", stamp);
    std::error_code ec;
    std::filesystem::create_directories("/photos", ec);
    if (FILE* f = std::fopen(name, "wb")) {
        std::fwrite(data, 1, static_cast<size_t>(len), f);
        std::fclose(f);
    }
}
#endif

}  // namespace velvet
