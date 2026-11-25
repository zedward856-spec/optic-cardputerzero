// SYSTEM's TOR and R36S rows.
//
// Tor: Velvet's BROWSER (FAST and FULL) goes through Tor while it's on. Music doesn't: YouTube's
// stream addresses only work from the address that asked for them. On the CardputerZero (and a
// PC) Velvet runs its own `tor` (a SOCKS proxy on 127.0.0.1:9052, the same port Optic DE uses);
// in Optic Suite, the phone's Debian runs it and the app routes its web views through it.
//
// R36S: shows Velvet on an R36S running Optic Link (~/r36s/opticlink, protocol olink.h) over
// Wi-Fi, 2x, and takes its buttons as keys. The first time, the R36S asks for the 4-digit
// code the row shows.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace velvet {

class Tor {
public:
    enum State { Missing = -1, Off = 0, On = 1, Starting = 2, Installing = 3, Unavailable = -2 };
    virtual ~Tor() = default;
    virtual int state() = 0;          // State
    virtual int percent() = 0;        // while Starting
    virtual std::string error() = 0;  // why the last toggle failed ("" = fine)
    virtual void toggle() = 0;
};
std::unique_ptr<Tor> make_tor();
// "socks5h://127.0.0.1:9052" while Velvet's Tor is up (for curl and Chromium), else ""
std::string tor_proxy();

class ScreenLink {
public:
    enum State { Off = 0, Waiting = 1, Pairing = 2, Live = 3, Unavailable = -1 };
    struct Event { int type, code, value; };   // olink.h OL_BTN / OL_AXIS
    virtual ~ScreenLink() = default;
    virtual int state() = 0;
    virtual std::string code() = 0;   // while Pairing
    virtual std::string peer() = 0;   // the R36S's name
    virtual void set_on(bool on) = 0;
    virtual bool on() = 0;
    // true when a receiver watches and the last picture has gone: hand over the next (RGB565)
    virtual bool wants_frame() = 0;
    virtual void frame(const uint16_t* px, int w, int h) = 0;
    virtual bool next_event(Event& e) = 0;
};
std::unique_ptr<ScreenLink> make_screen_link();

}  // namespace velvet
