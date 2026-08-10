#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace oc {

enum class TrayEvent {
    ToggleRecording,   // hotkey, or "Start/Stop" from the menu
    MarkHighlight,     // hotkey: drop a marker at the current recording position
    ShowWindow,        // tray icon double-clicked, or "Open"
    Quit,
};

// Hidden message window that owns the tray icon and the global hotkey. It runs
// its own message loop on a dedicated thread so it stays responsive while SDL
// drives the main thread.
class Tray {
public:
    ~Tray();

    bool start(unsigned hotkey_mods, unsigned hotkey_vk,
               unsigned marker_mods, unsigned marker_vk);
    void stop();

    // Drains the events raised since the last call.
    std::vector<TrayEvent> poll();

    // Updates the icon tooltip and menu wording.
    void set_recording(bool recording);

    // False when RegisterHotKey lost to another app that already owns the combo.
    bool hotkey_registered() const { return hotkey_ok_.load(std::memory_order_relaxed); }
    bool marker_hotkey_registered() const { return marker_ok_.load(std::memory_order_relaxed); }

    // Called from the window procedure on the tray thread.
    void push(TrayEvent e);
    bool is_recording() const { return recording_.load(std::memory_order_relaxed); }

private:
    void run(unsigned mods, unsigned vk, unsigned marker_mods, unsigned marker_vk);

    std::thread            thread_;
    std::atomic<bool>      recording_{false};
    std::atomic<bool>      hotkey_ok_{false};
    std::atomic<bool>      marker_ok_{false};
    std::atomic<void*>     hwnd_{nullptr};
    std::mutex             mutex_;
    std::vector<TrayEvent> events_;
};

// "Alt+Shift+F8" for display, from Win32 modifier and virtual-key codes.
std::string describe_hotkey(unsigned mods, unsigned vk);

} // namespace oc
