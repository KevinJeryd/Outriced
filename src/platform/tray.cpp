#include "platform/tray.h"

#include <windows.h>
#include <shellapi.h>
#include <utility>

namespace oc {
namespace {

constexpr UINT WM_OC_TRAY   = WM_APP + 1;
constexpr UINT WM_OC_QUIT   = WM_APP + 2;
constexpr UINT WM_OC_UPDATE = WM_APP + 3;
constexpr int  kHotkeyId    = 1;
constexpr int  kMarkerId    = 2;
constexpr UINT kIconId      = 1;

constexpr UINT kMenuToggle = 100;
constexpr UINT kMenuOpen   = 101;
constexpr UINT kMenuQuit   = 102;
constexpr UINT kMenuMark   = 103;

NOTIFYICONDATAW g_nid{};

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_HOTKEY:
        if (wp == kHotkeyId)      self->push(TrayEvent::ToggleRecording);
        else if (wp == kMarkerId) self->push(TrayEvent::MarkHighlight);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kMenuToggle: self->push(TrayEvent::ToggleRecording); return 0;
        case kMenuOpen:   self->push(TrayEvent::ShowWindow);      return 0;
        case kMenuMark:   self->push(TrayEvent::MarkHighlight);   return 0;
        case kMenuQuit:   self->push(TrayEvent::Quit);            return 0;
        default: break;
        }
        return 0;

    case WM_OC_UPDATE: {
        const bool rec = self->is_recording();
        wcscpy_s(g_nid.szTip, rec ? L"Outriced - RECORDING"
                                  : L"Outriced - idle");
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        return 0;
    }

    case WM_OC_QUIT:
        PostQuitMessage(0);
        return 0;

    case WM_OC_TRAY:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            self->push(TrayEvent::ShowWindow);
        } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            POINT pt{};
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, kMenuToggle,
                        self->is_recording() ? L"Stop recording" : L"Start recording");
            if (self->is_recording())
                AppendMenuW(menu, MF_STRING, kMenuMark, L"Mark highlight");
            AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open clip browser");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit");
            // Without this the menu refuses to dismiss when focus moves away.
            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            DestroyMenu(menu);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

Tray::~Tray() { stop(); }

std::string describe_hotkey(unsigned mods, unsigned vk) {
    std::string out;
    if (mods & MOD_CONTROL) out += "Ctrl+";
    if (mods & MOD_ALT)     out += "Alt+";
    if (mods & MOD_SHIFT)   out += "Shift+";
    if (mods & MOD_WIN)     out += "Win+";

    if (vk >= VK_F1 && vk <= VK_F24) {
        out += "F" + std::to_string(vk - VK_F1 + 1);
    } else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        out += (char)vk;
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%02X", vk);
        out += buf;
    }
    return out;
}

void Tray::push(TrayEvent e) {
    std::lock_guard lock(mutex_);
    events_.push_back(e);
}

std::vector<TrayEvent> Tray::poll() {
    std::lock_guard lock(mutex_);
    return std::exchange(events_, {});
}

void Tray::set_recording(bool recording) {
    recording_.store(recording, std::memory_order_relaxed);
    if (auto* h = hwnd_.load(std::memory_order_relaxed))
        PostMessageW((HWND)h, WM_OC_UPDATE, 0, 0);
}

bool Tray::start(unsigned hotkey_mods, unsigned hotkey_vk,
                 unsigned marker_mods, unsigned marker_vk) {
    thread_ = std::thread(&Tray::run, this, hotkey_mods, hotkey_vk,
                          marker_mods, marker_vk);
    // Give the window a moment to exist so hotkey_registered() is meaningful.
    for (int i = 0; i < 200 && !hwnd_.load(std::memory_order_relaxed); ++i)
        Sleep(5);
    return hwnd_.load(std::memory_order_relaxed) != nullptr;
}

void Tray::stop() {
    if (auto* h = hwnd_.load(std::memory_order_relaxed))
        PostMessageW((HWND)h, WM_OC_QUIT, 0, 0);
    if (thread_.joinable()) thread_.join();
}

void Tray::run(unsigned mods, unsigned vk, unsigned marker_mods, unsigned marker_vk) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OutricedTray";
    RegisterClassExW(&wc);

    // A message-only window still receives hotkeys and tray callbacks.
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Outriced",
                                0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) return;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);

    g_nid = {};
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = kIconId;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_OC_TRAY;
    g_nid.hIcon            = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Outriced - idle");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    hotkey_ok_.store(RegisterHotKey(hwnd, kHotkeyId, mods | MOD_NOREPEAT, vk) != FALSE,
                     std::memory_order_relaxed);
    marker_ok_.store(
        RegisterHotKey(hwnd, kMarkerId, marker_mods | MOD_NOREPEAT, marker_vk) != FALSE,
        std::memory_order_relaxed);
    hwnd_.store(hwnd, std::memory_order_relaxed);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(hwnd, kHotkeyId);
    UnregisterHotKey(hwnd, kMarkerId);
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    hwnd_.store(nullptr, std::memory_order_relaxed);
    DestroyWindow(hwnd);
}

} // namespace oc
