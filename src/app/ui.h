#pragma once
#include <filesystem>
#include <string>

struct SDL_Window;
struct SDL_Renderer;

namespace oc {

struct Settings;
class Recorder;
class Player;
class ClipExporter;

// Everything the UI needs from the rest of the app, so ui.cpp stays free of
// process and capture concerns.
struct AppContext {
    Settings*     settings = nullptr;
    Recorder*     recorder = nullptr;
    Player*       player   = nullptr;
    ClipExporter* exporter = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::filesystem::path root;

    // DPI scale of the window. Explicit widget sizes are multiplied by this;
    // ImGui's ScaleAllSizes only covers style metrics, not sizes passed by hand.
    float ui_scale  = 1.0f;

    bool  hotkey_ok = true;
    bool  quit      = false;
    bool  toggle_recording_requested = false;  // set by the UI, consumed by main
    bool  settings_dirty = false;              // settings changed; main persists
};

// Builds one frame of UI. Call between ImGui::NewFrame and ImGui::Render.
void draw_ui(AppContext& ctx);

// Rescan from disk on a worker thread; safe to call whenever a recording or
// export finishes.
void request_session_refresh(const AppContext& ctx);
void request_clip_refresh(const AppContext& ctx);

// One-line message shown above the tabs.
void set_status(const std::string& text);

// Releases cached thumbnail textures. Call before the renderer is destroyed.
void shutdown_ui();

} // namespace oc
