#pragma once
#include <filesystem>
#include <string>

struct mpv_handle;

namespace oc {

// libmpv playback embedded in a child window of the SDL window.
//
// mpv renders into a real HWND rather than into an ImGui texture: the render
// API would mean an OpenGL context and a manual frame pump, and a child window
// gets working video and audio for a fraction of the code. The trade-off is
// that the video always sits above the ImGui surface, so the layout leaves a
// hole for it.
class Player {
public:
    ~Player();

    bool init(void* parent_hwnd);
    void shutdown();
    bool available() const { return mpv_ != nullptr; }

    void load(const std::filesystem::path& file);
    // Unloads whatever is playing so the file is no longer held open.
    void stop();
    void toggle_pause();
    void set_paused(bool paused);
    bool paused() const { return paused_; }

    void seek_absolute(double seconds);
    void step_seconds(double delta);

    // 0-100, matching mpv's own scale.
    void   set_volume(double volume);
    double volume() const { return volume_; }
    void   set_muted(bool muted);
    bool   muted() const { return muted_; }

    double position() const { return position_; }
    double duration() const { return duration_; }

    // Pumps mpv events; call once per frame.
    void update();

    // Places the video child window, in pixels relative to the parent.
    void set_rect(int x, int y, int w, int h);
    void set_visible(bool visible);

    const std::string& error() const { return error_; }

private:
    mpv_handle* mpv_    = nullptr;
    void*       child_  = nullptr;  // HWND handed to mpv
    void*       parent_ = nullptr;
    double      position_ = 0.0;
    double      duration_ = 0.0;
    bool        paused_   = true;
    bool        visible_  = false;
    double      volume_   = 100.0;
    bool        muted_    = false;
    std::string error_;
};

} // namespace oc
