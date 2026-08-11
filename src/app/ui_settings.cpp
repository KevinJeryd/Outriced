#include "app/ui_internal.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "capture/audio_devices.h"
#include "media/clipper.h"
#include "app/dialogs.h"
#include "capture/displays.h"
#include "capture/encoders.h"
#include "capture/game_watch.h"
#include "platform/log.h"
#include "media/player.h"
#include "capture/recorder.h"
#include "media/sessions.h"
#include "app/settings.h"
#include "platform/tray.h"

namespace oc {
constexpr double kMinClipMb  = 2.0;
constexpr double kMaxClipMb  = 500.0;   // full Nitro
constexpr double kMinFolderGb = 0.0;    // 0 disables pruning
constexpr double kMaxFolderGb = 1000.0;
// ImGui reports keys in its own enum; RegisterHotKey wants a Win32 virtual key.
// Modifiers return 0 so they are never accepted as the trigger key itself.
unsigned imgui_key_to_vk(ImGuiKey key) {
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
        return 'A' + (unsigned)(key - ImGuiKey_A);
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
        return '0' + (unsigned)(key - ImGuiKey_0);
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24)
        return 0x70 + (unsigned)(key - ImGuiKey_F1);   // VK_F1
    switch (key) {
    case ImGuiKey_Insert:   return 0x2D;
    case ImGuiKey_Delete:   return 0x2E;
    case ImGuiKey_Home:     return 0x24;
    case ImGuiKey_End:      return 0x23;
    case ImGuiKey_PageUp:   return 0x21;
    case ImGuiKey_PageDown: return 0x22;
    case ImGuiKey_Pause:    return 0x13;
    default:                return 0;
    }
}
// ---------------------------------------------------------------- settings

void refresh_settings_draft(AppContext& ctx) {
    g_ui.draft    = *ctx.settings;
    g_ui.displays = enumerate_displays();
    g_ui.encoders = available_encoders(ctx.root / ctx.settings->ffmpeg_path);
    g_ui.detected_vendor = vendor_name(detect_vendor());
    g_ui.settings_loaded = true;
}

