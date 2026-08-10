#pragma once
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <mutex>
#include <thread>

namespace oc {

// A child process with an optional redirected stdin, used to send ffmpeg the
// 'q' that makes it finalize the MP4 instead of being killed outright.
class Process {
public:
    Process() = default;
    ~Process();
    // Neither copyable nor movable: it owns a reader thread and the mutex that
    // guards its buffer. Held through unique_ptr where ownership must move.
    Process(const Process&)            = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&&)                 = delete;
    Process& operator=(Process&&)      = delete;

    // `cmdline` is a fully quoted Win32 command line. When `capture_stderr` is
    // set, a reader thread drains the child's stderr; take_stderr() returns
    // whatever it has collected. Without this a child that dies mid-run takes
    // its explanation with it.
    bool start(const std::wstring& cmdline, bool redirect_stdin,
               bool capture_stderr = false);

    // Everything the child has written to stderr so far.
    std::string take_stderr();

    bool running() const;
    bool write_stdin(std::string_view bytes);
    void close_stdin();

    // Returns false on timeout.
    bool wait(unsigned timeout_ms);
    void terminate();

    std::optional<unsigned long> exit_code() const;

private:
    void* proc_   = nullptr;  // HANDLE
    void* thread_ = nullptr;  // HANDLE
    void* stdin_  = nullptr;  // HANDLE (write end)

    void*       stderr_read_ = nullptr;  // HANDLE (read end)
    std::thread stderr_thread_;
    std::mutex  stderr_mutex_;
    std::string stderr_text_;
};

// Runs a process to completion and captures stdout. Used for ffprobe.
std::optional<std::string> run_capture(const std::wstring& cmdline,
                                       unsigned timeout_ms = 15000);

// Quotes a single argument per the Win32 command-line parsing rules.
std::wstring quote_arg(const std::wstring& arg);
std::wstring to_wide(std::string_view s);
std::string  to_utf8(std::wstring_view s);

} // namespace oc
