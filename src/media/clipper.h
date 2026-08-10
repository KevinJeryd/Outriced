#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "app/settings.h"

namespace oc {

// Trims a marked range out of a session and re-encodes it to land just under the
// configured size target, so it can be dropped straight into Discord.
class ClipExporter {
public:
    ~ClipExporter();

    struct Request {
        std::filesystem::path source;
        double in_point  = 0.0;
        double out_point = 0.0;
        int    source_height = 0;   // 0 = assume 1080
        int    source_fps    = 0;   // 0 = assume 60
    };

    bool busy() const { return busy_.load(std::memory_order_relaxed); }
    const std::string& status() const { return status_; }
    std::filesystem::path last_output() const { return last_output_; }

    // Runs the export on a worker thread; poll busy()/status() from the UI.
    bool begin(const Request& req, const Settings& s, const std::filesystem::path& root);

    // Video bitrate in kbps that lands a clip of `duration` near `target_mb`,
    // after reserving room for the audio track.
    static int target_video_kbps(double duration, double target_mb, int audio_kbps);

    struct Plan {
        int height     = 1080;
        int fps        = 60;
        int video_kbps = 0;
    };

    // Resolution and frame rate the budget can carry. Shown in the UI before
    // export so the trade-off is visible while marking.
    static Plan plan(double duration, const Settings& s,
                     int source_height, int source_fps);

private:
    void run(Request req, Settings s, std::filesystem::path root);

    std::thread           thread_;
    std::atomic<bool>     busy_{false};
    std::string           status_;
    std::filesystem::path last_output_;
};

} // namespace oc
