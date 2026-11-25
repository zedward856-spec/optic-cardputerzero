// Bluetooth audio devices and where sound goes, for the Bluetooth page.
// Everything slow (bluetoothctl, pactl) runs on a worker thread; the UI reads a snapshot.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace velvet {

struct BtDevice {
    std::string mac, name;
    bool paired{false}, connected{false};
};

struct AudioOutput {
    std::string sink;    // the sound server's name for it
    std::string label;   // "Speaker", "Headphone jack", "Redmi Buds 6 Active"
    bool bluetooth{false};
};

struct AudioStream {
    int id{0};
    std::string app;     // who is playing ("Optic Suite", "Launcher", ...)
    int output{-1};      // index into outputs, -1 unknown
};

struct BtAudioState {
    bool available{false};          // bluetoothctl and pactl exist and a controller answered
    bool scanning{false}, busy{false};
    std::string note;               // the last action's outcome, one line
    std::vector<BtDevice> devices;  // paired first, then found ones (named only)
    std::vector<AudioOutput> outputs;
    int default_output{-1};
    std::vector<AudioStream> streams;
};

class BtAudio {
public:
    virtual ~BtAudio() = default;
    virtual BtAudioState state() = 0;                 // cheap: the last snapshot
    virtual void refresh() = 0;                       // ask for a new snapshot
    virtual void scan() = 0;                          // look for devices for ~10 s
    virtual void act(const std::string& mac) = 0;     // pair + connect, connect, or disconnect
    virtual void forget(const std::string& mac) = 0;
    virtual void set_default(int output) = 0;         // everything plays there (streams move too)
    virtual void move_stream(int id, int output) = 0;
    virtual void test() = 0;                          // a chime through the default output
};

std::unique_ptr<BtAudio> make_btaudio();

}  // namespace velvet
