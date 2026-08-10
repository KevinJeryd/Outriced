// Headless exercise of the capture path: start a recording, hold it for a few
// seconds, stop it the same way the GUI does, and report what came out.
#include <chrono>
#include <cstdio>
#include <thread>

#include "capture/audio_devices.h"
#include "capture/recorder.h"
#include "app/settings.h"

int main(int argc, char** argv) {
    const int seconds = (argc > 1) ? atoi(argv[1]) : 8;

    const auto root = oc::app_root();
    auto settings = oc::Settings::load(root / "settings.json");
    settings.resolve_hardware(root);
    printf("root      : %s\n", root.string().c_str());
    printf("monitor   : %d   %dx%d @ %d fps   encoder %s\n",
           settings.monitor_index, settings.capture_width, settings.capture_height,
           settings.framerate, settings.encoder.c_str());

    printf("--- outputs ---\n");
    for (const auto& d : oc::enumerate_output_devices())
        printf("  %s%s\n    %s\n", d.name.c_str(), d.is_default ? "  [default]" : "",
               d.id.c_str());
    printf("--- inputs ---\n");
    for (const auto& d : oc::enumerate_input_devices())
        printf("  %s%s\n    %s\n", d.name.c_str(), d.is_default ? "  [default]" : "",
               d.id.c_str());
    printf("audio     : %zu output(s) selected, mic=%s, tracks=%s\n",
           settings.audio_outputs.size(),
           settings.audio_input.empty() ? "none" : "set",
           settings.audio_track_mode.c_str());

    oc::Recorder rec;
    if (!rec.start(settings, root)) {
        printf("FAILED to start: %s\n", rec.last_error().c_str());
        return 1;
    }
    if (!rec.last_error().empty())
        printf("warning   : %s\n", rec.last_error().c_str());
    printf("backend   : %s\n", rec.backend().c_str());
    printf("recording : %s\n", rec.current_file().string().c_str());

    for (int i = 0; i < seconds; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!rec.recording()) {
            printf("ffmpeg exited early after %d s\n", i + 1);
            break;
        }
        printf("  t=%ds\n", i + 1);
    }

    const auto out = rec.stop();
    if (!out) {
        printf("FAILED to finalize: %s\n", rec.last_error().c_str());
        return 1;
    }
    printf("saved     : %s\n", out->string().c_str());
    return 0;
}
