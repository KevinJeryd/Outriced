#include "app/ui.h"

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
namespace {

constexpr double kMinClipMb  = 2.0;
constexpr double kMaxClipMb  = 500.0;   // full Nitro
constexpr double kMinFolderGb = 0.0;    // 0 disables pruning
constexpr double kMaxFolderGb = 1000.0;

// Set once per frame from AppContext so the size helpers can reach it.
float g_scale = 1.0f;

// Explicit widget sizes in logical pixels, scaled for the display.
ImVec2 px(float w, float h) { return ImVec2(w * g_scale, h * g_scale); }
float  px(float v)          { return v * g_scale; }

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

enum class Tab  { Sessions, Clips, Settings };
enum class View { List, Preview };

// One browsable library: sessions or clips. Both behave identically, so the
// list and preview code is written once and pointed at whichever is active.
struct Library {
    std::vector<Session> items;
    int                  selected = -1;
    View                 view = View::List;
    double               mark_in  = -1.0;
    double               mark_out = -1.0;
    int                  dragging = 0;   // 0 none, 1 in-handle, 2 out-handle

    std::mutex           scan_mutex;
    std::vector<Session> scanned;
    std::atomic<bool>    scanning{false};
    std::atomic<bool>    ready{false};
    std::thread          thread;
    bool                 marks_enabled = true;
};

struct UiState {
    Tab     tab = Tab::Sessions;
    Library sessions;
    Library clips;

    std::unordered_map<std::string, SDL_Texture*> thumbs;
    std::string status;

    // Delete confirmation. Holds the path rather than an index, so a rescan
    // between asking and confirming cannot delete the wrong file.
    std::filesystem::path pending_delete;
    std::string           pending_delete_name;
    bool                  open_delete_popup = false;

    // True while any modal is up. The video plays in a child window layered over
    // the ImGui surface, so a modal drawn underneath it is invisible while still
    // dimming the background and swallowing every click -- which reads exactly
    // like the app having frozen. The video is hidden for as long as one is open.
    bool                  modal_active = false;

    // Settings tab working copy, applied on Save.
    bool                     settings_loaded = false;
    Settings                 draft;
    std::vector<DisplayInfo> displays;
    std::vector<EncoderInfo> encoders;
    std::string              detected_vendor;
    std::string              settings_message;

    // Hotkey capture
    bool     capturing_hotkey = false;
    bool     capturing_marker = false;

    // Process picker for the auto-record watchlist
    std::vector<std::string> process_list;
    char                     process_filter[64] = {};

