// CardputerZero device services for the Velvet UI: Wi-Fi (NetworkManager), battery,
// backlight and volume. Mirrors what the stock APPLaunch "v1" settings do
// (nmcli, bq27220 sysfs, /sys/class/backlight, amixer), but runs every command
// with an argv list (no shell), so SSIDs and passwords can't inject commands.
//
// All slow work runs off the UI thread; callbacks are always delivered on the
// LVGL thread (from an lv_timer), so they may touch widgets directly.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace velvet {

struct WifiAp {
    std::string ssid;
    int signal{0};        // 0..100
    bool secure{false};   // needs a password
    bool saved{false};    // NetworkManager already has a profile for it
    bool active{false};   // currently connected
};

struct NetStatus {
    bool radio_on{true};
    bool connected{false};
    std::string ssid;
    std::string ip;       // IPv4 without prefix, "" if none
    int signal{0};
};

struct PowerInfo {
    bool valid{false};
    int percent{0};
    int millivolts{0};
    bool charging{false};
};

// The HDMI output and the Optic desktop on it (CardputerZero: /opt/optic-de/bin/optic-de-hdmi).
struct HdmiInfo {
    bool supported{false};   // this machine has an HDMI output the Optic desktop can run on
    bool connected{false};   // a screen is plugged in
    bool running{false};     // the Optic desktop is running on it
    std::string mode;        // "1024x600", "" when not known
    std::string hz;          // "75", "" when not known
};

class Device {
public:
    using Done = std::function<void(bool ok, const std::string& message)>;

    virtual ~Device() = default;

    // Wi-Fi (async)
    virtual void net_status(std::function<void(NetStatus)> cb) = 0;
    virtual void wifi_scan(std::function<void(std::vector<WifiAp>)> cb) = 0;
    // Empty password = use the saved profile, or join an open network.
    virtual void wifi_connect(const std::string& ssid, const std::string& password, Done cb) = 0;
    virtual void wifi_disconnect(Done cb) = 0;
    virtual void wifi_forget(const std::string& ssid, Done cb) = 0;
    virtual void wifi_set_radio(bool on, Done cb) = 0;

    // Hardware (fast, synchronous). Percent values are 0..100, -1 if unavailable.
    virtual PowerInfo power() = 0;
    virtual int brightness() = 0;
    virtual bool set_brightness(int percent) = 0;
    virtual int volume() = 0;
    virtual bool set_volume(int percent) = 0;
    virtual std::string hostname() = 0;

    // The Optic desktop on the HDMI screen: what is there now, and on / off. Not instant (it asks
    // the display): call it from a slow timer, not every frame.
    virtual HdmiInfo hdmi() { return {}; }
    virtual void set_hdmi(bool on) { (void)on; }

    // True for the fake backend used on the PC and in the browser.
    virtual bool is_demo() const = 0;
    // The Wi-Fi page shows real networks (the device's, or the phone's in Optic Suite).
    virtual bool real_wifi() const { return !is_demo(); }
};

// Real backend on the CardputerZero; demo backend on the desktop/web builds.
// On a desktop Linux build, VELVET_REAL_DEVICE=1 uses the real backend (careful:
// joining a network then changes this PC's Wi-Fi).
std::unique_ptr<Device> make_device();

}  // namespace velvet