// One function per section of the tab. Each takes the draft it edits and
// nothing else, so the compiler is what verifies the split: a section reading
// a local that belongs to another would fail to build.
namespace {

void section_capture(AppContext& ctx, Settings& d) {
    // ---- capture ----
    ImGui::SeparatorText("Capture");

    int monitor_pos = 0;
    std::vector<const char*> monitor_labels;
    for (int i = 0; i < (int)g_ui.displays.size(); ++i) {
        monitor_labels.push_back(g_ui.displays[i].label.c_str());
        if (g_ui.displays[i].index == d.monitor_index) monitor_pos = i;
    }
    if (!monitor_labels.empty()) {
        ImGui::SetNextItemWidth(px(320));
        if (ImGui::Combo("Monitor", &monitor_pos, monitor_labels.data(),
                         (int)monitor_labels.size()))
            d.monitor_index = g_ui.displays[monitor_pos].index;
    } else {
        ImGui::TextDisabled("No displays detected");
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) refresh_settings_draft(ctx);

    // Output resolution, expressed as a height the source is scaled to.
    const int src_h = (monitor_pos < (int)g_ui.displays.size())
                          ? g_ui.displays[monitor_pos].height : 1080;
    const char* res_labels[] = {"Native (no scaling)", "1440p", "1080p", "720p"};
    const int   res_heights[] = {0, 1440, 1080, 720};
    int res_pos = 0;
    for (int i = 0; i < 4; ++i) if (d.capture_height == res_heights[i]) res_pos = i;
    ImGui::SetNextItemWidth(px(320));
    if (ImGui::Combo("Recording resolution", &res_pos, res_labels, 4)) {
        if (res_heights[res_pos] == 0) {
            d.capture_width = d.capture_height = 0;
        } else {
            d.capture_height = res_heights[res_pos];
            d.capture_width  = (d.capture_height * 16) / 9;
            d.capture_width -= d.capture_width % 2;
        }
    }
    if (src_h > 0 && d.capture_height > src_h)
        ImGui::TextDisabled("  Upscaling from %dp; native would be sharper.", src_h);

    const int max_fps = (monitor_pos < (int)g_ui.displays.size() &&
                         g_ui.displays[monitor_pos].refresh_hz > 0)
                            ? g_ui.displays[monitor_pos].refresh_hz : 144;
    ImGui::SetNextItemWidth(px(320));
    if (ImGui::SliderInt("Framerate", &d.framerate, 24, max_fps))
        d.framerate = std::clamp(d.framerate, 24, max_fps);

    // Measured on this machine: in-game at 60 the capture held 58 real frames a
    // second; the same scene at 144 collapsed to between 2 and 21, because the
    // capture, the 4K downscale and the encoder are all competing with the game
    // for the same GPU. The file still claims 144 -- the shortfall is padded
    // with duplicates -- which is exactly what reads as stutter on playback.
    if (d.framerate > 60) {
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                           "  Above 60 the capture is unlikely to keep up while a game runs.");
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                           "  Measured here: 58 real fps at 60, but only 2-21 at 144.");
    }

    // Exposed because it is load-bearing for the two settings around it: it
    // decides whether capture polling can decimate, and whether clip export's
    // -ss lands where the scrubber said. It was previously settings.json only,
    // and a value left on vfr there silently changed what both of those did.
    const char* fps_mode_labels[] = {"Constant (recommended)", "Variable"};
    int fps_mode_pos = d.fps_mode == "vfr" ? 1 : 0;
    ImGui::SetNextItemWidth(px(320));
    if (ImGui::Combo("Frame timing", &fps_mode_pos, fps_mode_labels, 2))
        d.fps_mode = fps_mode_pos == 1 ? "vfr" : "cfr";
    if (fps_mode_pos == 1)
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                           "  Disables capture polling, and clip trim points get less exact.");

    // Only the ffmpeg-side backends are asked to produce frames at a rate.
    // Native capture waits on the duplication API and takes frames when the
    // desktop presents them, so there is nothing here for it to control.
    const bool native_capture =
        d.capture_backend == "native" || d.capture_backend == "auto";

    // Reduces duplicate frames but does not improve how even the motion looks;
    // see Settings::capture_poll_multiplier for the numbers. Kept visible so the
    // trade can be re-measured, not presented as a quality setting.
    ImGui::SetNextItemWidth(px(320));
    ImGui::BeginDisabled(fps_mode_pos == 1 || native_capture);
    if (ImGui::SliderInt("Capture polling", &d.capture_poll_multiplier, 1, 4, "%dx"))
        d.capture_poll_multiplier = std::clamp(d.capture_poll_multiplier, 1, 4);
    ImGui::EndDisabled();
    if (native_capture)
        ImGui::TextDisabled("  Not used by the built-in capture, which takes frames as they arrive.");
    else if (fps_mode_pos == 1)
        ImGui::TextDisabled("  Ignored while frame timing is variable.");
    else
        ImGui::TextDisabled("  Asks the display for a frame %d times a second; still records %d fps.",
                            d.framerate * d.capture_poll_multiplier, d.framerate);
    if (!native_capture && fps_mode_pos == 0 && d.capture_poll_multiplier > 1)
        ImGui::TextDisabled("  Fewer duplicate frames, but no measured gain in smoothness, and more GPU readback.");

    ImGui::SetNextItemWidth(px(320));
    ImGui::SliderInt("Recording bitrate (kbps)", &d.session_bitrate_kbps, 4000, 120000);
    ImGui::TextDisabled("  ~%.0f MB per minute",
                        d.session_bitrate_kbps * 60.0 / 8192.0);

    // A bitrate only means something against the pixels it has to cover. The
    // same 12 Mbps that looks clean at 1080p60 is a quarter of the budget at
    // 4K60, which shows up as blocky flat areas rather than as softness. The
    // slider gave no hint of that, so a recording left at native 4K after a
    // resolution change quietly fell to 0.0155 bpp.
    const int out_w = d.capture_width  > 0 ? d.capture_width
                                           : (src_h > 0 ? (src_h * 16) / 9 : 1920);
    const int out_h = d.capture_height > 0 ? d.capture_height
                                           : (src_h > 0 ? src_h : 1080);
    const double bpp = (double)d.session_bitrate_kbps * 1000.0 /
                       ((double)out_w * out_h * std::max(d.framerate, 1));
    const int want_kbps =
        (int)((double)out_w * out_h * std::max(d.framerate, 1) * 0.045 / 1000.0);
    ImGui::TextDisabled("  %dx%d at %d fps = %.4f bits per pixel", out_w, out_h,
                        d.framerate, bpp);
    if (bpp < 0.030) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "  Too low for this resolution; flat areas will go blocky.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Set to ~0.045 bpp"))
            d.session_bitrate_kbps = std::clamp(want_kbps, 4000, 120000);
    } else if (bpp < 0.040) {
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                           "  On the low side; %d kbps would be comfortable.", want_kbps);
    }

    // Encoder
    int enc_pos = 0;
    std::vector<const char*> enc_labels;
    for (int i = 0; i < (int)g_ui.encoders.size(); ++i) {
        enc_labels.push_back(g_ui.encoders[i].label.c_str());
        if (g_ui.encoders[i].id == d.encoder) enc_pos = i;
    }
    if (!enc_labels.empty()) {
        ImGui::SetNextItemWidth(px(320));
        if (ImGui::Combo("Encoder", &enc_pos, enc_labels.data(), (int)enc_labels.size()))
            d.encoder = g_ui.encoders[enc_pos].id;
    }
    ImGui::SameLine();
    if (ImGui::Button("Test")) {
        g_ui.settings_message =
            encoder_works(ctx.root / d.ffmpeg_path, d.encoder)
                ? d.encoder + " works on this machine."
                : d.encoder + " FAILED - pick another encoder.";
    }

    // Capture backend
    const char* backend_labels[] = {"Auto (recommended)", "Built-in capture (native)",
                                    "AMD zero-copy (vsrc_amf)",
                                    "NVIDIA zero-copy (scale_cuda)",
                                    "Intel zero-copy (scale_qsv)",
                                    "Desktop Duplication (ddagrab)"};
    const char* backend_ids[]    = {"auto", "native", "amf", "cuda", "qsv", "ddagrab"};
    constexpr int kBackendCount  = 6;
    int backend_pos = 0;
    for (int i = 0; i < kBackendCount; ++i)
        if (d.capture_backend == backend_ids[i]) backend_pos = i;
    ImGui::SetNextItemWidth(px(320));
    if (ImGui::Combo("Capture method", &backend_pos, backend_labels, kBackendCount))
        d.capture_backend = backend_ids[backend_pos];
    ImGui::SameLine();
    if (ImGui::Button("Test##backend")) {
        const auto b = capture_backend_from_string(d.capture_backend);
        if (b == CaptureBackend::Auto) {
            const auto picked = resolve_backend(b, detect_vendor(),
                                                ctx.root / d.ffmpeg_path, d.monitor_index);
            g_ui.settings_message = "Auto would use: " + to_string(picked);
        } else {
            g_ui.settings_message =
                backend_works(ctx.root / d.ffmpeg_path, b, d.monitor_index)
                    ? to_string(b) + " works on this machine."
                    : to_string(b) + " FAILED here; Auto would fall back to ddagrab.";
        }
    }
    ImGui::TextDisabled("  Auto uses the built-in capture, which grabs frames on its own");
    ImGui::TextDisabled("  thread and sends them on a fixed clock. The others capture");
    ImGui::TextDisabled("  inside ffmpeg and are kept for comparison.");

    ImGui::Checkbox("Capture audio", &d.capture_audio);
    ImGui::SameLine();
    ImGui::Checkbox("Draw cursor", &d.draw_mouse);

}

