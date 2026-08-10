// Verifies that libmpv loads, embeds into a child window, and can open a
// session file and report its duration.
#include <windows.h>
#include <cstdio>

#include "media/player.h"
#include "media/sessions.h"
#include "app/settings.h"

int main() {
    const auto root = oc::app_root();
    const auto settings = oc::Settings::load(root / "settings.json");

    auto found = oc::scan_sessions(settings, root);
    if (found.empty()) { printf("no sessions to play\n"); return 1; }

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MpvTestHost";
    RegisterClassExW(&wc);
    HWND host = CreateWindowExW(0, wc.lpszClassName, L"mpv test", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 640, 360,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!host) { printf("could not create host window\n"); return 1; }

    oc::Player player;
    if (!player.init(host)) {
        printf("player init failed: %s\n", player.error().c_str());
        return 1;
    }
    printf("mpv initialised\n");

    player.set_visible(true);
    player.set_rect(0, 0, 640, 360);
    player.load(found.front().file);
    printf("loading  : %s\n", found.front().display_name.c_str());

    // Pump both message queues until mpv reports a duration.
    for (int i = 0; i < 300; ++i) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        player.update();
        if (player.duration() > 0.0) {
            printf("duration : %.2fs\n", player.duration());
            Sleep(700);           // let it actually play a moment
            player.update();
            printf("position : %.2fs (playing=%s)\n",
                   player.position(), player.paused() ? "no" : "yes");
            const bool advanced = player.position() > 0.0;
            player.seek_absolute(2.0);
            for (int k = 0; k < 60; ++k) { player.update(); Sleep(20); }
            printf("seek->2s : now %.2fs\n", player.position());

            // The delete button has to work while a file is loaded, and Windows
            // refuses to remove a file that still has an open handle. Load a
            // throwaway copy, then stop and delete it exactly as the UI does.
            std::error_code ec;
            const auto copy = found.front().file.parent_path() / L"_delete_probe.mp4";
            std::filesystem::copy_file(found.front().file, copy,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                printf("delete   : could not stage a copy (%s)\n", ec.message().c_str());
            } else {
                player.load(copy);
                for (int k = 0; k < 80; ++k) { player.update(); Sleep(25);
                                               if (player.duration() > 0) break; }
                printf("delete   : loaded the copy, now stopping and deleting\n");
                player.stop();

                bool removed = false;
                for (int k = 0; k < 20 && !removed; ++k) {
                    ec.clear();
                    removed = std::filesystem::remove(copy, ec);
                    if (!removed) Sleep(25);
                }
                printf("delete   : %s\n", removed ? "removed while loaded - OK"
                                                  : "FAILED, file still locked");
                if (!removed) std::filesystem::remove(copy, ec);
            }

            player.shutdown();
            DestroyWindow(host);
            printf(advanced ? "OK\n" : "WARN: playback did not advance\n");
            return advanced ? 0 : 2;
        }
        Sleep(20);
    }

    printf("timed out waiting for duration\n");
    player.shutdown();
    DestroyWindow(host);
    return 1;
}
