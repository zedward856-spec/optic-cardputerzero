// The hub's PWN page: a pwnagotchi companion with swappable skins (the Capri skin is the user's
// own fan art; until it's drawn, a plain line-art "construct").
//
// On the CardputerZero (and a PC) it shows the real pwnagotchi through the status file its
// Optic plugin writes (/tmp/optic-pwn.status, the same file Optic DE's Pwn app reads), and
// ENTER starts / stops it with `optic-pwn`. Without pwnagotchi, and in Optic Suite / the web
// build, ENTER runs a demo with the same moods and numbers.
// Skins: ~/.local/share/optic-pwn/skins/<skin>/faces/<mood>.png, lines/<mood>.txt, name.txt,
// ABOUT.txt (its first line, e.g. the fan-content notice, shows under the name); the skin in use
// is `skin=` in ~/.config/optic-pwn/conf (shared with Optic DE).
#pragma once

#include <memory>
#include <string>

namespace velvet {

struct PwnState {
    bool live{false};   // something is running (the demo or pwnagotchi)
    bool demo{false};
    std::string mood{"sleep"};
    std::string line;
    std::string ch, aps, pwnd, total, peers, uptime, mode;
};

class Pwn {
public:
    virtual ~Pwn() = default;
    virtual PwnState read() = 0;
    virtual void toggle() = 0;
    virtual void next_skin() = 0;
    virtual std::string skin_name() = 0;
    virtual std::string notice() = 0;                          // the skin's ABOUT first line, or ""
    virtual std::string face_file(const std::string& mood) = 0; // an LVGL path ("A:/...png") or ""
};
std::unique_ptr<Pwn> make_pwn();

}  // namespace velvet