void section_audio(Settings& d) {
    // ---- audio devices ----
    if (d.capture_audio) {
        ImGui::SeparatorText("Audio devices");

        if (ImGui::Button("Rescan devices", px(150, 28))) {
            g_ui.output_devices = enumerate_output_devices();
            g_ui.input_devices  = enumerate_input_devices();
        }
        if (g_ui.output_devices.empty() && g_ui.input_devices.empty()) {
            g_ui.output_devices = enumerate_output_devices();
            g_ui.input_devices  = enumerate_input_devices();
        }

        ImGui::TextDisabled("Record from (tick more than one if your headset splits");
        ImGui::TextDisabled("game and chat across separate devices):");

        for (const auto& dev : g_ui.output_devices) {
            bool on = std::find(d.audio_outputs.begin(), d.audio_outputs.end(), dev.id)
                      != d.audio_outputs.end();
            std::string label = dev.name + (dev.is_default ? "   (system default)" : "");
            ImGui::PushID(dev.id.c_str());
            if (ImGui::Checkbox(label.c_str(), &on)) {
                if (on) d.audio_outputs.push_back(dev.id);
                else    d.audio_outputs.erase(
                            std::remove(d.audio_outputs.begin(), d.audio_outputs.end(), dev.id),
                            d.audio_outputs.end());
            }
            ImGui::PopID();
        }
        if (d.audio_outputs.empty())
            ImGui::TextDisabled("  Nothing ticked: the system default output is used.");

        // Microphone
        int mic_pos = 0;
        std::vector<const char*> mic_labels{"None"};
        std::vector<std::string> mic_ids{""};
        for (const auto& dev : g_ui.input_devices) {
            mic_labels.push_back(dev.name.c_str());
            mic_ids.push_back(dev.id);
        }
        for (int i = 0; i < (int)mic_ids.size(); ++i)
            if (mic_ids[i] == d.audio_input) mic_pos = i;
        ImGui::SetNextItemWidth(px(380));
        if (ImGui::Combo("Microphone", &mic_pos, mic_labels.data(), (int)mic_labels.size()))
            d.audio_input = mic_ids[mic_pos];

        if (!d.audio_input.empty()) {
            ImGui::SetNextItemWidth(px(320));
            ImGui::SliderInt("Mic level (%)", &d.mic_gain_percent, 0, 400);
            ImGui::TextDisabled("  Applied before mixing. 100 leaves it untouched.");

            const char* ch_labels[] = {"Stereo (leave as-is)",
                                       "Mono - copy left to both",
                                       "Mono - average both channels"};
            const char* ch_ids[]    = {"stereo", "mono_left", "mono_mix"};
            int ch_pos = 0;
            for (int i = 0; i < 3; ++i) if (d.mic_channel_mode == ch_ids[i]) ch_pos = i;
            ImGui::SetNextItemWidth(px(380));
            if (ImGui::Combo("Mic channels", &ch_pos, ch_labels, 3))
                d.mic_channel_mode = ch_ids[ch_pos];
            ImGui::TextDisabled("  If you only hear yourself in one ear, your mic is mono");
            ImGui::TextDisabled("  on a 2-channel device: pick \"copy left to both\".");
        }

        const char* track_labels[] = {"Mixed into one track",
                                      "Separate track per device"};
        const char* track_ids[]    = {"mixed", "separate"};
        int track_pos = 0;
        for (int i = 0; i < 2; ++i) if (d.audio_track_mode == track_ids[i]) track_pos = i;
        ImGui::SetNextItemWidth(px(380));
        if (ImGui::Combo("Tracks", &track_pos, track_labels, 2))
            d.audio_track_mode = track_ids[track_pos];
        ImGui::TextDisabled("  Mixed is ready to upload. Separate keeps voice and game");
        ImGui::TextDisabled("  apart for editing; most players only play the first track.");

        const size_t n = (d.audio_outputs.empty() ? 1 : d.audio_outputs.size()) +
                         (d.audio_input.empty() ? 0 : 1);
        ImGui::TextDisabled("  Recording %zu source%s -> %s.", n, n == 1 ? "" : "s",
                            (d.audio_track_mode == "separate" && n > 1)
                                ? "one track each" : "a single track");
    }
}

