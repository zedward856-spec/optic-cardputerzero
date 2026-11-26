// Infrared remotes for the Remote page: record buttons from a real remote, send them, keep
// them as Flipper .ir files, and fetch remotes from the online Flipper-IRDB.
// The kernel's LIRC devices (/dev/lircN) are used directly. Everything slow (waiting for a
// button, sending, curl) runs on a worker thread; the UI reads a snapshot. No UI code here.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace velvet {

// One button, as a Flipper .ir file holds it.
struct IrSignal {
    std::string name;
    bool raw{false};               // false: protocol + address + command; true: timings
    std::string protocol;          // Flipper's name: "NEC", "Samsung32", "SIRC15", ...
    uint32_t address{0}, command{0};
    uint32_t frequency{38000};     // raw: the carrier in Hz
    float duty{0.33f};             // raw: 0..1
    std::vector<uint32_t> data;    // microseconds, pulse first, then space, pulse, ...
                                   // (a recorded button keeps these even when it was decoded)
};

// ---- protocols and files: plain functions, no hardware ----

// True if ir_encode knows this protocol.
bool ir_can_send(const std::string& protocol);
// The timings to transmit for one press. `toggle` is the RC5 / RC6 toggle bit.
// False (and `why`) when the protocol is not one this can send, or a raw signal is empty.
bool ir_encode(const IrSignal& s, std::vector<uint32_t>& out, uint32_t& carrier_hz, float& duty,
               std::string* why = nullptr, bool toggle = false);
// Tries NEC / NECext / NEC42 / Samsung32 / Kaseikyo / RCA / SIRC (12, 15, 20) / RC5 / RC5X / RC6
// on captured timings (pulse first). Fills protocol, address, command; leaves name and data alone.
bool ir_decode(const std::vector<uint32_t>& timings, IrSignal& out);
// "NEC 04 08", "raw, 34 pulses"
std::string ir_describe(const IrSignal& s);
// The Flipper file format ("IR signals file" / "IR library file", version 1).
std::string ir_file_write(const std::vector<IrSignal>& signals, bool library = false);
bool ir_file_parse(const std::string& text, std::vector<IrSignal>& out);
// A name that is safe as a file name (letters, digits, space, _ - . ; at most 28 long).
std::string ir_safe_name(const std::string& s);
// Splits timings into pieces the kernel takes in one write (each an odd count, at most
// `max_us` long in total), cutting at the longest gaps; `gaps` gets the pause after each piece.
std::vector<std::vector<uint32_t>> ir_split_for_lirc(const std::vector<uint32_t>& t, std::vector<uint32_t>& gaps,
                                                     uint32_t max_us = 400000, size_t max_count = 1001);

// Turns the samples a LIRC receiver gives in MODE2 (pulse / space / timeout, microseconds) into
// one button's timings: from the first pulse to the first long silence. Bursts too short to be
// a button (noise) are dropped and it goes on waiting.
class IrCapture {
public:
    static constexpr uint32_t kGapUs = 75000;     // this much silence ends a button
    static constexpr size_t kMinTimings = 8;      // fewer than this was noise
    static constexpr size_t kMaxTimings = 1024;
    bool feed(uint32_t sample);    // one sample as read from /dev/lircN; true once a button is in
    bool quiet();                  // nothing has come for a while: ends a burst that had begun
    bool started() const { return !t_.empty(); }
    bool done() const { return done_; }
    const std::vector<uint32_t>& timings() const { return t_; }   // pulse first, odd count

private:
    bool end_burst();
    std::vector<uint32_t> t_;
    bool done_{false};
};

#if defined(OPTIC_IR_SELFTEST) || (defined(USE_DESKTOP) && USE_DESKTOP)
// Checks the encoders, decoders and the file format; prints each check, returns the failures.
int ir_self_test();
#endif

// ---- the page's backend ----

struct IrRemoteInfo {
    std::string name;
    int buttons{0};
};

struct IrDbEntry {
    std::string name;      // as in the database: "TVs", "Sony", "Sony_Bravia.ir"
    bool folder{false};
};

enum class IrRec { Idle, Waiting, Caught, Nothing, Failed };

struct IrState {
    bool demo{false};
    bool scanned{false};             // the lirc devices have been looked for
    std::string tx, rx;              // "lirc1" / "lirc0"; empty: none found
    std::string devices;             // one line: "send: lirc1  receive: lirc0"
    std::vector<IrRemoteInfo> remotes;
    std::string open;                // the remote that is open ("" none)
    std::vector<IrSignal> buttons;   // its buttons
    bool sending{false};
    IrRec rec{IrRec::Idle};
    int rec_left_s{0};               // seconds until it gives up
    IrSignal caught;                 // what was recorded; data = raw timings, always
    bool caught_decoded{false};
    std::string rec_error;
    bool db_busy{false};
    std::string db_path;             // the folder db shows ("" = the device kinds)
    std::vector<IrDbEntry> db;
    std::string db_error;
    std::string note;                // the last action's outcome, one line
    uint32_t note_id{0};             // goes up with every new note
    uint32_t saved_id{0};            // goes up when a database file was saved (and opened)
};

class Ir {
public:
    virtual ~Ir() = default;
    virtual IrState state() = 0;                          // cheap: the last snapshot
    virtual void open(const std::string& remote) = 0;     // "" closes
    virtual void send(int button) = 0;                    // of the open remote
    virtual void delete_button(int button) = 0;
    virtual void delete_remote(const std::string& remote) = 0;
    virtual bool exists(const std::string& remote) = 0;
    virtual void record_start() = 0;                      // wait (30 s) for one button
    virtual void record_cancel() = 0;
    // Adds what was caught to `remote` (made if new) as `button`, and opens that remote.
    virtual void record_keep(const std::string& remote, const std::string& button, bool as_raw) = 0;
    virtual void db_list(const std::string& path) = 0;    // a folder of the online database
    virtual void db_fetch(const std::string& path) = 0;   // a file: saved as a remote and opened
};

std::unique_ptr<Ir> make_ir();

}  // namespace velvet
