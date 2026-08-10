#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace oc {

// Watches for game executables by name and reports when one starts or stops.
//
// This only reads the process list, the same information Task Manager shows.
// Nothing is injected into the game and no hooks are installed, so it cannot
// upset anti-cheat.
//
// Cost matters here, because this runs while a game is in the foreground. A
// Toolhelp32 snapshot measured 6.7 ms across 323 processes -- enough to risk a
// dropped frame every time it fires -- so the sweep uses EnumProcesses, which
// returns bare PIDs in 0.078 ms, and only resolves a name for a PID it has not
// seen before. Once a game is found the sweep stops entirely and a handle to
// that one process is watched instead, which costs nothing at all.
class GameWatch {
public:
    ~GameWatch();

    enum class Event { None, Started, Stopped };

    // `names` are executable names, matched case-insensitively, with or without
    // the .exe suffix ("NarakaBladepoint" and "NarakaBladepoint.exe" both work).
    void set_watchlist(const std::vector<std::string>& names);

    // `recording` suppresses the hunt for newly started games: a second one
    // cannot be acted on while the first is still being recorded, so there is
    // no reason to pay for the sweep. Watching the running game for exit
    // continues regardless, since that is what stops the recording.
    Event poll(bool recording);

    // Executable that triggered the current Started state.
    const std::string& current_game() const { return current_; }
    bool game_running() const { return !current_.empty(); }

    // Full process list for the settings picker. Uses the heavier snapshot API,
    // which is fine for a one-off click.
    static std::vector<std::string> running_processes();

private:
    void release_handle();

    std::vector<std::string> watch_;   // lower-cased, without .exe
    std::string current_;
    void*       current_handle_ = nullptr;   // HANDLE to the watched process
    long long   last_sweep_ms_  = 0;

    // PID -> lower-cased base name, so a given process is only ever resolved
    // once no matter how many sweeps it survives.
    std::unordered_map<unsigned long, std::string> name_cache_;
};

} // namespace oc
