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

// A row of the tags currently filtered on, plus a popup to add another. Tags
// combine with AND: each one added narrows the list further.
void draw_tag_filter(Library& lib) {
    const auto& all = known_tags();
    if (all.empty() && lib.filter_tags.empty()) return;   // nothing tagged yet

    ImGui::TextDisabled("Filter:");
    ImGui::SameLine();

    for (int i = 0; i < (int)lib.filter_tags.size(); ++i) {
        ImGui::PushID(2000 + i);
        ImGui::TextUnformatted(lib.filter_tags[i].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            lib.filter_tags.erase(lib.filter_tags.begin() + i);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
        ImGui::SameLine();
    }

    if (ImGui::SmallButton("+ Tag")) ImGui::OpenPopup("filter_tag");
    if (!lib.filter_tags.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) lib.filter_tags.clear();
    }

    if (ImGui::BeginPopup("filter_tag")) {
        bool any = false;
        for (const auto& t : all) {
            bool already = false;
            for (const auto& f : lib.filter_tags)
                if (tags_equal(f, t)) { already = true; break; }
            if (already) continue;
            any = true;
            if (ImGui::Selectable(t.c_str())) {
                lib.filter_tags.push_back(t);
                ImGui::CloseCurrentPopup();
            }
        }
        if (!any) ImGui::TextDisabled("No other tags in use.");
        ImGui::EndPopup();
    }
}

// Shown only once more than one row is ticked, so the list looks unchanged
// until multi-select is actually being used.
void draw_selection_bar(Library& lib, AppContext& ctx) {
    (void)ctx;
    if (lib.marked.size() < 2) return;
    ImGui::Text("%d selected", (int)lib.marked.size());
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.15f, 0.15f, 1.0f));
    if (ImGui::SmallButton("Delete selected")) ask_delete_marked(lib);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        lib.marked.clear();
        if (lib.selected >= 0 && lib.selected < (int)lib.items.size())
            lib.marked.assign(1, lib.items[lib.selected].file);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(shift for a range, ctrl to pick individually)");
}

} // namespace

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

    draw_tag_filter(lib);

    // Rows that pass the filter, in display order. Built up front because a
    // shift-click ranges over what is on screen: with a tag filter active,
    // "everything in between" has to mean the visible rows, not the hidden
    // ones sitting between them in lib.items.
    std::vector<int> visible;
    visible.reserve(lib.items.size());
    for (int i = 0; i < (int)lib.items.size(); ++i)
        if (matches_filter(lib.items[i], lib.filter_tags)) visible.push_back(i);

    draw_selection_bar(lib, ctx);

    ImGui::BeginChild("list", px(0, 0));
    const int shown = (int)visible.size();
    for (int vi = 0; vi < shown; ++vi) {
        const int i = visible[vi];
        const Session& s = lib.items[i];
        ImGui::PushID(i);

        const float row_h = px(92.0f);
        const bool clicked = ImGui::Selectable("##row", is_marked(lib, s.file),
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

        if (clicked) {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyShift && lib.selected >= 0) {
                // Range from the anchor to here, over visible rows only. The
                // anchor may have been filtered out since it was set, in which
                // case there is no range to draw and this behaves as a plain
                // click.
                int anchor_vi = -1;
                for (int k = 0; k < shown; ++k)
                    if (visible[k] == lib.selected) { anchor_vi = k; break; }
                lib.marked.clear();
                if (anchor_vi < 0) {
                    lib.marked.push_back(s.file);
                    lib.selected = i;
                } else {
                    const int lo = std::min(anchor_vi, vi), hi = std::max(anchor_vi, vi);
                    for (int k = lo; k <= hi; ++k)
                        lib.marked.push_back(lib.items[visible[k]].file);
                }
            } else if (io.KeyCtrl) {
                // Toggle this one, leave the rest alone. The anchor follows the
                // last row touched so a later shift-click ranges from here.
                if (is_marked(lib, s.file))
                    std::erase(lib.marked, s.file);
                else
                    lib.marked.push_back(s.file);
                lib.selected = i;
            } else {
                lib.marked.assign(1, s.file);
                lib.selected = i;
            }
        }
        if (double_clicked) {
            lib.view     = View::Preview;
            lib.mark_in  = -1.0;
            lib.mark_out = -1.0;
            ctx.player->load(s.file);
        }

        // Right-click the row for per-item actions. Right-clicking something
        // that is part of a selection acts on the whole selection; right-
        // clicking outside one narrows to the row under the cursor, which is
        // what every file manager does.
        if (ImGui::BeginPopupContextItem("row_menu")) {
            if (!is_marked(lib, s.file)) {
                lib.marked.assign(1, s.file);
                lib.selected = i;
            }
            const int n = (int)lib.marked.size();
            ImGui::BeginDisabled(n > 1);
            if (ImGui::MenuItem("Open")) {
                lib.view = View::Preview;
                lib.mark_in = lib.mark_out = -1.0;
                ctx.player->load(s.file);
            }
            if (ImGui::MenuItem("Show in folder")) open_folder(s.file.parent_path());
            ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem(n > 1 ? "Delete selected..." : "Delete..."))
                ask_delete_marked(lib);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (shown == 0)
        ImGui::TextDisabled("No %s match the selected tags.", clips ? "clips" : "sessions");
    ImGui::EndChild();
}

} // namespace oc
