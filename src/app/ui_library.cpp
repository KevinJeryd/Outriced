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

} // namespace oc
