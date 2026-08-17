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

namespace {

// ::tolower takes and returns int and is undefined for negative char values,
// so every character goes through unsigned char first.
char fold_char(char c) { return (char)std::tolower((unsigned char)c); }

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.remove_prefix(1);
    while (!s.empty() && std::isspace((unsigned char)s.back()))  s.remove_suffix(1);
    return s;
}

} // namespace

bool tags_equal(std::string_view a, std::string_view b) {
    a = trimmed(a);
    b = trimmed(b);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (fold_char(a[i]) != fold_char(b[i])) return false;
    return true;
}

bool tag_contains(std::string_view haystack, std::string_view needle) {
    needle = trimmed(needle);
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && fold_char(haystack[i + k]) == fold_char(needle[k])) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}

bool has_tag(const Session& s, std::string_view tag) {
    for (const auto& t : s.tags)
        if (tags_equal(t, tag)) return true;
    return false;
}

const std::vector<std::string>& known_tags() {
    if (g_ui.tag_cache_valid) return g_ui.tag_cache;

    std::vector<std::string> out;
    auto absorb = [&](const std::vector<Session>& items) {
        for (const auto& s : items)
            for (const auto& t : s.tags) {
                if (trimmed(t).empty()) continue;
                bool seen = false;
                for (const auto& o : out) if (tags_equal(o, t)) { seen = true; break; }
                if (!seen) out.push_back(t);
            }
    };
    absorb(g_ui.sessions.items);
    absorb(g_ui.clips.items);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(),
            [](char x, char y) { return fold_char(x) < fold_char(y); });
    });

    g_ui.tag_cache       = std::move(out);
    g_ui.tag_cache_valid = true;
    return g_ui.tag_cache;
}

bool matches_filter(const Session& s, const std::vector<std::string>& filter) {
    for (const auto& want : filter)
        if (!has_tag(s, want)) return false;
    return true;
}

bool add_tag_to(Session& s, std::string_view tag) {
    const auto clean = trimmed(tag);
    if (clean.empty() || has_tag(s, clean)) return true;
    s.tags.emplace_back(clean);
    // Only clears the flag. The vector itself is untouched until the next
    // known_tags(), so a caller iterating it right now keeps a valid reference.
    g_ui.tag_cache_valid = false;
    if (save_tags(s)) return true;
    OC_LOG_W("[ui] could not write tags for {}", s.file.filename().string());
    return false;
}

bool remove_tag_from(Session& s, std::string_view tag) {
    const auto before = s.tags.size();
    std::erase_if(s.tags, [&](const std::string& t) { return tags_equal(t, tag); });
    if (s.tags.size() == before) return true;
    g_ui.tag_cache_valid = false;
    if (save_tags(s)) return true;
    OC_LOG_W("[ui] could not write tags for {}", s.file.filename().string());
    return false;
}

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
    g_ui.tag_cache_valid = false;   // the tags these items carry just changed

    // Drop anything ticked that is no longer in the folder, so "3 selected"
    // cannot outlive the files it counted.
    std::erase_if(lib.marked, [&](const std::filesystem::path& p) {
        for (const auto& s : lib.items) if (s.file == p) return false;
        return true;
    });

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
    g_ui.pending_delete      = {s.file};
    g_ui.pending_delete_name = s.display_name;
    g_ui.open_delete_popup   = true;
    // Set here rather than derived later: ImGui::IsPopupOpen resolves the name
    // against the current ID stack, so asking outside the window that owns the
    // popup quietly answers "no" and the video never gets out of the way.
    g_ui.modal_active        = true;
}

bool is_marked(const Library& lib, const std::filesystem::path& file) {
    for (const auto& p : lib.marked)
        if (p == file) return true;
    return false;
}

void ask_delete_marked(const Library& lib) {
    if (lib.marked.empty()) return;
    g_ui.pending_delete = lib.marked;
    g_ui.pending_delete_name =
        lib.marked.size() == 1
            ? lib.marked.front().stem().string()
            : std::to_string(lib.marked.size()) + " videos";
    g_ui.open_delete_popup = true;
    g_ui.modal_active      = true;
}

} // namespace oc
