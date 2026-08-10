#include "platform/overlay.h"

#include <windows.h>

#include "capture/displays.h"
#include "media/sessions.h"
#include "platform/subprocess.h"

namespace oc {
namespace {

constexpr wchar_t kClass[] = L"OutricedOverlay";
constexpr int kWidth  = 300;
constexpr int kHeight = 112;

struct Payload {
    std::string line1, line2, line3, line4;
};

// GDI objects are created once and reused. The first version built two fonts
// and two brushes on every paint, at up to 60 paints a second: tens of thousands
// of allocations a minute against a per-session GDI heap that the whole desktop
// shares, which is the sort of pressure that takes explorer down with it.
struct Gdi {
    HFONT  big    = nullptr;
    HFONT  small  = nullptr;
    HBRUSH bg     = nullptr;
    HBRUSH accent = nullptr;

    void ensure() {
        if (big) return;
        big = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        small = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        bg     = CreateSolidBrush(RGB(16, 16, 20));
        accent = CreateSolidBrush(RGB(220, 60, 60));
    }
    void release() {
        if (big)    { DeleteObject(big);    big = nullptr; }
        if (small)  { DeleteObject(small);  small = nullptr; }
        if (bg)     { DeleteObject(bg);     bg = nullptr; }
        if (accent) { DeleteObject(accent); accent = nullptr; }
    }
};

Gdi g_gdi;

LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        auto* p = reinterpret_cast<Payload*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);

        g_gdi.ensure();

        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, g_gdi.bg);

        // A red rule down the side reads as "recording" at a glance.
        RECT bar{0, 0, 5, rc.bottom};
        FillRect(dc, &bar, g_gdi.accent);

        SetBkMode(dc, TRANSPARENT);
        HFONT font  = g_gdi.big;
        HFONT small = g_gdi.small;

        if (p) {
            HGDIOBJ old = SelectObject(dc, font);
            SetTextColor(dc, RGB(240, 240, 245));
            RECT r1{16, 6, rc.right - 8, 34};
            const auto w1 = to_wide(p->line1);
            DrawTextW(dc, w1.c_str(), -1, &r1, DT_LEFT | DT_SINGLELINE);

            SelectObject(dc, small);
            SetTextColor(dc, RGB(160, 160, 172));
            RECT r2{16, 36, rc.right - 8, 58};
            const auto w2 = to_wide(p->line2);
            DrawTextW(dc, w2.c_str(), -1, &r2, DT_LEFT | DT_SINGLELINE);

            // Key hints, dimmer still so they never compete with the status.
            SetTextColor(dc, RGB(125, 125, 138));
            RECT r3{16, 62, rc.right - 8, 84};
            const auto w3 = to_wide(p->line3);
            DrawTextW(dc, w3.c_str(), -1, &r3, DT_LEFT | DT_SINGLELINE);

            RECT r4{16, 84, rc.right - 8, 106};
            const auto w4 = to_wide(p->line4);
            DrawTextW(dc, w4.c_str(), -1, &r4, DT_LEFT | DT_SINGLELINE);
            SelectObject(dc, old);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    // Never take focus or clicks; the overlay is decoration over a game.
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;

    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

Overlay::~Overlay() { hide(); }

bool Overlay::show(int monitor_index) {
    if (hwnd_) return true;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = overlay_proc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    // Place it near the top-right of the requested display.
    //
    // DXGI reports desktop coordinates in physical pixels, but this process is
    // DPI-unaware, so CreateWindow works in Windows' virtualised space -- on a
    // 150% display the two differ by that factor and using DXGI's numbers puts
    // the overlay off-screen. Win32's own monitor rect is already in the right
    // space, so DXGI is used only to identify *which* display, matched across
    // by device name, and Win32 supplies the geometry.
    int x = 40, y = 40;
    std::string want;
    for (const auto& d : enumerate_displays()) {
        if (d.index == monitor_index) { want = d.device_name; break; }
    }

    struct Ctx { const std::string* want; int x, y; bool found; } ctx{&want, x, y, false};
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(param);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(mon, &mi)) return TRUE;
            if (to_utf8(mi.szDevice) != *c->want) return TRUE;
            c->x = mi.rcMonitor.right - kWidth - 40;
            c->y = mi.rcMonitor.top + 40;
            c->found = true;
            return FALSE;
        }, (LPARAM)&ctx);

    if (ctx.found) { x = ctx.x; y = ctx.y; }

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kClass, L"Recording", WS_POPUP,
        x, y, kWidth, kHeight,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return false;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      (LONG_PTR)new Payload{"REC  0:00", "starting...",
                                            stop_key_ + "  stop",
                                            mark_key_ + "  mark highlight"});
    SetLayeredWindowAttributes(hwnd, 0, 225, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    hwnd_ = hwnd;
    return true;
}

void Overlay::hide() {
    if (!hwnd_) return;
    auto* p = reinterpret_cast<Payload*>(GetWindowLongPtrW((HWND)hwnd_, GWLP_USERDATA));
    delete p;
    DestroyWindow((HWND)hwnd_);
    hwnd_ = nullptr;
    g_gdi.release();
    last_paint_ms_ = 0;
}

void Overlay::set_hints(const std::string& stop_key, const std::string& mark_key) {
    stop_key_ = stop_key;
    mark_key_ = mark_key;
}

void Overlay::update(double elapsed, double fps, unsigned long long bytes, int markers) {
    if (!hwnd_) return;
    auto* p = reinterpret_cast<Payload*>(GetWindowLongPtrW((HWND)hwnd_, GWLP_USERDATA));
    if (!p) return;

    p->line1 = "REC  " + format_duration(elapsed);

    char buf[160];
    if (fps > 0.0)
        snprintf(buf, sizeof(buf), "%.0f fps   %s", fps, format_size(bytes).c_str());
    else
        snprintf(buf, sizeof(buf), "%s", format_size(bytes).c_str());
    if (markers > 0) {
        char m[32];
        snprintf(m, sizeof(m), "   %d mark%s", markers, markers == 1 ? "" : "s");
        strcat_s(buf, m);
    }
    p->line2 = buf;

    p->line3 = stop_key_ + "  stop";
    p->line4 = mark_key_ + "  mark highlight";

    // Repaint a few times a second, not once per frame. Nothing here changes
    // faster than that, and forcing a z-order recalculation at frame rate makes
    // the window manager do real work on behalf of a status readout.
    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const long long ms = now.QuadPart * 1000 / freq.QuadPart;
    if (last_paint_ms_ != 0 && ms - last_paint_ms_ < 250) return;
    last_paint_ms_ = ms;

    // Reassert topmost only occasionally, in case a game has taken the z-order.
    if (++paint_count_ % 8 == 0) {
        SetWindowPos((HWND)hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    InvalidateRect((HWND)hwnd_, nullptr, FALSE);
}

void Overlay::pump() {
    if (!hwnd_) return;
    MSG msg;
    while (PeekMessageW(&msg, (HWND)hwnd_, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

} // namespace oc