void section_clips(Settings& d) {

    // ---- clips ----
    ImGui::SeparatorText("Clips");

    ImGui::SetNextItemWidth(px(320));
    ImGui::SliderScalar("Target size (MB)", ImGuiDataType_Double, &d.target_clip_size_mb,
                        &kMinClipMb, &kMaxClipMb, "%.1f");
    ImGui::TextDisabled("  Discord: 10 MB free, 50 MB Nitro Basic, 500 MB Nitro.");
    ImGui::TextDisabled("  Counted as 10,000,000 bytes, so 9.9 is the practical maximum.");

    ImGui::Checkbox("Two-pass encode (fills the whole budget)", &d.clip_two_pass);
    ImGui::TextDisabled("  Off = single-pass CRF: faster, smaller files, lower quality.");

    const char* q_labels[] = {"Auto (fit to bitrate)", "Source", "1080p", "720p", "480p"};
    const char* q_ids[]    = {"auto", "source", "1080p", "720p", "480p"};
    int q_pos = 0;
    for (int i = 0; i < 5; ++i) if (d.clip_quality == q_ids[i]) q_pos = i;
    ImGui::SetNextItemWidth(px(320));
    if (ImGui::Combo("Clip resolution", &q_pos, q_labels, 5))
        d.clip_quality = q_ids[q_pos];

    const char* presets[] = {"veryfast", "faster", "fast", "medium",
                             "slow", "slower", "veryslow"};
    int p_pos = 6;
    for (int i = 0; i < 7; ++i) if (d.clip_preset == presets[i]) p_pos = i;
    ImGui::SetNextItemWidth(px(320));
    if (ImGui::Combo("Encode preset", &p_pos, presets, 7))
        d.clip_preset = presets[p_pos];
    // Measured two-pass on a 20 s 1080p60 clip, i5-13600K.
    ImGui::TextDisabled("  Slower = better quality per MB. For a 20s clip, two-pass:");
    ImGui::TextDisabled("  slow ~14s, slower ~24s, veryslow ~45s. Scale with length.");

    ImGui::BeginDisabled(d.clip_two_pass);
    ImGui::SetNextItemWidth(px(320));
    ImGui::SliderInt("Quality (CRF)", &d.clip_crf, 14, 30);
    ImGui::EndDisabled();
    if (d.clip_two_pass) ImGui::TextDisabled("  CRF is unused in two-pass mode.");

    ImGui::SetNextItemWidth(px(320));
    ImGui::SliderInt("Clip audio (kbps)", &d.clip_audio_kbps, 64, 192);

    // Equal min and max pins the rate; a lower min lets the planner trade frame
    // rate for resolution once a long clip cannot hold both.
    bool allow_half = d.clip_min_fps < d.clip_max_fps;
    if (ImGui::Checkbox("Allow half frame rate on long clips", &allow_half))
        d.clip_min_fps = allow_half ? std::max(1, d.clip_max_fps / 2) : d.clip_max_fps;
    ImGui::TextDisabled("  Keeps a sharper picture when a long clip cannot hold full rate.");


    // ---- folders ----
}

