// What runs inside the Terminal page. On Linux (the CardputerZero and the desktop build) a real
// login shell on a pseudo-terminal; in the browser a small built-in demo shell with a pretend
// file system, so the shared HTML still has something to type into.
#pragma once

#include <memory>
#include <string>

namespace velvet {

class Shell {
public:
    virtual ~Shell() = default;
    virtual bool start(int cols, int rows) = 0;
    virtual bool is_demo() const = 0;
    virtual void write(const std::string& bytes) = 0;
    virtual std::string read() = 0;   // whatever the shell printed since the last call
    virtual bool alive() = 0;
    virtual void resize(int cols, int rows) = 0;
    virtual std::string name() const = 0;  // "bash", "demo sh", ...
};

// Starts a shell sized cols x rows. Real shell where possible, else the demo shell
// (always the demo in the browser, or with VELVET_DEMO_SHELL=1).
std::unique_ptr<Shell> make_shell(int cols, int rows);

}  // namespace velvet
