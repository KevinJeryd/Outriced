#include "media/player.h"

#include <windows.h>
#include <algorithm>
#include <cstring>

#if OC_HAVE_MPV
#include <mpv/client.h>
#endif

#include "platform/subprocess.h"

namespace oc {

#if OC_HAVE_MPV

namespace {
constexpr wchar_t kChildClass[] = L"OutricedVideo";

void ensure_child_class() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kChildClass;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);
    registered = true;
}
} // namespace

Player::~Player() { shutdown(); }

bool Player::init(void* parent_hwnd) {
    if (mpv_) return true;
    parent_ = parent_hwnd;
    if (!parent_) { error_ = "no parent window"; return false; }

    ensure_child_class();
    HWND child = CreateWindowExW(0, kChildClass, L"",
                                 WS_CHILD | WS_CLIPSIBLINGS,
                                 0, 0, 16, 16,
                                 (HWND)parent_, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    if (!child) { error_ = "could not create the video window"; return false; }
    child_ = child;

    mpv_ = mpv_create();
    if (!mpv_) {
        error_ = "mpv_create failed";
        DestroyWindow(child);
        child_ = nullptr;
        return false;
    }

    const int64_t wid = (int64_t)(intptr_t)child;
    mpv_set_option(mpv_, "wid", MPV_FORMAT_INT64, (void*)&wid);

    // Scrubbing wants responsiveness over strict correctness.
    mpv_set_option_string(mpv_, "keep-open", "yes");
    mpv_set_option_string(mpv_, "idle", "yes");
    mpv_set_option_string(mpv_, "osc", "no");
    mpv_set_option_string(mpv_, "osd-level", "0");
    mpv_set_option_string(mpv_, "input-default-bindings", "no");
    mpv_set_option_string(mpv_, "input-vo-keyboard", "no");
    mpv_set_option_string(mpv_, "hr-seek", "yes");
    mpv_set_option_string(mpv_, "hr-seek-framedrop", "yes");
    mpv_set_option_string(mpv_, "vo", "gpu");
    mpv_set_option_string(mpv_, "hwdec", "auto-safe");

    if (mpv_initialize(mpv_) < 0) {
        error_ = "mpv_initialize failed";
        mpv_destroy(mpv_);
        mpv_ = nullptr;
        DestroyWindow(child);
        child_ = nullptr;
        return false;
    }

    mpv_observe_property(mpv_, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "pause",    MPV_FORMAT_FLAG);
    return true;
}

void Player::shutdown() {
    if (mpv_) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }
    if (child_) {
        DestroyWindow((HWND)child_);
        child_ = nullptr;
    }
}

void Player::load(const std::filesystem::path& file) {
    if (!mpv_) return;
    const std::string utf8 = to_utf8(file.wstring());
    const char* cmd[] = {"loadfile", utf8.c_str(), nullptr};
    mpv_command_async(mpv_, 0, cmd);
    position_ = 0.0;
    duration_ = 0.0;
    set_paused(false);
}

void Player::stop() {
    if (!mpv_) return;
    const char* cmd[] = {"stop", nullptr};
    // Synchronous: the caller is usually about to delete the file, and mpv must
    // have let go of it before that can succeed.
    mpv_command(mpv_, cmd);
    position_ = 0.0;
    duration_ = 0.0;
    paused_   = true;
}

void Player::toggle_pause() { set_paused(!paused_); }

void Player::set_paused(bool paused) {
    if (!mpv_) return;
    int flag = paused ? 1 : 0;
    mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &flag);
    paused_ = paused;
}

void Player::seek_absolute(double seconds) {
    if (!mpv_) return;
    if (seconds < 0) seconds = 0;
    if (duration_ > 0 && seconds > duration_) seconds = duration_;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.3f", seconds);
    const char* cmd[] = {"seek", buf, "absolute", "exact", nullptr};
    mpv_command_async(mpv_, 0, cmd);
    position_ = seconds;  // optimistic, corrected by the next time-pos event
}

void Player::step_seconds(double delta) { seek_absolute(position_ + delta); }

void Player::set_volume(double volume) {
    volume_ = std::clamp(volume, 0.0, 100.0);
    if (!mpv_) return;
    mpv_set_property_async(mpv_, 0, "volume", MPV_FORMAT_DOUBLE, &volume_);
}

void Player::set_muted(bool muted) {
    muted_ = muted;
    if (!mpv_) return;
    int flag = muted ? 1 : 0;
    mpv_set_property_async(mpv_, 0, "mute", MPV_FORMAT_FLAG, &flag);
}

void Player::update() {
    if (!mpv_) return;
    while (true) {
        mpv_event* ev = mpv_wait_event(mpv_, 0.0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;
        if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto* prop = (mpv_event_property*)ev->data;
            if (!prop->data) continue;
            if (strcmp(prop->name, "time-pos") == 0 && prop->format == MPV_FORMAT_DOUBLE)
                position_ = *(double*)prop->data;
            else if (strcmp(prop->name, "duration") == 0 && prop->format == MPV_FORMAT_DOUBLE)
                duration_ = *(double*)prop->data;
            else if (strcmp(prop->name, "pause") == 0 && prop->format == MPV_FORMAT_FLAG)
                paused_ = *(int*)prop->data != 0;
        }
    }
}

void Player::set_rect(int x, int y, int w, int h) {
    if (!child_) return;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    SetWindowPos((HWND)child_, HWND_TOP, x, y, w, h,
                 SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

void Player::set_visible(bool visible) {
    if (visible_ == visible) return;
    visible_ = visible;
    if (child_) ShowWindow((HWND)child_, visible ? SW_SHOWNA : SW_HIDE);
}

#else // !OC_HAVE_MPV

Player::~Player() = default;
bool Player::init(void*)              { error_ = "built without libmpv"; return false; }
void Player::shutdown()               {}
void Player::load(const std::filesystem::path&) {}
void Player::stop()                   {}
void Player::toggle_pause()           {}
void Player::set_paused(bool)         {}
void Player::seek_absolute(double)    {}
void Player::step_seconds(double)     {}
void Player::set_volume(double v)     { volume_ = v; }
void Player::set_muted(bool m)        { muted_ = m; }
void Player::update()                 {}
void Player::set_rect(int, int, int, int) {}
void Player::set_visible(bool)        {}

#endif

} // namespace oc
