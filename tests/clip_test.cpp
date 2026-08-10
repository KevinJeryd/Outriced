// Exercises the clip export path against an existing session.
#include <chrono>
#include <cstdio>
#include <thread>

#include "media/clipper.h"
#include "media/sessions.h"
#include "app/settings.h"

int main(int argc, char** argv) {
    const auto root = oc::app_root();
    const auto settings = oc::Settings::load(root / "settings.json");

    auto found = oc::scan_sessions(settings, root);
    if (found.empty()) {
        printf("no sessions to clip from\n");
        return 1;
    }

    // Optional third argument selects a session by name substring; the default
    // is the newest one.
    size_t pick = 0;
    if (argc > 3) {
        for (size_t i = 0; i < found.size(); ++i) {
            if (found[i].display_name.find(argv[3]) != std::string::npos) { pick = i; break; }
        }
    }
    const auto& s = found[pick];
    const double in  = (argc > 1) ? atof(argv[1]) : 1.0;
    const double out = (argc > 2) ? atof(argv[2]) : std::min(in + 5.0, s.duration);

    printf("source   : %s (%.2fs)\n", s.display_name.c_str(), s.duration);
    printf("range    : %.2f -> %.2f  (%.2fs)\n", in, out, out - in);
    printf("target   : %.1f MB, video %d kbps\n", settings.target_clip_size_mb,
           oc::ClipExporter::target_video_kbps(out - in, settings.target_clip_size_mb,
                                               settings.clip_audio_kbps));

    printf("source fmt: %dx%d @ %dfps\n", s.width, s.height, s.fps);

    oc::ClipExporter ex;
    if (!ex.begin({s.file, in, out, s.height, s.fps}, settings, root)) {
        printf("begin failed: %s\n", ex.status().c_str());
        return 1;
    }
    while (ex.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    printf("result   : %s\n", ex.status().c_str());
    return ex.last_output().empty() ? 1 : 0;
}
