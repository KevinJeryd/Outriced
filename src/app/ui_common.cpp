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

// The single instances the whole UI hangs off. Declared in ui_internal.h and
// defined here, so every screen sees the same state.
float   g_scale = 1.0f;
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

} // namespace oc
