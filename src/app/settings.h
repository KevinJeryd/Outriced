#pragma once
#include <string>
#include <vector>
#include <filesystem>

namespace oc {

// How clip resolution is chosen for a given size budget.
enum class ClipQuality {
    Auto,      // pick the tallest of 1080/900/720/540 the budget can carry well
    Source,    // never downscale
    P1080, P720, P480,
};

std::string to_string(ClipQuality q);
ClipQuality clip_quality_from_string(std::string_view s);

struct Settings {
    // ---- Capture ----
    int  monitor_index   = 0;      // ddagrab output_idx / vsrc_amf monitor_index
    int  framerate       = 60;
    int  capture_width   = 1920;   // 0,0 = record at the monitor's native size
    int  capture_height  = 1080;
    std::string encoder  = "";     // empty = auto-detect on first run
    std::string capture_backend = "auto";   // auto | amf | cuda | qsv | ddagrab
    int  session_bitrate_kbps = 12000;
    bool capture_audio   = true;
    bool draw_mouse      = true;

    // ---- Audio devices ----
    // Playback endpoints to record (loopback). Empty means "the default output",
    // which is the right answer for most people. More than one matters when a
    // headset splits game and chat across separate devices: recording only the
    // system default would silently drop everyone's voices.
    std::vector<std::string> audio_outputs;
    // Microphone or line input. Empty means do not record one.
    std::string audio_input;
    // "mixed"    - everything summed into one track, ready to upload.
    // "separate" - one track per device, for editing voice and game apart.
    std::string audio_track_mode = "mixed";
    int  mic_gain_percent = 100;
    // A mono microphone that Windows presents as a two-channel endpoint puts the
    // signal on the left channel only, so the recording is audible in one ear.
    //   "stereo"    - leave the channels alone
    //   "mono_left" - copy the left channel to both (the usual fix)
    //   "mono_mix"  - average both channels into both
    std::string mic_channel_mode = "stereo";

    // ---- Clip export ----
    // Decimal MB, matching how upload limits are counted: 10 MB means
    // 10,000,000 bytes, so 9.9 leaves a 100 KB margin rather than a whole one.
    double target_clip_size_mb = 9.9;
    int    clip_audio_kbps     = 128;
    std::string clip_quality   = "auto";
    // x264 preset. Clips are short, so the slower presets cost seconds and buy
    // a lot of quality at a fixed bitrate.
    std::string clip_preset    = "veryslow";
    // Quality floor for the single-pass mode; the size cap still wins.
    int    clip_crf            = 20;
    // Two passes let x264 spend the whole budget where the footage needs it,
    // which is what "as good as possible under the cap" actually requires. One
    // pass is CRF-driven and produces a smaller file when the footage is easy.
    bool   clip_two_pass       = true;
    // Long selections cannot hold 60 fps at a fixed size without collapsing to a
    // tiny frame, so the ladder is allowed to halve the frame rate to keep
    // resolution. Set equal values to pin the rate.
    int    clip_max_fps        = 60;
    int    clip_min_fps        = 30;

    // ---- Auto-record ----
    // Executable names to watch for. Matched on the process list only; nothing
    // is injected into the game.
    std::vector<std::string> auto_record_games;
    bool auto_record_enabled = false;

    // ---- Overlay ----
    bool overlay_enabled = false;
    int  overlay_monitor = -1;   // -1 = the first display that is not the captured one

    // ---- Hotkeys (Win32 RegisterHotKey) ----
    unsigned hotkey_mods = 0x0001 | 0x0004;  // MOD_ALT | MOD_SHIFT
    unsigned hotkey_vk   = 0x77;             // VK_F8
    // Drops a marker at the current position while recording, so the moment can
    // be found again on the timeline afterwards.
    unsigned marker_hotkey_mods = 0x0001 | 0x0004;
    unsigned marker_hotkey_vk   = 0x78;      // VK_F9

    // ---- Housekeeping ----
    // Decimal GB. 0 disables pruning. Recordings are large, so the sessions
    // folder is the one that runs away; clips are small and default to unlimited.
    double max_sessions_gb = 50.0;
    double max_clips_gb    = 0.0;

    // ---- Paths ----
    std::string sessions_dir = "sessions";
    std::string clips_dir    = "clips";
    std::string ffmpeg_path  = "tools/ffmpeg/bin/ffmpeg.exe";
    std::string ffprobe_path = "tools/ffmpeg/bin/ffprobe.exe";

    static Settings load(const std::filesystem::path& file);
    void save(const std::filesystem::path& file) const;

    // Fills in anything left on auto by probing the machine. Runs once at
    // startup and again when the user changes the monitor or backend.
    void resolve_hardware(const std::filesystem::path& root);
};

// Directory the executable lives in, walking up past build output folders so
// that running from build/Release still finds sessions/ next to the source tree.
std::filesystem::path app_root();

// Resolves a configured folder: absolute paths are taken as they are, relative
// ones hang off the app root so a portable copy stays self-contained.
std::filesystem::path resolve_dir(const std::filesystem::path& root,
                                  const std::string& value);

} // namespace oc
