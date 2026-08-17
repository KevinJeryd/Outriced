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
                           "Record hotkey is taken by another app - use the button or rebind it.");
    if (!ctx.marker_hotkey_ok)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                           "Highlight hotkey is taken by another app - rebind it in Settings.");
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
        const int n_pending = (int)g_ui.pending_delete.size();
        ImGui::Text(n_pending > 1 ? "Permanently delete these %d files?"
                                  : "Permanently delete this file?", n_pending);
        ImGui::TextDisabled("%s", g_ui.pending_delete_name.c_str());
        ImGui::TextDisabled("This cannot be undone.");
        ImGui::Separator();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Delete", px(120, 32))) {
            Library& lib = active_library();
            // One at a time, counting failures rather than stopping: a file the
            // player still holds should not prevent the rest from going.
            int done = 0, failed = 0;
            for (const auto& p : g_ui.pending_delete) {
                if (delete_video(p, ctx)) ++done;
                else                      ++failed;
            }
            g_ui.status = failed == 0
                ? "Deleted " + g_ui.pending_delete_name
                : "Deleted " + std::to_string(done) + ", could not delete " +
                  std::to_string(failed) + " (open somewhere else?)";
            lib.view = View::List;
            lib.marked.clear();
            lib.selected = -1;
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
