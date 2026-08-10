#include "capture/game_watch.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <filesystem>
#include <set>

#include "platform/subprocess.h"

namespace oc {
namespace {

constexpr long long kSweepIntervalMs = 1500;

std::string normalise(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0)
        name.resize(name.size() - 4);
    return name;
}

long long now_ms() {
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (long long)(c.QuadPart * 1000 / f.QuadPart);
}

// Base name of a process, lower-cased and without .exe. Empty when the process
// cannot be opened, which is normal for anything running at higher privilege.
std::string name_of(DWORD pid) {
    // QUERY_LIMITED_INFORMATION is the least this can ask for and works against
    // processes that the fuller access right would be refused on.
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};

    wchar_t buf[MAX_PATH]{};
    DWORD   len = MAX_PATH;
    std::string out;
    if (QueryFullProcessImageNameW(h, 0, buf, &len))
        out = normalise(std::filesystem::path(buf).filename().string());
    CloseHandle(h);
    return out;
}

} // namespace

GameWatch::~GameWatch() { release_handle(); }

void GameWatch::release_handle() {
    if (current_handle_) {
        CloseHandle((HANDLE)current_handle_);
        current_handle_ = nullptr;
    }
}

void GameWatch::set_watchlist(const std::vector<std::string>& names) {
    watch_.clear();
    for (const auto& n : names) {
        auto v = normalise(n);
        if (!v.empty()) watch_.push_back(std::move(v));
    }
    // A game already being tracked should stop being tracked if it is no longer
    // on the list, rather than firing a spurious event later.
    if (!current_.empty() &&
        std::find(watch_.begin(), watch_.end(), current_) == watch_.end()) {
        current_.clear();
        release_handle();
    }
}

GameWatch::Event GameWatch::poll(bool recording) {
    // ---- a game is being tracked: watch that one handle and nothing else ----
    if (!current_.empty()) {
        if (!current_handle_) {          // handle lost; treat as exited
            current_.clear();
            return Event::Stopped;
        }
        // Signalled means the process has exited. This is a status check on a
        // handle we already hold, so there is no enumeration and no allocation.
        if (WaitForSingleObject((HANDLE)current_handle_, 0) == WAIT_OBJECT_0) {
            current_.clear();
            release_handle();
            return Event::Stopped;
        }
        return Event::None;
    }

    if (watch_.empty()) return Event::None;

    // Nothing to gain from finding a second game mid-recording.
    if (recording) return Event::None;

    const long long now = now_ms();
    if (last_sweep_ms_ != 0 && now - last_sweep_ms_ < kSweepIntervalMs)
        return Event::None;
    last_sweep_ms_ = now;

    // ---- cheap sweep: PIDs only, names resolved once per process ----
    DWORD pids[2048];
    DWORD bytes = 0;
    if (!EnumProcesses(pids, sizeof(pids), &bytes)) return Event::None;
    const size_t count = bytes / sizeof(DWORD);

    std::unordered_map<unsigned long, std::string> fresh;
    fresh.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const DWORD pid = pids[i];
        if (pid == 0) continue;

        std::string name;
        if (auto it = name_cache_.find(pid); it != name_cache_.end()) {
            name = it->second;                    // seen before: no syscall
        } else {
            name = name_of(pid);                  // first sighting only
        }
        fresh.emplace(pid, name);
        if (name.empty()) continue;

        if (std::find(watch_.begin(), watch_.end(), name) != watch_.end()) {
            // Hold a handle so every later check is a handle test rather than
            // another sweep. SYNCHRONIZE is what WaitForSingleObject needs.
            HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, pid);
            if (!h) continue;                     // vanished between calls
            release_handle();
            current_handle_ = h;
            current_        = name;
            name_cache_     = std::move(fresh);
            return Event::Started;
        }
    }

    // Replacing the cache wholesale drops entries for processes that have since
    // exited, so PIDs cannot go stale and be mistaken for a new process later.
    name_cache_ = std::move(fresh);
    return Event::None;
}

std::vector<std::string> GameWatch::running_processes() {
    // Windows runs well over a hundred background processes, so an unfiltered
    // list buries the one entry the user is looking for. These are the usual
    // suspects; anything unrecognised is still shown, because guessing which
    // executable is "a game" is exactly what this must not try to do.
    static const std::set<std::string> kNoise = {
        "svchost", "conhost", "dllhost", "rundll32", "csrss", "wininit", "winlogon",
        "services", "lsass", "smss", "fontdrvhost", "dwm", "sihost", "taskhostw",
        "ctfmon", "explorer", "runtimebroker", "searchhost", "startmenuexperiencehost",
        "shellexperiencehost", "textinputhost", "applicationframehost", "systemsettings",
        "audiodg", "spoolsv", "wmiprvse", "msmpeng", "nissrv", "securityhealthservice",
        "securityhealthsystray", "system", "registry", "idle", "memory compression",
        "backgroundtaskhost", "wudfhost", "sppsvc", "trustedinstaller", "tiworker",
        "usocoreworker", "mousocoreworker", "lockapp", "widgets", "widgetservice",
        "phoneexperiencehost", "crossdeviceservice", "outplayed_clone", "ffmpeg", "ffprobe",
    };

    std::vector<std::string> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::set<std::string> seen;
    if (Process32FirstW(snap, &entry)) {
        do {
            std::string name = to_utf8(entry.szExeFile);
            const std::string key = normalise(name);
            if (kNoise.count(key)) continue;
            if (seen.insert(key).second) out.push_back(std::move(name));
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);

    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return _stricmp(a.c_str(), b.c_str()) < 0;
    });
    return out;
}

} // namespace oc
