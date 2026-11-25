// Network page backends: Ethernet, Tailscale, a speed test and diagnostic tools (ping, LAN
// scan, port scan, DNS, trace). Everything slow runs on worker threads; the UI polls the
// snapshots below from its page timer (all getters are thread-safe and cheap).
// Commands run with argv lists (no shell), so typed hosts can't inject commands.
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace velvet {

struct EthInfo {
    bool present{false};   // an Ethernet port exists
    bool link{false};      // cable in, link up
    std::string iface, ip, mac, gateway;
    int speed{0};          // Mb/s, 0 unknown
};

struct TsPeer {
    std::string name, ip;
    bool online{false};
};
struct TsInfo {
    enum State { Missing = -1, Off = 0, On = 1, NeedsLogin = 2, Busy = 3 };
    int state{Missing};
    std::string name, ip, login_url, error;
    std::vector<TsPeer> peers;
};

struct SpeedInfo {
    enum Phase { Idle = 0, Ping = 1, Down = 2, Up = 3, Done = 4, Failed = -1 };
    int phase{Idle};
    float ping_ms{0}, down{0}, up{0};  // results (Mb/s)
    float live{0};                     // the current reading for the needle (Mb/s)
    float progress{0};                 // 0..1 through the current phase
    std::string error;
};

enum class NetTool { Ping, Scan, Ports, Dns, Trace };

class NetTools {
public:
    virtual ~NetTools() = default;
    virtual bool demo() const = 0;

    virtual void eth_refresh() = 0;
    virtual EthInfo eth() = 0;

    virtual void ts_refresh() = 0;
    virtual void ts_set(bool on) = 0;   // up (may ask for a login link) / down
    // true on the phone: the Tailscale app switches it, so OK just opens that app
    virtual bool ts_external() const { return false; }
    virtual TsInfo ts() = 0;

    virtual void speed_start() = 0;
    virtual void speed_cancel() = 0;
    virtual SpeedInfo speed() = 0;

    // A tool's output so far, one line each; [running] while it works.
    virtual void tool_run(NetTool t, const std::string& target) = 0;
    virtual void tool_cancel() = 0;
    virtual std::vector<std::string> tool_lines(bool* running) = 0;
    // A sensible target to start from (the gateway, this network, a public host)
    virtual std::string tool_default(NetTool t) = 0;
};

std::unique_ptr<NetTools> make_net_tools();

}  // namespace velvet
