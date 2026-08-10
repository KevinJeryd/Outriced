#pragma once
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace oc {

enum class LogLevel { Debug, Info, Warn, Error };

// Opens logs/outriced.log under the app root, rotating the previous run to
// .1.log. Safe to call more than once; later calls are ignored.
void log_init(const std::filesystem::path& root);
void log_shutdown();

// Installs the unhandled-exception filter that writes a minidump next to the
// log. Call once, as early as possible.
void install_crash_handler();

void log_write(LogLevel level, std::string_view message);

template <class... Args>
void log_msg(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    log_write(level, std::format(fmt, std::forward<Args>(args)...));
}

#define OC_LOG_D(...) ::oc::log_msg(::oc::LogLevel::Debug, __VA_ARGS__)
#define OC_LOG_I(...) ::oc::log_msg(::oc::LogLevel::Info,  __VA_ARGS__)
#define OC_LOG_W(...) ::oc::log_msg(::oc::LogLevel::Warn,  __VA_ARGS__)
#define OC_LOG_E(...) ::oc::log_msg(::oc::LogLevel::Error, __VA_ARGS__)

// Where the log lives, for the "open log folder" button.
std::filesystem::path log_directory();

} // namespace oc
