#include "platform/log.h"

#include <windows.h>
#include <dbghelp.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>

#pragma comment(lib, "dbghelp.lib")

namespace oc {
namespace {

std::mutex            g_mutex;
std::ofstream         g_file;
std::filesystem::path g_dir;
bool                  g_open = false;

const char* level_name(LogLevel l) {
    switch (l) {
    case LogLevel::Debug: return "DBG";
    case LogLevel::Warn:  return "WRN";
    case LogLevel::Error: return "ERR";
    default:              return "INF";
    }
}

std::string timestamp() {
    const auto now   = std::chrono::system_clock::now();
    const auto local = std::chrono::current_zone()->to_local(now);
    return std::format("{:%H:%M:%S}", std::chrono::floor<std::chrono::milliseconds>(local));
}

// Writes a minidump beside the log. Deliberately does almost nothing else: this
// runs after the process is already broken, so every extra call is a chance to
// fault again before the dump is on disk.
LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    wchar_t path[MAX_PATH]{};
    const auto now   = std::chrono::system_clock::now();
    const auto local = std::chrono::current_zone()->to_local(now);
    const auto name  = std::format(L"crash_{:%Y-%m-%d_%H-%M-%S}.dmp",
                                   std::chrono::floor<std::chrono::seconds>(local));
    const auto full  = (g_dir.empty() ? std::filesystem::path(L".") : g_dir) / name;
    wcsncpy_s(path, full.wstring().c_str(), _TRUNCATE);

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers    = FALSE;

        // WithIndirectlyReferencedMemory keeps the dump small but still gives a
        // usable view of the objects the faulting frames were touching.
        const auto type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory |
                                          MiniDumpScanMemory |
                                          MiniDumpWithThreadInfo);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type,
                          info ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(f);
    }

    if (g_open) {
        const DWORD code = info && info->ExceptionRecord
                               ? info->ExceptionRecord->ExceptionCode : 0;
        const void* addr = info && info->ExceptionRecord
                               ? info->ExceptionRecord->ExceptionAddress : nullptr;
        g_file << timestamp() << " ERR  [crash] unhandled exception 0x"
               << std::hex << code << std::dec << " at " << addr
               << ", dump written to " << full.string() << '\n';
        g_file.flush();
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

std::filesystem::path log_directory() { return g_dir; }

void log_init(const std::filesystem::path& root) {
    std::lock_guard lock(g_mutex);
    if (g_open) return;

    std::error_code ec;
    g_dir = root / "logs";
    std::filesystem::create_directories(g_dir, ec);

    // Keep the previous run around; a crash is usually reported after a restart.
    const auto current  = g_dir / "outriced.log";
    const auto previous = g_dir / "outriced.1.log";
    std::filesystem::remove(previous, ec);
    std::filesystem::rename(current, previous, ec);

    g_file.open(current, std::ios::out | std::ios::trunc);
    g_open = g_file.is_open();
    if (!g_open) return;

    g_file << "=== Outriced started " << timestamp() << " ===\n";
    g_file.flush();
}

void log_shutdown() {
    std::lock_guard lock(g_mutex);
    if (!g_open) return;
    g_file << timestamp() << " INF  [app] shutting down\n";
    g_file.flush();
    g_file.close();
    g_open = false;
}

void install_crash_handler() {
    SetUnhandledExceptionFilter(crash_filter);
    // A pure-virtual call or a failed allocation otherwise bypasses the filter
    // and takes the process down with no dump at all.
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
}

void log_write(LogLevel level, std::string_view message) {
    std::lock_guard lock(g_mutex);
    if (!g_open) return;
    g_file << timestamp() << ' ' << level_name(level) << "  " << message << '\n';
    // Flushed every line on purpose: the interesting entries are the ones
    // written immediately before a crash, and a buffered tail would be lost.
    g_file.flush();
}

} // namespace oc
