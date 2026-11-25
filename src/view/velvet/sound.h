// UI sound effects. Every sound is synthesised at start-up (no audio files), mixed in
// miniaudio's callback, and played through the default output: ALSA/PulseAudio on the
// CardputerZero and the PC, Web Audio in the browser. If there is no audio device, every
// call is a quiet no-op.
#pragma once

#include <string>
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
    friend struct SoundAccess;
    std::unique_ptr<Impl> impl_;
};

// Music decoded inside the app (MP3, FLAC, WAV) and mixed into the same output as the effects:
// it starts at once, where an outside player takes seconds to load on a CardputerZero.
namespace inapp {
bool open(const std::string& path);   // false: no output, or a format this can't decode (m4a...)
void stop();
void pause(bool paused);
void seek(double seconds);
double pos();                          // seconds played
double length(const std::string& path);   // seconds, 0 if unknown (reads the file: not for the UI thread)
bool ended();                          // the file ran out (stays true until stop / open)
}  // namespace inapp

}  // namespace velvet
