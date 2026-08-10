#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <windows.h>

#include "media/clipper.h"
#include "platform/log.h"
#include "capture/displays.h"
#include "capture/game_watch.h"
#include "platform/overlay.h"
#include "media/player.h"
#include "capture/recorder.h"
#include "media/sessions.h"
#include "app/settings.h"
#include "platform/tray.h"
#include "app/ui.h"

namespace {

// The window starts hidden: this lives in the tray, and only surfaces when a
// recording finishes or the user asks for it.
SDL_Window*   g_window   = nullptr;
SDL_Renderer* g_renderer = nullptr;

void surface_window() {
    if (!g_window) return;
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
    if (HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_window),
                                                 SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
        // SDL_RaiseWindow alone often loses to the foreground-lock rules when
        // the click came from the tray rather than from this window.
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
    }
}

} // namespace

int main(int, char**) {
    const auto root = oc::app_root();

    // Before anything else: a crash during start-up is exactly the kind that is
    // impossible to diagnose without a dump.
    oc::log_init(root);
    oc::install_crash_handler();
    OC_LOG_I("[app] Outriced starting, root={}", root.string());

    const auto settings_file = root / "settings.json";
    oc::Settings settings = oc::Settings::load(settings_file);

    // Fills in the encoder on first run and repairs a monitor index that points
    // at a display which is no longer plugged in.
    settings.resolve_hardware(root);
    settings.save(settings_file);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        MessageBoxA(nullptr, SDL_GetError(), "Outriced", MB_ICONERROR);
        return 1;
    }

    // Opens as a normal window. Closing it hides to the tray, where the recorder
    // keeps running; Quit from the tray menu exits for real.
    g_window = SDL_CreateWindow("Outriced", 1180, 760, SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        MessageBoxA(nullptr, SDL_GetError(), "Outriced", MB_ICONERROR);
        SDL_Quit();
        return 1;
    }

    g_renderer = SDL_CreateRenderer(g_window, nullptr);
    if (!g_renderer) {
        MessageBoxA(nullptr, SDL_GetError(), "Outriced", MB_ICONERROR);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(g_renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini clutter next to the exe
    ImGui::StyleColorsDark();
    ImGui::GetStyle().FrameRounding  = 4.0f;
    ImGui::GetStyle().WindowRounding = 0.0f;
    // The manifest makes this process DPI-aware, so Windows no longer stretches
    // the UI for us. On a 150% display everything would otherwise render a third
    // too small.
    float ui_scale = SDL_GetWindowDisplayScale(g_window);
    if (ui_scale <= 0.1f) ui_scale = 1.0f;
    if (ui_scale > 1.01f) {
        ImGui::GetStyle().ScaleAllSizes(ui_scale);
        ImGui::GetIO().FontGlobalScale = ui_scale;
        SDL_SetWindowSize(g_window, (int)(1180 * ui_scale), (int)(760 * ui_scale));
    }

    ImGui_ImplSDL3_InitForSDLRenderer(g_window, g_renderer);
    ImGui_ImplSDLRenderer3_Init(g_renderer);

    oc::Recorder     recorder;
    oc::ClipExporter exporter;
    oc::Player       player;
    oc::Tray         tray;
    oc::GameWatch    game_watch;
    oc::Overlay      overlay;

    // Picks a monitor for the overlay that is not the one being captured, so it
    // never ends up recorded inside the video.
    auto overlay_monitor_for = [&](const oc::Settings& s) {
        if (s.overlay_monitor >= 0) return s.overlay_monitor;
        for (const auto& d : oc::enumerate_displays())
            if (d.index != s.monitor_index) return d.index;
        return s.monitor_index;
    };

    HWND main_hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_window),
                                                  SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    player.init(main_hwnd);

    tray.start(settings.hotkey_mods, settings.hotkey_vk,
               settings.marker_hotkey_mods, settings.marker_hotkey_vk);

    oc::AppContext ctx;
    ctx.settings  = &settings;
    ctx.recorder  = &recorder;
    ctx.player    = &player;
    ctx.exporter  = &exporter;
    ctx.renderer  = g_renderer;
    ctx.root      = root;
    ctx.ui_scale  = ui_scale > 0.1f ? ui_scale : 1.0f;
    ctx.hotkey_ok = tray.hotkey_registered();

    oc::request_session_refresh(ctx);
    oc::request_clip_refresh(ctx);
    surface_window();

    auto stop_recording = [&](bool surface) {
        const int resumes = recorder.resume_count();
        const auto file = recorder.stop();
        OC_LOG_I("[rec] stopped; {} segment(s)", recorder.segments().size());
        // Housekeeping runs after the file is closed, never during capture.
        const int pruned = oc::prune_folder(oc::resolve_dir(root, settings.sessions_dir),
                                            settings.max_sessions_gb);
        if (pruned > 0) OC_LOG_I("[prune] removed {} old session(s)", pruned);
        tray.set_recording(false);
        overlay.hide();
        std::string msg = file ? "Saved " + file->filename().string()
                               : "Recording failed: " + recorder.last_error();
        if (resumes > 0)
            msg += "  (capture was interrupted " + std::to_string(resumes) +
                   "x and resumed; see the extra _pt files)";
        if (pruned > 0) msg += "  [" + std::to_string(pruned) + " old session(s) pruned]";
        oc::set_status(msg);
        oc::request_session_refresh(ctx);
        if (surface) surface_window();
    };

    auto start_recording = [&] {
        if (!recorder.start(settings, root)) {
            oc::set_status("Could not start: " + recorder.last_error());
            surface_window();
            return;
        }
        tray.set_recording(true);
        const std::string via = " [" + recorder.backend() + " / " + settings.encoder + "]";
        oc::set_status(recorder.last_error().empty()
                           ? "Recording..." + via
                           : "Recording (" + recorder.last_error() + ")" + via);
        if (settings.overlay_enabled) {
            overlay.set_hints(oc::describe_hotkey(settings.hotkey_mods, settings.hotkey_vk),
                              oc::describe_hotkey(settings.marker_hotkey_mods,
                                                  settings.marker_hotkey_vk));
            overlay.show(overlay_monitor_for(settings));
        }
    };

    auto toggle_recording = [&] {
        if (recorder.recording()) stop_recording(true);
        else                      start_recording();
    };

    if (settings.auto_record_enabled) game_watch.set_watchlist(settings.auto_record_games);

    unsigned    active_hotkey_mods  = settings.hotkey_mods;
    unsigned    active_hotkey_vk    = settings.hotkey_vk;
    std::string active_sessions_dir = settings.sessions_dir;
    std::string active_clips_dir    = settings.clips_dir;
    bool        was_exporting       = false;
    int         last_resume_count   = 0;

    // Redraw interval while the window is visible but unfocused. Nothing on
    // screen changes faster than this when nobody is interacting with it.
    constexpr Uint64 kUnfocusedRenderMs = 500;
    Uint64           last_render_ms     = 0;

    auto handle_event = [&](const SDL_Event& ev) {
        ImGui_ImplSDL3_ProcessEvent(&ev);
        if (ev.type == SDL_EVENT_QUIT) {
            // Closing the window keeps the recorder alive in the tray.
            SDL_HideWindow(g_window);
            player.set_visible(false);
        } else if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                   ev.window.windowID == SDL_GetWindowID(g_window)) {
            SDL_HideWindow(g_window);
            player.set_visible(false);
        }
    };

    bool running = true;
    while (running) {
        // How long to sleep is a *polling* decision, not a rendering one. Several
        // things are serviced once per iteration and their latency is this
        // period: the tray's hotkey queue, the check that notices ffmpeg has
        // died and starts a resume, and the overlay's readout. While recording,
        // the hotkey being waited on is usually the stop, and slow death
        // detection costs footage across the seam, so the period tightens.
        //
        // SDL_WaitEventTimeout rather than a sleep, because it returns the
        // moment input arrives. A click on an unfocused window wakes it at once,
        // so the timeout only governs how often the loop turns with no input.
        //
        // SDL_WindowFlags is 64-bit; narrowing it drops the high flags.
        const SDL_WindowFlags flags = SDL_GetWindowFlags(g_window);
        const bool hidden  = (flags & SDL_WINDOW_HIDDEN) != 0;
        const bool focused = (flags & SDL_WINDOW_INPUT_FOCUS) != 0;

        int wait_ms = 0;                 // focused: vsync already paces the loop
        if (hidden)        wait_ms = 250;
        else if (!focused) wait_ms = recorder.recording() ? 250 : 500;

        SDL_Event ev;
        if (wait_ms > 0 && SDL_WaitEventTimeout(&ev, wait_ms))
            handle_event(ev);
        while (SDL_PollEvent(&ev))
            handle_event(ev);

        for (auto e : tray.poll()) {
            switch (e) {
            case oc::TrayEvent::ToggleRecording: toggle_recording(); break;
            case oc::TrayEvent::MarkHighlight:
                if (recorder.recording()) {
                    recorder.mark_highlight();
                    oc::set_status("Highlight marked at " +
                                   oc::format_duration(recorder.elapsed()));
                }
                break;
            case oc::TrayEvent::ShowWindow:      surface_window();   break;
            case oc::TrayEvent::Quit:            running = false;    break;
            }
        }

        player.update();

        // Auto start/stop from the game process list.
        if (settings.auto_record_enabled) {
            switch (game_watch.poll(recorder.recording())) {
            case oc::GameWatch::Event::Started:
                if (!recorder.recording()) {
                    start_recording();
                    oc::set_status("Auto-started for " + game_watch.current_game());
                }
                break;
            case oc::GameWatch::Event::Stopped:
                if (recorder.recording()) stop_recording(true);
                break;
            default: break;
            }
        }

        // Polled on the session being active, not on ffmpeg being alive: this is
        // what notices the capture dying, so gating it on a live process would
        // make it unreachable in exactly the case it exists for.
        if (recorder.recording()) {
            recorder.refresh_progress();

            if (recorder.session_failed()) {
                OC_LOG_W("[app] capture could not be resumed; ending the session");
                stop_recording(true);
            } else {
                if (recorder.resuming()) {
                    oc::set_status("Capture interrupted (display changed) - resuming...");
                } else if (recorder.resume_count() > last_resume_count) {
                    last_resume_count = recorder.resume_count();
                    oc::set_status("Capture was interrupted and resumed (part " +
                                   std::to_string(last_resume_count + 1) + ")");
                    oc::request_session_refresh(ctx);
                }
                if (overlay.visible()) {
                    overlay.update(recorder.elapsed(), recorder.capture_fps(),
                                   recorder.output_bytes(), (int)recorder.markers().size());
                    overlay.pump();
                }
            }
        } else {
            last_resume_count = 0;   // ready for the next session
        }

        // Nothing to draw while hidden. The wait above already idled, so there
        // is no sleep here; re-read the flag because an event may have just
        // shown the window.
        if ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_HIDDEN) != 0)
            continue;

        // Rendering is gated separately from the polling above, because the two
        // want different rates. Polling has to stay brisk while recording;
        // drawing does not, and drawing is the expensive half. Unfocused, this
        // window shows a clock and an fps readout, so twice a second is ample
        // however often the loop happens to turn.
        //
        // Focused, this is skipped entirely and vsync sets the pace, exactly as
        // before.
        if (!focused) {
            const Uint64 now_ms = SDL_GetTicks();
            if (now_ms - last_render_ms < kUnfocusedRenderMs) continue;
            last_render_ms = now_ms;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ctx.toggle_recording_requested = false;
        ctx.settings_dirty = false;
        oc::draw_ui(ctx);
        if (ctx.toggle_recording_requested) toggle_recording();

        if (ctx.settings_dirty) {
            const bool folders_moved = settings.sessions_dir != active_sessions_dir ||
                                       settings.clips_dir    != active_clips_dir;
            settings.save(settings_file);

            if (folders_moved) {
                active_sessions_dir = settings.sessions_dir;
                active_clips_dir    = settings.clips_dir;
                std::error_code ec;
                std::filesystem::create_directories(
                    oc::resolve_dir(root, settings.sessions_dir), ec);
                std::filesystem::create_directories(
                    oc::resolve_dir(root, settings.clips_dir), ec);
                oc::request_session_refresh(ctx);
                oc::request_clip_refresh(ctx);
            }
            game_watch.set_watchlist(settings.auto_record_enabled ? settings.auto_record_games
                                                                  : std::vector<std::string>{});
            if (!settings.overlay_enabled) overlay.hide();
            else if (recorder.recording() && !overlay.visible())
                overlay.show(overlay_monitor_for(settings));
            // Re-register the hotkey in place if it changed, so the new binding
            // works without a restart.
            if (settings.hotkey_mods != active_hotkey_mods ||
                settings.hotkey_vk   != active_hotkey_vk) {
                tray.stop();
                tray.start(settings.hotkey_mods, settings.hotkey_vk,
               settings.marker_hotkey_mods, settings.marker_hotkey_vk);
                tray.set_recording(recorder.recording());
                active_hotkey_mods = settings.hotkey_mods;
                active_hotkey_vk   = settings.hotkey_vk;
                ctx.hotkey_ok      = tray.hotkey_registered();
            }
        }

        // Pick up a clip as soon as an export finishes.
        if (was_exporting && !exporter.busy()) {
            oc::request_clip_refresh(ctx);
            was_exporting = false;
        } else if (exporter.busy()) {
            was_exporting = true;
        }

        if (ctx.quit) running = false;

        ImGui::Render();
        SDL_SetRenderDrawColorFloat(g_renderer, 0.09f, 0.09f, 0.11f, 1.0f);
        SDL_RenderClear(g_renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
        SDL_RenderPresent(g_renderer);
    }

    if (recorder.recording()) recorder.stop();
    overlay.hide();
    tray.stop();
    player.shutdown();

    OC_LOG_I("[app] exiting");
    oc::shutdown_ui();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    oc::log_shutdown();
    return 0;
}
