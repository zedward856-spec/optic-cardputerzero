// MUSIC: your own files (MY MUSIC: ~/Music and the SD card's Music folder, no internet needed)
// + YouTube search + streaming for the CardputerZero. yt-dlp finds tracks and the stream
// addresses; mpv (or ffplay) plays the sound, or ffmpeg plays a video sized to the 320x170 screen. Everything slow runs on a thread,
// so the UI never waits. Not in the browser demo (make_music() returns nullptr there).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace velvet {

struct Track {
    std::string id, title, channel;
    int duration{0};  // seconds, 0 = unknown / live
    std::string path;  // a file of your own (MY MUSIC); empty = YouTube
    bool video_file{false};
    bool local() const { return !path.empty(); }
};

class Music {
public:
    struct Status {
        bool busy{false};      // searching or opening a track
        bool playing{false};   // a track is loaded (maybe paused)
        bool paused{false};
        bool ended{false};     // the last track finished by itself (once, then cleared)
        bool video{false};     // playing as video (frames via frame())
        bool loading{false};   // the player has been started but no sound has come out yet
        double pos{0}, dur{0};
        int volume{70};
        std::string title, error, note;   // note: e.g. "SAVED: <title>" after a download (once)
    };
    virtual ~Music() = default;
    // false + why ("install mpv") when there's no player; YouTube also needs yt-dlp (online())
    virtual bool available(std::string& why) = 0;
    virtual bool online() = 0;   // yt-dlp is installed (search / stream / save)
    // MY MUSIC: audio + video files in ~/Music and "/media/SD Card/Music", sorted by name
    virtual std::vector<Track> library() = 0;
    // saves a YouTube track's sound into ~/Music (in the background), so it plays offline later
    virtual void download(const Track& t) = 0;
    virtual void search(const std::string& query) = 0;
    // results of the last search, once it finished (true once per search)
    virtual bool take_results(std::vector<Track>& out) = 0;
    virtual void play(const Track& t) = 0;
    // As video: picture 320x170 (fitted, black bars) + sound, one ffmpeg so they stay in step.
    static constexpr int kVW = 320, kVH = 170;
    virtual void play_video(const Track& t) = 0;
    // The newest video frame (kVW*kVH ARGB); true when there's a new one since the last call.
    virtual bool frame(std::vector<uint32_t>& px) = 0;
    virtual void toggle_pause() = 0;
    virtual void seek(int seconds) = 0;   // relative
    virtual void volume(int delta) = 0;
    // this track will probably be played next: look its stream up now, so OK starts it sooner
    virtual void prepare(const Track&) {}
    virtual void stop() = 0;
    virtual Status status() = 0;
    // The playing track's cover (a JPEG/PNG: a file's own picture, a YouTube thumbnail): true
    // once each time it changes; [bytes] empty = it has none.
    virtual bool art(std::string& bytes) = 0;
};

std::unique_ptr<Music> make_music();
std::string fmt_time(double seconds);   // "3:07", "1:02:45"

}  // namespace velvet