    // Audio endpoints, refreshed on demand rather than every frame.
    std::vector<AudioDevice> output_devices;
    std::vector<AudioDevice> input_devices;
};

UiState g_ui;

Library& active_library() {
    return g_ui.tab == Tab::Clips ? g_ui.clips : g_ui.sessions;
}

void start_scan(Library& lib, const AppContext& ctx, bool clips) {
    if (lib.scanning.exchange(true)) return;
    if (lib.thread.joinable()) lib.thread.join();

    Settings settings = *ctx.settings;
    auto root = ctx.root;
    lib.thread = std::thread([&lib, settings, root, clips] {
        auto found = clips ? scan_clips(settings, root) : scan_sessions(settings, root);
        for (const auto& s : found) ensure_thumbnail(s, settings, root);
        {
            std::lock_guard lock(lib.scan_mutex);
            lib.scanned = std::move(found);
        }
        lib.ready.store(true, std::memory_order_release);
        lib.scanning.store(false, std::memory_order_release);
    });
}

void absorb_scan(Library& lib) {
    if (!lib.ready.exchange(false, std::memory_order_acquire)) return;
    std::lock_guard lock(lib.scan_mutex);

    // Keep the selection pinned to the same file across a refresh.
    std::string previous;
    if (lib.selected >= 0 && lib.selected < (int)lib.items.size())
        previous = lib.items[lib.selected].file.string();

    lib.items = std::move(lib.scanned);
    lib.selected = -1;
    for (int i = 0; i < (int)lib.items.size(); ++i) {
        if (lib.items[i].file.string() == previous) { lib.selected = i; break; }
    }
    if (lib.view == View::Preview && lib.selected < 0) lib.view = View::List;
}

SDL_Texture* thumbnail_texture(const Session& sess, AppContext& ctx) {
    const std::string key = sess.file.string();
    if (auto it = g_ui.thumbs.find(key); it != g_ui.thumbs.end())
        return it->second;

    SDL_Texture* tex = nullptr;
    std::error_code ec;
    if (std::filesystem::exists(sess.thumbnail, ec)) {
        if (SDL_Surface* surf = SDL_LoadBMP(sess.thumbnail.string().c_str())) {
            tex = SDL_CreateTextureFromSurface(ctx.renderer, surf);
            SDL_DestroySurface(surf);
        }
    }
    g_ui.thumbs[key] = tex;  // cache misses too, so we retry at most once
    return tex;
}

void open_folder(const std::filesystem::path& dir) {
    SDL_OpenURL(("file:///" + dir.generic_string()).c_str());
}

// Removes the video and everything cached alongside it. The player must let go
// of the file first: mpv keeps it open while loaded, and Windows will refuse the
// delete rather than allowing it behind an open handle.
bool delete_video(const std::filesystem::path& file, AppContext& ctx) {
    ctx.player->stop();

    // Drop the cached texture so a later file of the same name is not shown the
    // old thumbnail.
    if (auto it = g_ui.thumbs.find(file.string()); it != g_ui.thumbs.end()) {
        if (it->second) SDL_DestroyTexture(it->second);
        g_ui.thumbs.erase(it);
    }

    std::error_code ec;
    const auto sidecar = std::filesystem::path(file).replace_extension(".json");
    const auto thumb   = file.parent_path() / ".thumbs" / (file.stem().wstring() + L".bmp");
    std::filesystem::remove(sidecar, ec);
    std::filesystem::remove(thumb, ec);
    std::filesystem::remove(file.parent_path() / (file.stem().wstring() + L".progress"), ec);

    ec.clear();
    // mpv releases the file asynchronously, so give it a moment before giving up.
    for (int i = 0; i < 20; ++i) {
        if (std::filesystem::remove(file, ec)) return true;
        SDL_Delay(25);
    }
    return !std::filesystem::exists(file, ec);
}

void ask_delete(const Session& s) {
    g_ui.pending_delete      = s.file;
    g_ui.pending_delete_name = s.display_name;
    g_ui.open_delete_popup   = true;
    // Set here rather than derived later: ImGui::IsPopupOpen resolves the name
    // against the current ID stack, so asking outside the window that owns the
    // popup quietly answers "no" and the video never gets out of the way.
    g_ui.modal_active        = true;
}

// ---------------------------------------------------------------- list view

void draw_list(Library& lib, AppContext& ctx, bool clips) {
    if (lib.scanning.load(std::memory_order_relaxed) && lib.items.empty()) {
        ImGui::TextDisabled("Scanning...");
        return;
    }
    if (lib.items.empty()) {
        ImGui::TextDisabled(clips ? "No clips yet. Cut one from a session."
                                  : "No sessions yet. Press the hotkey or Start recording.");
        return;
    }

    ImGui::BeginChild("list", px(0, 0));
    for (int i = 0; i < (int)lib.items.size(); ++i) {
        const Session& s = lib.items[i];
        ImGui::PushID(i);

        const float row_h = px(92.0f);
        const bool clicked = ImGui::Selectable("##row", lib.selected == i,
                                               ImGuiSelectableFlags_AllowDoubleClick,
                                               ImVec2(0, row_h));
        const bool double_clicked = clicked && ImGui::IsMouseDoubleClicked(0);

        ImVec2 row_min = ImGui::GetItemRectMin();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const float pad = px(6.0f);
        const float thumb_w = px(142.0f), thumb_h = row_h - pad * 2;
        if (SDL_Texture* tex = thumbnail_texture(s, ctx)) {
            dl->AddImage((ImTextureID)(intptr_t)tex,
                         ImVec2(row_min.x + pad, row_min.y + pad),
                         ImVec2(row_min.x + pad + thumb_w, row_min.y + pad + thumb_h));
        } else {
            dl->AddRectFilled(ImVec2(row_min.x + pad, row_min.y + pad),
                              ImVec2(row_min.x + pad + thumb_w, row_min.y + pad + thumb_h),
                              IM_COL32(40, 40, 46, 255));
        }

        const float tx = row_min.x + pad * 2 + thumb_w;
        dl->AddText(ImVec2(tx, row_min.y + pad + px(4.0f)), IM_COL32(235, 235, 240, 255),
                    s.display_name.c_str());

        std::string meta = format_duration(s.duration) + "   " + format_size(s.size_bytes);
        if (s.height > 0) meta += "   " + std::to_string(s.height) + "p";
        if (s.fps > 0)    meta += std::to_string(s.fps);
        dl->AddText(ImVec2(tx, row_min.y + pad + px(26.0f)), IM_COL32(150, 150, 160, 255),
                    meta.c_str());

        if (clicked) lib.selected = i;
        if (double_clicked) {
            lib.view     = View::Preview;
            lib.mark_in  = -1.0;
            lib.mark_out = -1.0;
            ctx.player->load(s.file);
        }

        // Right-click the row for per-item actions.
        if (ImGui::BeginPopupContextItem("row_menu")) {
            lib.selected = i;
            if (ImGui::MenuItem("Open")) {
                lib.view = View::Preview;
                lib.mark_in = lib.mark_out = -1.0;
                ctx.player->load(s.file);
            }
            if (ImGui::MenuItem("Show in folder")) open_folder(s.file.parent_path());
            ImGui::Separator();
            if (ImGui::MenuItem("Delete...")) ask_delete(s);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- preview

void draw_preview(Library& lib, AppContext& ctx) {
    if (lib.selected < 0 || lib.selected >= (int)lib.items.size()) {
        lib.view = View::List;
        return;
    }
    const Session& s = lib.items[lib.selected];
    Player& player = *ctx.player;

    if (ImGui::Button("< Back", px(90, 30))) {
        player.set_paused(true);
        player.set_visible(false);
        lib.view = View::List;
        return;
    }
    ImGui::SameLine();
    ImGui::Text("%s", s.display_name.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Show in folder", px(130, 30)))
        open_folder(s.file.parent_path());
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.15f, 0.15f, 1.0f));
    if (ImGui::Button("Delete", px(90, 30))) ask_delete(s);
    ImGui::PopStyleColor();

    const double duration = player.duration() > 0 ? player.duration() : s.duration;

    // Reserve the video area; the mpv child window is positioned over it.
    const float controls_h = lib.marks_enabled ? 132.0f : 76.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 video_pos = ImGui::GetCursorScreenPos();
    ImVec2 video_size(avail.x, std::max(120.0f, avail.y - controls_h));

    if (player.available() && !g_ui.modal_active) {
        ImVec2 win_pos = ImGui::GetMainViewport()->Pos;
        player.set_visible(true);
        player.set_rect((int)(video_pos.x - win_pos.x), (int)(video_pos.y - win_pos.y),
                        (int)video_size.x, (int)video_size.y);
    } else if (player.available()) {
        // A modal is up: get the video out of the way so the dialog is visible.
        player.set_visible(false);
        ImGui::GetWindowDrawList()->AddRectFilled(
            video_pos, ImVec2(video_pos.x + video_size.x, video_pos.y + video_size.y),
            IM_COL32(12, 12, 15, 255));
    } else {
        ImGui::GetWindowDrawList()->AddRectFilled(
            video_pos, ImVec2(video_pos.x + video_size.x, video_pos.y + video_size.y),
            IM_COL32(20, 20, 24, 255));
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(video_pos.x + 16, video_pos.y + 16), IM_COL32(200, 120, 120, 255),
            "libmpv unavailable - run tools/fetch_mpv.ps1 and rebuild");
    }
    ImGui::Dummy(video_size);

    // ---- transport ----
    if (ImGui::Button(player.paused() ? "Play" : "Pause", px(80, 30)))
        player.toggle_pause();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Space");
    ImGui::SameLine();
    if (ImGui::Button("-5s", px(46, 30))) player.step_seconds(-5.0);
    ImGui::SameLine();
    if (ImGui::Button("-1s", px(46, 30))) player.step_seconds(-1.0);
    ImGui::SameLine();
    if (ImGui::Button("+1s", px(46, 30))) player.step_seconds(1.0);
    ImGui::SameLine();
    if (ImGui::Button("+5s", px(46, 30))) player.step_seconds(5.0);
    ImGui::SameLine();
    ImGui::Text("%s / %s", format_duration(player.position()).c_str(),
                format_duration(duration).c_str());

    ImGui::SameLine();
    if (ImGui::Button(player.muted() ? "Unmute" : "Mute", px(76, 30)))
        player.set_muted(!player.muted());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(160));
    float vol = (float)player.volume();
    if (ImGui::SliderFloat("##volume", &vol, 0.0f, 100.0f, "vol %.0f%%"))
        player.set_volume(vol);

    float pos = (float)player.position();
    ImGui::SetNextItemWidth(-1);
    const bool scrubbed =
        ImGui::SliderFloat("##scrub", &pos, 0.0f, (float)std::max(0.1, duration), "");

    const ImVec2 bar_min = ImGui::GetItemRectMin();
    const ImVec2 bar_max = ImGui::GetItemRectMax();
    const bool   bar_hovered = ImGui::IsItemHovered();
    ImDrawList*  dl = ImGui::GetWindowDrawList();

    auto time_to_x = [&](double t) {
        const double clamped = std::clamp(t, 0.0, std::max(0.1, duration));
        return bar_min.x + (float)(clamped / std::max(0.1, duration)) * (bar_max.x - bar_min.x);
    };
    auto x_to_time = [&](float x) {
        const float w = std::max(1.0f, bar_max.x - bar_min.x);
        return std::clamp((double)((x - bar_min.x) / w) * std::max(0.1, duration),
                          0.0, std::max(0.1, duration));
    };

    // ---- selected range ----
    if (lib.marks_enabled && lib.mark_in >= 0 && lib.mark_out > lib.mark_in) {
        dl->AddRectFilled(ImVec2(time_to_x(lib.mark_in), bar_min.y),
                          ImVec2(time_to_x(lib.mark_out), bar_max.y),
                          IM_COL32(80, 170, 255, 70));
    }

    // ---- highlight markers ----
    if (!s.markers.empty() && duration > 0.0) {
        for (double m : s.markers) {
            if (m < 0 || m > duration) continue;
            const float x = time_to_x(m);
            dl->AddLine(ImVec2(x, bar_min.y), ImVec2(x, bar_max.y),
                        IM_COL32(255, 196, 64, 235), px(2.0f));
        }
    }

    // ---- draggable in/out handles ----
    //
    // Drawn and hit-tested by hand rather than as widgets: they have to sit on
    // top of the slider, and a real widget there would eat the clicks that are
    // meant to scrub.
    if (lib.marks_enabled) {
        const float grab = px(7.0f);
        struct Handle { double* value; ImU32 colour; int id; };
        Handle handles[] = {
            {&lib.mark_in,  IM_COL32(120, 220, 120, 255), 1},
            {&lib.mark_out, IM_COL32(255, 130, 120, 255), 2},
        };

        for (auto& h : handles) {
            if (*h.value < 0) continue;
            const float x = time_to_x(*h.value);
            dl->AddLine(ImVec2(x, bar_min.y - px(3.0f)), ImVec2(x, bar_max.y + px(3.0f)),
                        h.colour, px(3.0f));
            // A small tab makes the grab area obvious.
            dl->AddRectFilled(ImVec2(x - grab * 0.5f, bar_min.y - px(9.0f)),
                              ImVec2(x + grab * 0.5f, bar_min.y - px(1.0f)), h.colour, px(2.0f));
        }

        const ImVec2 mouse = ImGui::GetMousePos();
        if (ImGui::IsMouseClicked(0) && lib.dragging == 0) {
            for (auto& h : handles) {
                if (*h.value < 0) continue;
                if (std::abs(mouse.x - time_to_x(*h.value)) <= grab &&
                    mouse.y >= bar_min.y - px(10.0f) && mouse.y <= bar_max.y + px(4.0f)) {
                    lib.dragging = h.id;
                    break;
                }
            }
        }
        if (lib.dragging != 0) {
            if (ImGui::IsMouseDown(0)) {
                const double t = x_to_time(mouse.x);
                if (lib.dragging == 1)
                    lib.mark_in = (lib.mark_out > 0) ? std::min(t, lib.mark_out - 0.1) : t;
                else
                    lib.mark_out = (lib.mark_in >= 0) ? std::max(t, lib.mark_in + 0.1) : t;
            } else {
                // Land the playhead on whichever edge was just moved.
                player.seek_absolute(lib.dragging == 1 ? lib.mark_in : lib.mark_out);
                lib.dragging = 0;
            }
        }
    }

    // A drag on a handle must not also scrub the video.
    if (scrubbed && lib.dragging == 0) player.seek_absolute(pos);
    if (bar_hovered && lib.dragging == 0 && ImGui::IsMouseClicked(1)) {
        // Right-click on the bar sets the nearer edge, which is quicker than
        // driving the playhead to the exact spot first.
        const double t = x_to_time(ImGui::GetMousePos().x);
        if (lib.mark_in < 0 || std::abs(t - lib.mark_in) < std::abs(t - lib.mark_out))
            lib.mark_in = t;
        else
            lib.mark_out = t;
    }

    if (!s.markers.empty()) {
        ImGui::TextDisabled("%d highlight%s", (int)s.markers.size(),
                            s.markers.size() == 1 ? "" : "s");
        ImGui::SameLine();
        if (ImGui::Button("< Prev", px(78, 26))) {
            double best = -1.0;
            for (double m : s.markers)
                if (m < player.position() - 0.5 && m > best) best = m;
            if (best >= 0) player.seek_absolute(best);
        }
        ImGui::SameLine();
        if (ImGui::Button("Next >", px(78, 26))) {
            double best = -1.0;
            for (double m : s.markers)
                if (m > player.position() + 0.5 && (best < 0 || m < best)) best = m;
            if (best >= 0) player.seek_absolute(best);
        }
        // A highlight is the middle of the moment, not the start of it: the key
        // gets pressed after the thing happens.
        ImGui::SameLine();
        if (ImGui::Button("Clip around marker", px(170, 26))) {
            double nearest = s.markers.front();
            for (double m : s.markers)
                if (std::abs(m - player.position()) < std::abs(nearest - player.position()))
                    nearest = m;
            lib.mark_in  = std::max(0.0, nearest - 12.0);
            lib.mark_out = std::min(duration, nearest + 3.0);
            player.seek_absolute(lib.mark_in);
        }
    }

    if (!lib.marks_enabled) return;

    // ---- marking ----
    if (ImGui::Button("Mark In", px(90, 30)))  lib.mark_in  = player.position();
    ImGui::SameLine();
    if (ImGui::Button("Mark Out", px(90, 30))) lib.mark_out = player.position();
    ImGui::SameLine();

    const bool have_range = lib.mark_in >= 0 && lib.mark_out > lib.mark_in;
    if (have_range) {
        const double len = lib.mark_out - lib.mark_in;
        const auto pl = ClipExporter::plan(len, *ctx.settings,
                                           s.height > 0 ? s.height : 1080, s.fps);
        ImGui::Text("In %s  ->  Out %s   (%s  ->  %dp%d @ %d kbps)",
                    format_duration(lib.mark_in).c_str(),
                    format_duration(lib.mark_out).c_str(),
                    format_duration(len).c_str(), pl.height, pl.fps, pl.video_kbps);
    } else if (lib.mark_in >= 0) {
        ImGui::TextDisabled("In %s - now set Mark Out", format_duration(lib.mark_in).c_str());
    } else {
        ImGui::TextDisabled("Set Mark In and Mark Out to cut a clip");
    }

    ImGui::BeginDisabled(!have_range || ctx.exporter->busy());
    if (ImGui::Button("Save clip", px(120, 32))) {
        ClipExporter::Request req{s.file, lib.mark_in, lib.mark_out, s.height, s.fps};
        ctx.exporter->begin(req, *ctx.settings, ctx.root);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ctx.exporter->busy())
        ImGui::Text("Exporting...");
    else if (!ctx.exporter->status().empty())
        ImGui::TextWrapped("%s", ctx.exporter->status().c_str());
}

// ---------------------------------------------------------------- settings

void refresh_settings_draft(AppContext& ctx) {
    g_ui.draft    = *ctx.settings;
    g_ui.displays = enumerate_displays();
    g_ui.encoders = available_encoders(ctx.root / ctx.settings->ffmpeg_path);
    g_ui.detected_vendor = vendor_name(detect_vendor());
    g_ui.settings_loaded = true;
}

void draw_settings(AppContext& ctx) {
    if (!g_ui.settings_loaded) refresh_settings_draft(ctx);
    Settings& d = g_ui.draft;

    ImGui::TextDisabled("Detected GPU: %s", g_ui.detected_vendor.c_str());
    ImGui::Separator();

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

    ImGui::SetNextItemWidth(px(320));
    ImGui::SliderInt("Recording bitrate (kbps)", &d.session_bitrate_kbps, 4000, 60000);
    ImGui::TextDisabled("  ~%.0f MB per minute",
                        d.session_bitrate_kbps * 60.0 / 8192.0);

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
    const char* backend_labels[] = {"Auto (recommended)", "AMD zero-copy (vsrc_amf)",
                                    "NVIDIA zero-copy (scale_cuda)",
                                    "Intel zero-copy (scale_qsv)",
                                    "Desktop Duplication (ddagrab)"};
    const char* backend_ids[]    = {"auto", "amf", "cuda", "qsv", "ddagrab"};
    int backend_pos = 0;
    for (int i = 0; i < 5; ++i) if (d.capture_backend == backend_ids[i]) backend_pos = i;
    ImGui::SetNextItemWidth(px(320));
    if (ImGui::Combo("Capture method", &backend_pos, backend_labels, 5))
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
    ImGui::TextDisabled("  Auto keeps frames on the GPU when your vendor's path works,");
    ImGui::TextDisabled("  and falls back to Desktop Duplication, which runs anywhere.");

    ImGui::Checkbox("Capture audio", &d.capture_audio);
    ImGui::SameLine();
    ImGui::Checkbox("Draw cursor", &d.draw_mouse);

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
    ImGui::TextDisabled("  Keeps a sharper picture when a long clip cannot hold 60 fps.");

    // ---- folders ----
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

void request_session_refresh(const AppContext& ctx) {
    start_scan(g_ui.sessions, ctx, false);
}

void request_clip_refresh(const AppContext& ctx) {
    start_scan(g_ui.clips, ctx, true);
}

void set_status(const std::string& text) { g_ui.status = text; }

void draw_ui(AppContext& ctx) {
    g_scale = ctx.ui_scale > 0.1f ? ctx.ui_scale : 1.0f;

    absorb_scan(g_ui.sessions);
    absorb_scan(g_ui.clips);


    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);

    // ---- always-visible record control ----
    const bool recording = ctx.recorder->recording();
    if (recording) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Stop recording", px(150, 32)))
            ctx.toggle_recording_requested = true;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "REC  %s",
                           format_duration(ctx.recorder->elapsed()).c_str());
    } else {
        if (ImGui::Button("Start recording", px(150, 32)))
            ctx.toggle_recording_requested = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", describe_hotkey(ctx.settings->hotkey_mods,
                                                  ctx.settings->hotkey_vk).c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", px(80, 32))) {
        request_session_refresh(ctx);
        request_clip_refresh(ctx);
    }
    if (!ctx.hotkey_ok)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                           "Hotkey is taken by another app - use the tray icon or rebind it.");
    if (!g_ui.status.empty())
        ImGui::TextWrapped("%s", g_ui.status.c_str());

    // ---- tabs ----
    Tab previous_tab = g_ui.tab;
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Sessions")) {
            g_ui.tab = Tab::Sessions;
            g_ui.sessions.marks_enabled = true;
            if (g_ui.sessions.view == View::List) {
                ctx.player->set_visible(false);
                draw_list(g_ui.sessions, ctx, false);
            } else {
                draw_preview(g_ui.sessions, ctx);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Clips")) {
            g_ui.tab = Tab::Clips;
            // Clips are already cut, so no marking here -- just review them.
            g_ui.clips.marks_enabled = false;
            if (g_ui.clips.view == View::List) {
                ctx.player->set_visible(false);
                draw_list(g_ui.clips, ctx, true);
            } else {
                draw_preview(g_ui.clips, ctx);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            g_ui.tab = Tab::Settings;
            ctx.player->set_visible(false);
            ImGui::BeginChild("settings_scroll", px(0, 0));
            draw_settings(ctx);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // Leaving a tab should stop whatever it was playing.
    if (previous_tab != g_ui.tab) {
        ctx.player->set_paused(true);
        ctx.player->set_visible(false);
    }

    // ---- space toggles playback ----
    //
    // Guarded on no active item so it does not fire while a settings field or
    // the process filter has the keyboard.
    if (g_ui.tab != Tab::Settings && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        Library& lib = active_library();
        if (lib.view == View::Preview) ctx.player->toggle_pause();
    }

    // ---- delete confirmation ----
    if (g_ui.open_delete_popup) {
        ImGui::OpenPopup("Delete file?");
        g_ui.open_delete_popup = false;
    }
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Delete file?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Permanently delete this file?");
        ImGui::TextDisabled("%s", g_ui.pending_delete_name.c_str());
        ImGui::TextDisabled("This cannot be undone.");
        ImGui::Separator();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Delete", px(120, 32))) {
            Library& lib = active_library();
            const bool ok = delete_video(g_ui.pending_delete, ctx);
            g_ui.status = ok ? "Deleted " + g_ui.pending_delete_name
                             : "Could not delete " + g_ui.pending_delete_name +
                               " (is it open somewhere else?)";
            lib.view = View::List;
            ctx.player->set_visible(false);
            g_ui.pending_delete.clear();
            g_ui.modal_active = false;
            if (g_ui.tab == Tab::Clips) request_clip_refresh(ctx);
            else                        request_session_refresh(ctx);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", px(120, 32))) {
            g_ui.pending_delete.clear();
            g_ui.modal_active = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else if (!g_ui.open_delete_popup) {
        // Not open and not about to be: covers dismissal with Escape, which
        // would otherwise leave the video hidden for good.
        g_ui.modal_active = false;
        g_ui.pending_delete.clear();
    }

    ImGui::End();
}

void shutdown_ui() {
    if (g_ui.sessions.thread.joinable()) g_ui.sessions.thread.join();
    if (g_ui.clips.thread.joinable())    g_ui.clips.thread.join();
    for (auto& [key, tex] : g_ui.thumbs)
        if (tex) SDL_DestroyTexture(tex);
    g_ui.thumbs.clear();
}

} // namespace oc
