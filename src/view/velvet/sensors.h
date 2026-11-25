// Motion sensor and camera for the SENSORS and CAMERA apps.
//   CardputerZero: BMI270 (accel + gyro) and BMM150 (compass) through Linux IIO sysfs; the
//                  IMX219 camera through rpicam-vid / rpicam-still.
//   Browser:       the phone's own motion sensors and camera, fed in by the page.
//   Desktop:       simulated values and a test pattern.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace velvet {

// Where photos are kept (and the GALLERY reads them): the SD card's DCIM/Velvet, else
// ~/Pictures/Velvet; in the browser demo an in-memory /photos folder.
std::string photos_dir();

struct Motion {
    bool valid{false};
    float ax{0}, ay{0}, az{9.81f};  // m/s^2, gravity included. Device frame: x right, y up the screen, z out of it
    float gx{0}, gy{0}, gz{0};      // deg/s
    bool has_heading{false};
    float heading{0};               // degrees from north, clockwise
    std::string source;             // "BMI270", "PHONE", "SIMULATED", ...
};

class Sensors {
public:
    virtual ~Sensors() = default;
    virtual Motion read() = 0;
};

class Camera {
public:
    static constexpr int kW = 320, kH = 170;  // viewfinder = the whole screen

    virtual ~Camera() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    // Copies the newest frame (kW x kH, 0xRRGGBB) into `out`. False if nothing new.
    virtual bool frame(std::vector<uint32_t>& out) = 0;
    // "LIVE", "STARTING", "NO CAMERA", "ALLOW CAMERA", ...
    virtual std::string status() = 0;
    virtual bool live() = 0;
    // Takes a full-resolution photo in the background.
    virtual void snap() = 0;
    // After snap(): true once finished, with ok + a short message ("SAVED ...").
    virtual bool snap_result(bool& ok, std::string& message) = 0;
};

std::unique_ptr<Sensors> make_sensors();
std::unique_ptr<Camera> make_camera();

}  // namespace velvet