void section_folders(AppContext& ctx, Settings& d) {
    ImGui::SeparatorText("Folders");

    // Shows where the files actually land, since a relative setting is resolved
    // against the app root and an absolute one is used as-is.
    auto folder_row = [&](const char* label, std::string& value, const wchar_t* title) {
        ImGui::PushID(label);
        char buf[512]{};
        snprintf(buf, sizeof(buf), "%s", value.c_str());
        ImGui::SetNextItemWidth(px(380));
        if (ImGui::InputText("##path", buf, sizeof(buf))) value = buf;
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            const auto current = std::filesystem::path(value).is_absolute()
                                     ? std::filesystem::path(value)
                                     : ctx.root / value;
            if (auto picked = pick_folder(title, current))
                value = portable_path(*picked, ctx.root);
        }
        ImGui::SameLine();
        ImGui::Text("%s", label);

        const auto resolved = std::filesystem::path(value).is_absolute()
                                  ? std::filesystem::path(value)
                                  : ctx.root / value;
        std::error_code ec;
        const bool exists = std::filesystem::exists(resolved, ec);
        ImGui::TextDisabled("  -> %s%s", resolved.string().c_str(),
                            exists ? "" : "   (will be created)");
        ImGui::PopID();
    };

    folder_row("Recordings", d.sessions_dir, L"Choose where recordings are saved");
    folder_row("Clips",      d.clips_dir,    L"Choose where clips are saved");

    // Size caps. Pruning runs after a recording finishes, never during capture.
    const auto sess_bytes = folder_size(resolve_dir(ctx.root, d.sessions_dir));
    const auto clip_bytes = folder_size(resolve_dir(ctx.root, d.clips_dir));

    ImGui::SetNextItemWidth(px(320));
    ImGui::SliderScalar("Recordings size limit (GB)", ImGuiDataType_Double,
                        &d.max_sessions_gb, &kMinFolderGb, &kMaxFolderGb, "%.0f");
    ImGui::TextDisabled("  Currently %s. 0 = no limit; oldest are deleted first.",
                        format_size(sess_bytes).c_str());

    ImGui::SetNextItemWidth(px(320));
    ImGui::SliderScalar("Clips size limit (GB)", ImGuiDataType_Double,
                        &d.max_clips_gb, &kMinFolderGb, &kMaxFolderGb, "%.0f");
    ImGui::TextDisabled("  Currently %s.", format_size(clip_bytes).c_str());
    ImGui::TextDisabled("  A path inside the app folder is stored relative, so a copied or");
    ImGui::TextDisabled("  shared install keeps working. Anywhere else is stored absolute.");

    // ---- auto record ----
}

