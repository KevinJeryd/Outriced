#pragma once
// Shared internals of the UI, split across ui_*.cpp. Not part of the app's
// interface: everything the rest of the program uses is in app/ui.h.
//
// This exists because the whole UI used to sit in one anonymous namespace in
// ui.cpp, which meant nothing could be moved out of that file without first
// giving it a name. The state below is a single process-wide instance for the
// same reason ImGui itself is: there is exactly one window, drawn from one
// thread, and threading it through every draw call bought nothing.
#include <SDL3/SDL.h>
#include <imgui.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/settings.h"
#include "app/ui.h"
#include "capture/audio_devices.h"
#include "capture/displays.h"
#include "capture/encoders.h"
#include "media/sessions.h"

namespace oc {

// Set once per frame from AppContext so the size helpers can reach it.
extern float g_scale;

// Explicit widget sizes in logical pixels, scaled for the display.
inline ImVec2 px(float w, float h) { return ImVec2(w * g_scale, h * g_scale); }
inline float  px(float v)          { return v * g_scale; }

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

extern UiState g_ui;

// ---- ui_common.cpp: state shared by every screen -------------------------

Library& active_library();
void     start_scan(Library& lib, const AppContext& ctx, bool clips);
void     absorb_scan(Library& lib);
void     open_folder(const std::filesystem::path& dir);
bool     delete_video(const std::filesystem::path& file, AppContext& ctx);
void     ask_delete(const Session& s);

// ---- one file per screen -------------------------------------------------

void draw_list(Library& lib, AppContext& ctx, bool clips);   // ui_library.cpp
void draw_preview(Library& lib, AppContext& ctx);            // ui_preview.cpp
void draw_settings(AppContext& ctx);                         // ui_settings.cpp

} // namespace oc
