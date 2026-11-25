// optic-camstream — the camera viewfinder for a small machine: raw YUV420 frames on stdout.
//
//   optic-camstream WIDTH HEIGHT FPS [full] [flip]
//
// Does what `rpicam-vid -n -t 0 --codec yuv420 -o -` does for the viewfinder, and nothing else: it
// talks to libcamera directly, so none of rpicam-apps' encoder, preview and option libraries are
// loaded. On a CardputerZero (CM0, ~350 MB) that is the difference between about 6 s and about 2 s
// from start to the first frame. Each frame is WIDTH*HEIGHT*3/2 bytes: Y, then U, then V, rows
// packed (no stride padding). "full": use the sensor's full field of view (its binned full-frame
// mode) instead of the centre crop a small size picks by default. "flip": the picture turned 180
// degrees, for a sensor mounted upside down (the CardputerZero's is).
// Ends on SIGTERM / SIGINT, or when stdout is closed.
//
// build (headers from libcamera-dev of the installed libcamera version):
//   g++ -O2 -std=c++17 -I<dev>/usr/include/libcamera optic-camstream.cpp -o optic-camstream -lcamera -lcamera-base
#include <libcamera/libcamera.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace libcamera;

static std::shared_ptr<Camera> cam;
static Stream *view;
static int W, H, stride;
static std::atomic<bool> stop{false};
struct Plane { const uint8_t *p; size_t len; };
static std::map<const FrameBuffer *, std::vector<Plane>> maps;

static bool put(const uint8_t *p, size_t n) {
    while (n) {
        ssize_t k = write(STDOUT_FILENO, p, n);
        if (k <= 0) return false;
        p += k; n -= k;
    }
    return true;
}

static void completed(Request *req) {
    if (req->status() == Request::RequestCancelled || stop) return;
    const FrameBuffer *fb = req->buffers().at(view);
    const auto &pl = maps[fb];
    bool ok = true;
    if (pl.size() >= 3) {                                    // one mapping per plane
        for (int i = 0; i < 3 && ok; i++) {
            int w = i ? W / 2 : W, h = i ? H / 2 : H, s = i ? stride / 2 : stride;
            for (int y = 0; y < h && ok; y++) ok = put(pl[i].p + (size_t)y * s, w);
        }
    } else if (!pl.empty()) {                                // all three planes in one
        const uint8_t *p = pl[0].p;
        for (int y = 0; y < H && ok; y++) ok = put(p + (size_t)y * stride, W);
        p += (size_t)stride * H;
        for (int i = 0; i < 2 && ok; i++) {
            for (int y = 0; y < H / 2 && ok; y++) ok = put(p + (size_t)y * (stride / 2), W / 2);
            p += (size_t)(stride / 2) * (H / 2);
        }
    }
    if (!ok) { stop = true; return; }                        // the reader went away
    req->reuse(Request::ReuseBuffers);
    cam->queueRequest(req);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: optic-camstream WIDTH HEIGHT FPS [full] [flip]\n"); return 2; }
    W = atoi(argv[1]); H = atoi(argv[2]);
    int fps = atoi(argv[3]); if (fps < 1) fps = 30;
    bool full = false, flip = false;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "full")) full = true;
        else if (!strcmp(argv[i], "flip")) flip = true;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, [](int) { stop = true; });
    signal(SIGINT, [](int) { stop = true; });

    CameraManager cm;
    if (cm.start() || cm.cameras().empty()) { fprintf(stderr, "optic-camstream: no camera\n"); return 1; }
    cam = cm.cameras()[0];
    if (cam->acquire()) { fprintf(stderr, "optic-camstream: the camera is in use\n"); return 1; }

    std::vector<StreamRole> roles = { StreamRole::Viewfinder };
    if (full) roles.push_back(StreamRole::Raw);
    std::unique_ptr<CameraConfiguration> cfg = cam->generateConfiguration(roles);
    if (!cfg) { fprintf(stderr, "optic-camstream: no configuration\n"); return 1; }
    cfg->at(0).size = Size(W, H);
    cfg->at(0).pixelFormat = formats::YUV420;
    cfg->at(0).bufferCount = 3;
    if (full) {                                              // the largest binned mode: all of the sensor, half its pixels
        Size sensor = cam->properties().get(properties::PixelArraySize).value_or(Size(0, 0));
        if (sensor.width) cfg->at(1).size = Size(sensor.width / 2, sensor.height / 2);
        cfg->at(1).bufferCount = 2;
    }
    if (flip) cfg->orientation = Orientation::Rotate180;      // done by the sensor itself (its h / v flip)
    if (cfg->validate() == CameraConfiguration::Invalid || cam->configure(cfg.get())) {
        fprintf(stderr, "optic-camstream: can't configure %dx%d\n", W, H); return 1;
    }
    view = cfg->at(0).stream();
    W = cfg->at(0).size.width; H = cfg->at(0).size.height; stride = cfg->at(0).stride;

    FrameBufferAllocator alloc(cam);
    std::vector<std::unique_ptr<Request>> reqs;
    for (StreamConfiguration &sc : *cfg)
        if (alloc.allocate(sc.stream()) < 0) { fprintf(stderr, "optic-camstream: no buffers\n"); return 1; }
    const auto &vb = alloc.buffers(view);
    for (size_t i = 0; i < vb.size(); i++) {
        FrameBuffer *fb = vb[i].get();
        for (const FrameBuffer::Plane &pl : fb->planes()) {
            void *m = mmap(nullptr, pl.offset + pl.length, PROT_READ, MAP_SHARED, pl.fd.get(), 0);
            if (m == MAP_FAILED) { fprintf(stderr, "optic-camstream: mmap failed\n"); return 1; }
            maps[fb].push_back({ (const uint8_t *)m + pl.offset, pl.length });
        }
        std::unique_ptr<Request> r = cam->createRequest();
        r->addBuffer(view, fb);
        if (full) {                                          // the raw stream has to be fed too
            const auto &rb = alloc.buffers(cfg->at(1).stream());
            r->addBuffer(cfg->at(1).stream(), rb[i % rb.size()].get());
        }
        reqs.push_back(std::move(r));
    }
    if (full && alloc.buffers(cfg->at(1).stream()).size() < reqs.size()) reqs.resize(alloc.buffers(cfg->at(1).stream()).size());

    ControlList ctl(cam->controls());
    int64_t us = 1000000 / fps;
    ctl.set(controls::FrameDurationLimits, Span<const int64_t, 2>({ us, us }));
    cam->requestCompleted.connect(completed);
    if (cam->start(&ctl)) { fprintf(stderr, "optic-camstream: can't start\n"); return 1; }
    for (auto &r : reqs) cam->queueRequest(r.get());
    fprintf(stderr, "optic-camstream: %dx%d at %d fps%s%s\n", W, H, fps, full ? ", full view" : "", flip ? ", turned 180" : "");

    while (!stop) usleep(50000);
    cam->stop();
    cam->release();
    cam.reset();
    cm.stop();
    return 0;
}