void section_auto_record(Settings& d) {
    ImGui::SeparatorText("Auto-record");

    ImGui::Checkbox("Start and stop with a game", &d.auto_record_enabled);
    ImGui::TextDisabled("  Watches the process list only. Nothing is injected into the game.");

    if (d.auto_record_enabled) {
        for (int i = 0; i < (int)d.auto_record_games.size(); ++i) {
            ImGui::PushID(1000 + i);
            ImGui::SetNextItemWidth(px(280));
            char buf[128]{};
            snprintf(buf, sizeof(buf), "%s", d.auto_record_games[i].c_str());
            if (ImGui::InputText("##game", buf, sizeof(buf)))
                d.auto_record_games[i] = buf;
            ImGui::SameLine();
            if (ImGui::Button("Remove")) {
                d.auto_record_games.erase(d.auto_record_games.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }

        if (ImGui::Button("Add game")) d.auto_record_games.push_back("");
        ImGui::SameLine();
        if (ImGui::Button("Pick from running processes")) {
            g_ui.process_list = GameWatch::running_processes();
            ImGui::OpenPopup("pick_process");
        }

        if (ImGui::BeginPopup("pick_process")) {
            ImGui::TextDisabled("Launch the game first, then pick it here.");
            ImGui::SetNextItemWidth(px(300));
            ImGui::InputText("Filter", g_ui.process_filter, sizeof(g_ui.process_filter));
            ImGui::BeginChild("proclist", px(320, 260));
            for (const auto& name : g_ui.process_list) {
                if (g_ui.process_filter[0]) {
                    std::string lower = name, needle = g_ui.process_filter;
                    // ::tolower takes and returns int, and is undefined for
                    // negative char values; go through unsigned char and cast
                    // the result back explicitly.
                    auto fold = [](std::string& s) {
                        std::transform(s.begin(), s.end(), s.begin(),
                                       [](unsigned char c) { return (char)std::tolower(c); });
                    };
                    fold(lower);
                    fold(needle);
                    if (lower.find(needle) == std::string::npos) continue;
                }
                if (ImGui::Selectable(name.c_str())) {
                    d.auto_record_games.push_back(name);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
    }

    // ---- overlay ----
}

void section_overlay(Settings& d) {
    ImGui::SeparatorText("Overlay");

    ImGui::Checkbox("Show recording overlay", &d.overlay_enabled);
    if (d.overlay_enabled) {
        int ov_pos = 0;
        std::vector<const char*> ov_labels{"Auto (a screen you are not recording)"};
        std::vector<int>         ov_values{-1};
        for (const auto& disp : g_ui.displays) {
            ov_labels.push_back(disp.label.c_str());
            ov_values.push_back(disp.index);
        }
        for (int i = 0; i < (int)ov_values.size(); ++i)
            if (ov_values[i] == d.overlay_monitor) ov_pos = i;
        ImGui::SetNextItemWidth(px(320));
        if (ImGui::Combo("Overlay screen", &ov_pos, ov_labels.data(), (int)ov_labels.size()))
            d.overlay_monitor = ov_values[ov_pos];
        ImGui::TextDisabled("  Shows elapsed time, live capture fps and file size.");
    }

    // ---- hotkey ----
}

void section_hotkeys(AppContext& ctx, Settings& d) {
    ImGui::SeparatorText("Hotkeys");
    ImGui::Text("Record toggle:   %s", describe_hotkey(d.hotkey_mods, d.hotkey_vk).c_str());
    ImGui::SameLine();
    if (ImGui::Button(g_ui.capturing_hotkey ? "Press a key...##rec" : "Rebind##rec")) {
        g_ui.capturing_hotkey = !g_ui.capturing_hotkey;
        g_ui.capturing_marker = false;
    }

    ImGui::Text("Mark highlight:  %s",
                describe_hotkey(d.marker_hotkey_mods, d.marker_hotkey_vk).c_str());
    ImGui::SameLine();
    if (ImGui::Button(g_ui.capturing_marker ? "Press a key...##mark" : "Rebind##mark")) {
        g_ui.capturing_marker = !g_ui.capturing_marker;
        g_ui.capturing_hotkey = false;
    }
    ImGui::TextDisabled("  Drops a marker on the timeline while recording, so the moment");
    ImGui::TextDisabled("  is easy to find afterwards. Also on the tray menu.");

    if (!ctx.hotkey_ok || !ctx.marker_hotkey_ok)
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "  %s could not be registered; another app already owns it.",
                           !ctx.hotkey_ok && !ctx.marker_hotkey_ok ? "Both hotkeys"
                           : !ctx.hotkey_ok                        ? "The record hotkey"
                                                                   : "The highlight hotkey");

    // Windows refuses to deliver a hotkey to a normal process while a window
    // belonging to an elevated one has focus. Games with kernel anti-cheat
    // (League of Legends and Valorant both run their client elevated) therefore
    // swallow every hotkey, while the buttons in this window keep working, which
    // makes it look like the hotkeys are broken rather than blocked.
    if (!ctx.elevated) {
        ImGui::TextDisabled("  If a hotkey does nothing inside a game but works elsewhere, that");
        ImGui::TextDisabled("  game is running as administrator. Windows will not pass hotkeys");
        ImGui::TextDisabled("  to Outriced unless it runs as administrator too.");
    }

    if (g_ui.capturing_marker) {
        ImGuiIO& io = ImGui::GetIO();
        for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
            if (!ImGui::IsKeyPressed((ImGuiKey)key, false)) continue;
            const unsigned vk = imgui_key_to_vk((ImGuiKey)key);
            if (vk == 0) continue;
            unsigned mods = 0;
            if (io.KeyCtrl)  mods |= 0x0002;
            if (io.KeyAlt)   mods |= 0x0001;
            if (io.KeyShift) mods |= 0x0004;
            if (mods == 0) {
                g_ui.settings_message = "Pick a combination with Ctrl, Alt or Shift.";
            } else if (mods == d.hotkey_mods && vk == d.hotkey_vk) {
                g_ui.settings_message = "That is already the record toggle.";
            } else {
                d.marker_hotkey_mods = mods;
                d.marker_hotkey_vk   = vk;
                g_ui.capturing_marker = false;
                g_ui.settings_message = "Marker hotkey set. Save to apply.";
            }
            break;
        }
    }

    if (g_ui.capturing_hotkey) {
        ImGuiIO& io = ImGui::GetIO();
        for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
            if (!ImGui::IsKeyPressed((ImGuiKey)key, false)) continue;
            const unsigned vk = imgui_key_to_vk((ImGuiKey)key);
            if (vk == 0) continue;   // ignore the modifiers themselves
            unsigned mods = 0;
            if (io.KeyCtrl)  mods |= 0x0002;  // MOD_CONTROL
            if (io.KeyAlt)   mods |= 0x0001;  // MOD_ALT
            if (io.KeyShift) mods |= 0x0004;  // MOD_SHIFT
            if (mods == 0) {
                g_ui.settings_message = "Pick a combination with Ctrl, Alt or Shift.";
            } else {
                d.hotkey_mods = mods;
                d.hotkey_vk   = vk;
                g_ui.capturing_hotkey = false;
                g_ui.settings_message = "Hotkey set. Save to apply.";
            }
            break;
        }
    }
}

void section_actions(AppContext& ctx, Settings& d) {
    // ---- actions ----
    ImGui::SeparatorText("");
    if (ImGui::Button("Save settings", px(140, 34))) {
        *ctx.settings = d;
        ctx.settings_dirty = true;
        g_ui.settings_message = "Saved. Capture changes apply to the next recording.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert", px(100, 34))) {
        refresh_settings_draft(ctx);
        g_ui.settings_message.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open sessions folder", px(170, 34)))
        open_folder(resolve_dir(ctx.root, d.sessions_dir));
    ImGui::SameLine();
    if (ImGui::Button("Open clips folder", px(150, 34)))
        open_folder(resolve_dir(ctx.root, d.clips_dir));
    ImGui::SameLine();
    if (ImGui::Button("Open logs", px(110, 34))) open_folder(log_directory());
    ImGui::TextDisabled("Logs and crash dumps live in logs\\ - send those when reporting a problem.");

    if (!g_ui.settings_message.empty())
        ImGui::TextWrapped("%s", g_ui.settings_message.c_str());
}

} // namespace

void draw_settings(AppContext& ctx) {
    if (!g_ui.settings_loaded) refresh_settings_draft(ctx);
    Settings& d = g_ui.draft;

    ImGui::TextDisabled("Detected GPU: %s", g_ui.detected_vendor.c_str());
    ImGui::Separator();

    section_capture(ctx, d);
    section_audio(d);
    section_clips(d);
    section_folders(ctx, d);
    section_auto_record(d);
    section_overlay(d);
    section_hotkeys(ctx, d);
    section_actions(ctx, d);
}

} // namespace oc
