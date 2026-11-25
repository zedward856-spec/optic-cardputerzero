// UI sound effects. Every sound is synthesised at start-up (no audio files), mixed in
// miniaudio's callback, and played through the default output: ALSA/PulseAudio on the
// CardputerZero and the PC, Web Audio in the browser. If there is no audio device, every
// call is a quiet no-op.
#pragma once

#include <memory>

namespace velvet {

enum class Sfx { Move, Open, Back, Swoosh, Error, Success, ToggleOn, ToggleOff, Key, Shutter, Count };

class Sound {
public:
    Sound();
    ~Sound();
    void play(Sfx s);
    void set_enabled(bool on);
    bool enabled() const;
    void set_gain(float g);  // 0..1, on top of the system volume
    bool available() const;  // an output device opened

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace velvet
