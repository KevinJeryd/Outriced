#pragma once
#include <string>

namespace oc {

// Small always-on-top status window, meant for a monitor you are not playing on.
// Plain Win32 and GDI rather than a second SDL window: it has to sit above a
// fullscreen game without stealing focus or a swapchain, and it must cost
// nothing to keep on screen while recording.
class Overlay {
public:
    ~Overlay();

    // `monitor_index` matches the DXGI ordering used everywhere else.
    bool show(int monitor_index);
    void hide();
    bool visible() const { return hwnd_ != nullptr; }

    // Key hints shown under the status line, so the bindings are visible at the
    // moment they are needed rather than remembered.
    void set_hints(const std::string& stop_key, const std::string& mark_key);

    // Elapsed seconds, capture fps (0 if unknown), the output size so far, and
    // how many highlights have been marked.
    void update(double elapsed, double fps, unsigned long long bytes, int markers);

    // Must be pumped from the owning thread each frame.
    void pump();

private:
    void*       hwnd_ = nullptr;
    std::string stop_key_ = "?", mark_key_ = "?";
    long long   last_paint_ms_ = 0;
    unsigned    paint_count_   = 0;
};

} // namespace oc
