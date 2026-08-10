#include "platform/subprocess.h"

#include <windows.h>
#include <utility>

namespace oc {

std::wstring to_wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string to_utf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
        return arg;

    std::wstring out = L"\"";
    for (size_t i = 0; i < arg.size(); ++i) {
        size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++slashes; ++i; }
        if (i == arg.size()) {
            out.append(slashes * 2, L'\\');       // escape trailing slashes
            break;
        }
        if (arg[i] == L'"') {
            out.append(slashes * 2 + 1, L'\\');   // escape slashes and the quote
            out.push_back(L'"');
        } else {
            out.append(slashes, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

Process::~Process() {
    close_stdin();
    // Closing the read end unblocks the reader thread's pending ReadFile.
    if (stderr_read_) {
        CloseHandle((HANDLE)stderr_read_);
        stderr_read_ = nullptr;
    }
    if (stderr_thread_.joinable()) stderr_thread_.join();
    if (proc_)   CloseHandle((HANDLE)proc_);
    if (thread_) CloseHandle((HANDLE)thread_);
}

std::string Process::take_stderr() {
    std::lock_guard lock(stderr_mutex_);
    return stderr_text_;
}

bool Process::start(const std::wstring& cmdline, bool redirect_stdin,
                    bool capture_stderr) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (redirect_stdin) {
        if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
        // The child must not inherit our write end, or ffmpeg never sees EOF.
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    }

    HANDLE err_rd = nullptr, err_wr = nullptr;
    if (capture_stderr) {
        if (!CreatePipe(&err_rd, &err_wr, &sa, 0)) {
            if (rd) CloseHandle(rd);
            if (wr) CloseHandle(wr);
            return false;
        }
        SetHandleInformation(err_rd, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (redirect_stdin || capture_stderr) {
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = redirect_stdin ? rd : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = capture_stderr ? err_wr : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = capture_stderr ? err_wr : GetStdHandle(STD_ERROR_HANDLE);
    }

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmdline;  // CreateProcessW may write to this

    BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr,
                             (redirect_stdin || capture_stderr) ? TRUE : FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    if (rd)     CloseHandle(rd);      // the child owns its copy now
    if (err_wr) CloseHandle(err_wr);  // ditto, and so the reader sees EOF
    if (!ok) {
        if (wr)     CloseHandle(wr);
        if (err_rd) CloseHandle(err_rd);
        return false;
    }

    proc_   = pi.hProcess;
    thread_ = pi.hThread;
    stdin_  = wr;

    if (capture_stderr) {
        stderr_read_ = err_rd;
        stderr_thread_ = std::thread([this] {
            char buf[2048];
            DWORD got = 0;
            while (stderr_read_ &&
                   ReadFile((HANDLE)stderr_read_, buf, sizeof(buf), &got, nullptr) &&
                   got > 0) {
                std::lock_guard lock(stderr_mutex_);
                stderr_text_.append(buf, got);
                // A runaway child must not be able to grow this without bound.
                if (stderr_text_.size() > 64 * 1024)
                    stderr_text_.erase(0, stderr_text_.size() - 64 * 1024);
            }
        });
    }
    return true;
}

bool Process::running() const {
    if (!proc_) return false;
    return WaitForSingleObject((HANDLE)proc_, 0) == WAIT_TIMEOUT;
}

bool Process::write_stdin(std::string_view bytes) {
    if (!stdin_) return false;
    DWORD written = 0;
    return WriteFile((HANDLE)stdin_, bytes.data(), (DWORD)bytes.size(), &written, nullptr)
           && written == bytes.size();
}

void Process::close_stdin() {
    if (stdin_) {
        CloseHandle((HANDLE)stdin_);
        stdin_ = nullptr;
    }
}

bool Process::wait(unsigned timeout_ms) {
    if (!proc_) return true;
    return WaitForSingleObject((HANDLE)proc_, timeout_ms) == WAIT_OBJECT_0;
}

void Process::terminate() {
    if (proc_) TerminateProcess((HANDLE)proc_, 1);
}

std::optional<unsigned long> Process::exit_code() const {
    if (!proc_) return std::nullopt;
    DWORD code = 0;
    if (!GetExitCodeProcess((HANDLE)proc_, &code)) return std::nullopt;
    if (code == STILL_ACTIVE) return std::nullopt;
    return code;
}

std::optional<std::string> run_capture(const std::wstring& cmdline, unsigned timeout_ms) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return std::nullopt;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmdline;
    BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);  // so the read below terminates when the child exits
    if (!ok) {
        CloseHandle(rd);
        return std::nullopt;
    }

    std::string out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0)
        out.append(buf, got);

    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, timeout_ms);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return out;
}

} // namespace oc
