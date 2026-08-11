#include "app/settings.h"

#include <windows.h>
#include <fstream>
#include <nlohmann/json.hpp>

#include "capture/displays.h"
#include "capture/encoders.h"

using nlohmann::json;

namespace oc {

std::string to_string(ClipQuality q) {
    switch (q) {
    case ClipQuality::Source: return "source";
    case ClipQuality::P1080:  return "1080p";
    case ClipQuality::P720:   return "720p";
    case ClipQuality::P480:   return "480p";
    default:                  return "auto";
    }
}

ClipQuality clip_quality_from_string(std::string_view s) {
    if (s == "source") return ClipQuality::Source;
    if (s == "1080p")  return ClipQuality::P1080;
    if (s == "720p")   return ClipQuality::P720;
    if (s == "480p")   return ClipQuality::P480;
    return ClipQuality::Auto;
}

std::filesystem::path app_root() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(buf).parent_path();

    // Running from build/Release should still use the sessions/ and tools/
    // folders that live at the top of the source tree.
    for (int i = 0; i < 4; ++i) {
        if (std::filesystem::exists(dir / "tools" / "ffmpeg" / "bin" / "ffmpeg.exe"))
            return dir;
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return std::filesystem::path(buf).parent_path();
}

std::filesystem::path resolve_dir(const std::filesystem::path& root,
                                  const std::string& value) {
    if (value.empty()) return root;
    std::filesystem::path p(value);
    return p.is_absolute() ? p : root / p;
}

Settings Settings::load(const std::filesystem::path& file) {
    Settings s;
    std::ifstream in(file);
    if (!in) return s;   // caller resolves hardware, then saves

    json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        return s;  // keep defaults rather than refusing to launch
    }

    auto get = [&](const char* key, auto& dst) {
        if (auto it = j.find(key); it != j.end() && !it->is_null()) {
            try { dst = it->get<std::decay_t<decltype(dst)>>(); } catch (const std::exception&) {}
        }
    };

    get("monitor_index",        s.monitor_index);
    get("framerate",            s.framerate);
    get("capture_width",        s.capture_width);
    get("capture_height",       s.capture_height);
    get("encoder",              s.encoder);
    get("capture_backend",      s.capture_backend);
    get("session_bitrate_kbps", s.session_bitrate_kbps);
    get("capture_audio",        s.capture_audio);
    get("draw_mouse",           s.draw_mouse);
    get("fps_mode",             s.fps_mode);
    get("capture_poll_multiplier", s.capture_poll_multiplier);
    get("audio_outputs",        s.audio_outputs);
    get("audio_input",          s.audio_input);
    get("audio_track_mode",     s.audio_track_mode);
    get("mic_gain_percent",     s.mic_gain_percent);
    get("mic_channel_mode",     s.mic_channel_mode);
    get("target_clip_size_mb",  s.target_clip_size_mb);
    get("clip_audio_kbps",      s.clip_audio_kbps);
    get("clip_quality",         s.clip_quality);
    get("clip_preset",          s.clip_preset);
    get("clip_crf",             s.clip_crf);
    get("clip_two_pass",        s.clip_two_pass);
    get("clip_max_fps",         s.clip_max_fps);
    get("clip_min_fps",         s.clip_min_fps);
    get("auto_record_games",    s.auto_record_games);
    get("auto_record_enabled",  s.auto_record_enabled);
    get("overlay_enabled",      s.overlay_enabled);
    get("overlay_monitor",      s.overlay_monitor);
    get("hotkey_mods",          s.hotkey_mods);
    get("hotkey_vk",            s.hotkey_vk);
    get("marker_hotkey_mods",   s.marker_hotkey_mods);
    get("marker_hotkey_vk",     s.marker_hotkey_vk);
    get("max_sessions_gb",      s.max_sessions_gb);
    get("max_clips_gb",         s.max_clips_gb);
    get("sessions_dir",         s.sessions_dir);
    get("clips_dir",            s.clips_dir);
    get("ffmpeg_path",          s.ffmpeg_path);
    get("ffprobe_path",         s.ffprobe_path);
    return s;
}

void Settings::save(const std::filesystem::path& file) const {
    json j{
        {"monitor_index",        monitor_index},
        {"framerate",            framerate},
        {"capture_width",        capture_width},
        {"capture_height",       capture_height},
        {"encoder",              encoder},
        {"capture_backend",      capture_backend},
        {"session_bitrate_kbps", session_bitrate_kbps},
        {"capture_audio",        capture_audio},
        {"draw_mouse",           draw_mouse},
        {"fps_mode",             fps_mode},
        {"capture_poll_multiplier", capture_poll_multiplier},
        {"audio_outputs",        audio_outputs},
        {"audio_input",          audio_input},
        {"audio_track_mode",     audio_track_mode},
        {"mic_gain_percent",     mic_gain_percent},
        {"mic_channel_mode",     mic_channel_mode},
        {"target_clip_size_mb",  target_clip_size_mb},
        {"clip_audio_kbps",      clip_audio_kbps},
        {"clip_quality",         clip_quality},
        {"clip_preset",          clip_preset},
        {"clip_crf",             clip_crf},
        {"clip_two_pass",        clip_two_pass},
        {"clip_max_fps",         clip_max_fps},
        {"clip_min_fps",         clip_min_fps},
        {"auto_record_games",    auto_record_games},
        {"auto_record_enabled",  auto_record_enabled},
        {"overlay_enabled",      overlay_enabled},
        {"overlay_monitor",      overlay_monitor},
        {"hotkey_mods",          hotkey_mods},
        {"hotkey_vk",            hotkey_vk},
        {"marker_hotkey_mods",   marker_hotkey_mods},
        {"marker_hotkey_vk",     marker_hotkey_vk},
        {"max_sessions_gb",      max_sessions_gb},
        {"max_clips_gb",         max_clips_gb},
        {"sessions_dir",         sessions_dir},
        {"clips_dir",            clips_dir},
        {"ffmpeg_path",          ffmpeg_path},
        {"ffprobe_path",         ffprobe_path},
    };
    std::ofstream out(file);
    if (out) out << j.dump(2) << '\n';
}

void Settings::resolve_hardware(const std::filesystem::path& root) {
    const auto ffmpeg = root / ffmpeg_path;

    // Keep the monitor index in range if displays were unplugged since last run.
    const auto displays = enumerate_displays();
    if (!displays.empty()) {
        bool valid = false;
        for (const auto& d : displays) if (d.index == monitor_index) valid = true;
        if (!valid) monitor_index = displays.front().index;

        // Capturing above the panel's refresh rate cannot produce new frames,
        // it only makes the encoder work harder for duplicates.
        for (const auto& d : displays) {
            if (d.index == monitor_index && d.refresh_hz > 0 && framerate > d.refresh_hz)
                framerate = d.refresh_hz;
        }
    }
    if (framerate < 24)  framerate = 24;
    if (framerate > 240) framerate = 240;

    if (encoder.empty()) {
        const auto choices = available_encoders(ffmpeg);
        encoder = "libx264";
        for (const auto& e : choices) {
            if (!e.hardware) continue;
            if (encoder_works(ffmpeg, e.id)) { encoder = e.id; break; }
        }
    }
}

} // namespace oc
