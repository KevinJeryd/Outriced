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
namespace {

// Tags on the open video: existing ones as removable chips, then a popup that
// both picks an existing tag and creates a new one.
//
// Each edit writes the sidecar immediately rather than on leaving the screen. A
// rescan runs on a worker thread and replaces the whole item list when it lands,
// so anything still only in memory at that moment would be lost.
void draw_tag_editor(Session& s) {
    // Recomputed every frame, and this runs before the video is positioned, so
    // the flag is right for the frame it is read in.
    g_ui.tag_popup_open = false;

    ImGui::SeparatorText("Tags");

    for (int i = 0; i < (int)s.tags.size(); ++i) {
        // Without a unique id per chip every "x" button is the same widget and
        // they all respond to a click on any one of them.
        ImGui::PushID(i);
        ImGui::TextUnformatted(s.tags[i].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            remove_tag_from(s, s.tags[i]);
            ImGui::PopID();
            break;              // the vector just changed; stop iterating it
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    if (s.tags.empty()) {
        ImGui::TextDisabled("No tags");
        ImGui::SameLine();
    }

    if (ImGui::Button("Add tag")) {
        g_ui.tag_input[0] = '\0';
        ImGui::OpenPopup("add_tag");
    }

    if (ImGui::BeginPopup("add_tag")) {
        g_ui.tag_popup_open = true;
        ImGui::TextDisabled("Type a name and press Enter, or pick one below.");
        ImGui::SetNextItemWidth(px(260));

        // Focused on the frame the popup opens so it can be typed into straight
        // away; doing it every frame would fight the mouse.
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool entered = ImGui::InputText("##tag_name", g_ui.tag_input,
                                              sizeof(g_ui.tag_input),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        if (entered && g_ui.tag_input[0]) {
            add_tag_to(s, g_ui.tag_input);
            g_ui.tag_input[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

        const auto& all = known_tags();
        bool any_offered = false;
        for (const auto& t : all) {
            if (has_tag(s, t)) continue;               // already on this video
            if (!tag_contains(t, g_ui.tag_input)) continue;
            if (!any_offered) { ImGui::Separator(); any_offered = true; }
            if (ImGui::Selectable(t.c_str())) {
                add_tag_to(s, t);
                g_ui.tag_input[0] = '\0';
                ImGui::CloseCurrentPopup();
                break;      // `t` points into the list add_tag_to just invalidated
            }
        }
        ImGui::EndPopup();
    }
}

} // namespace

// ---------------------------------------------------------------- preview

void draw_preview(Library& lib, AppContext& ctx) {
    if (lib.selected < 0 || lib.selected >= (int)lib.items.size()) {
        lib.view = View::List;
        return;
    }
    // Non-const because the tag section edits this session in place. Everything
    // else here only reads it.
    Session& s = lib.items[lib.selected];
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

    draw_tag_editor(s);

    const double duration = player.duration() > 0 ? player.duration() : s.duration;

    // Reserve the video area; the mpv child window is positioned over it.
    const float controls_h = lib.marks_enabled ? 132.0f : 76.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 video_pos = ImGui::GetCursorScreenPos();
    ImVec2 video_size(avail.x, std::max(120.0f, avail.y - controls_h));

    if (player.available() && !g_ui.modal_active && !g_ui.tag_popup_open) {
        ImVec2 win_pos = ImGui::GetMainViewport()->Pos;
        player.set_visible(true);
        player.set_rect((int)(video_pos.x - win_pos.x), (int)(video_pos.y - win_pos.y),
                        (int)video_size.x, (int)video_size.y);
    } else if (player.available()) {
        // A modal or the tag popup is up: mpv's child window sits above
        // everything ImGui draws, so it has to go away for them to be visible.
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

    // Held down versus let go. While the handle is held every frame produces a
    // new value, so those seeks have to be the cheap kind; the accurate one is
    // issued once, when the drag ends.
    const bool scrub_held    = ImGui::IsItemActive();
    const bool scrub_release = ImGui::IsItemDeactivatedAfterEdit();

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
    if (lib.dragging == 0) {
        if (scrubbed && scrub_held) player.seek_absolute(pos, /*exact=*/false);
        else if (scrub_release)     player.seek_absolute(pos, /*exact=*/true);
        else if (scrubbed)          player.seek_absolute(pos, /*exact=*/true);
    }
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

} // namespace oc
