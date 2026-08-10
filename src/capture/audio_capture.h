#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace oc {

// Captures the default render endpoint in WASAPI loopback mode and streams raw
// PCM into a named pipe that ffmpeg reads as a second input.
//
// ffmpeg's stdin is reserved for the graceful 'q' stop, which is why the audio
// travels over a pipe of its own rather than through stdin.
class AudioCapture {
public:
    ~AudioCapture();

    // Opens the endpoint to learn the mix format, then creates the pipe server
    // and starts the capture thread. The thread blocks until ffmpeg connects.
    //
    // `device_id` is a WASAPI endpoint id; empty means the default endpoint.
    // `loopback` records what a playback device is producing (game, chat); with
    // it false the device is a microphone or line input and is recorded directly.
    // Returns false if the endpoint could not be opened.
    bool start(const std::wstring& pipe_name,
               const std::string& device_id = {},
               bool loopback = true);
    void stop();

    // Friendly label for logs and the UI.
    const std::string& label() const { return label_; }

    // Valid after a successful start(). Describes what the pipe will carry, so
    // the caller can build matching ffmpeg input flags.
    const char* sample_format() const { return sample_fmt_; }  // "f32le" / "s16le"
    int sample_rate() const { return sample_rate_; }
    int channels()    const { return channels_; }

    bool failed() const { return failed_.load(std::memory_order_relaxed); }
    std::string error() const;

private:
    void run(std::wstring pipe_name, std::string device_id, bool loopback);

    std::string label_;

    std::thread       thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};

    const char* sample_fmt_ = "f32le";
    int  sample_rate_ = 48000;
    int  channels_    = 2;
    int  bytes_per_frame_ = 8;

    mutable std::mutex err_mutex_;
    std::string        error_;
};

} // namespace oc
