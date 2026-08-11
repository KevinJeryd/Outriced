#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace oc {

// Captures a monitor with DXGI Desktop Duplication and streams NV12 frames into
// a named pipe that ffmpeg reads as rawvideo.
//
// This exists because ffmpeg's ddagrab filter could not be made to produce
// evenly spaced motion from outside. Measured against OBS on the same machine,
// same game and same 60 fps, the spread of per-frame motion was 1.14 for
// ddagrab and 0.89 for OBS, and the largest motion steps were 4-5x a typical
// frame against OBS's 2.5x. Poll rate, output resolution, capture backend,
// encoder and frame-rate mode were each ruled out by measurement; halving the
// pixel work from 1080p to 720p moved nothing (1.137 -> 1.173).
//
// The structural difference is what this class copies from OBS:
//
//   ddagrab is one filter pulled by the graph, so a frame is acquired when the
//   graph asks for one. Acquisition timing is therefore the graph's timing.
//
//   Here a dedicated thread blocks in AcquireNextFrame continuously, keeping
//   whatever the newest desktop image is, and emits on a strict timer grid.
//   When nothing new arrived in a slot the previous frame is sent again, which
//   is why OBS shows *more* duplicate frames than ddagrab and still looks
//   better: even spacing matters more than novelty.
//
// Pipe writes happen on a second thread behind a bounded queue so that an
// encoder hitch cannot reach back and disturb capture timing.
class VideoCapture {
public:
    // Both defined in the .cpp: the queue is an incomplete type here, so an
    // implicitly generated constructor or destructor would not know how to
    // destroy it in a translation unit that only sees this header.
    VideoCapture();
    ~VideoCapture();

    struct Config {
        int monitor_index = 0;   // index into enumerate_displays()
        int width     = 0;       // 0,0 = the monitor's native size
        int height    = 0;
        int framerate = 60;
    };

    // Resolves the output size, creates the pipe and starts the threads. The
    // capture grid does not begin until ffmpeg opens the pipe, so the first
    // frame is not stale. Returns false if the monitor could not be duplicated.
    bool start(const std::wstring& pipe_name, const Config& cfg);
    void stop();

    // Valid after a successful start(), so the caller can build matching
    // ffmpeg input flags. Always even, as NV12 requires.
    int width()  const { return out_w_; }
    int height() const { return out_h_; }

    bool failed() const { return failed_.load(std::memory_order_relaxed); }
    std::string error() const;

    // Diagnostics, read at stop() for the log.
    long long emitted()   const { return emitted_.load(std::memory_order_relaxed); }
    long long duplicated() const { return duplicated_.load(std::memory_order_relaxed); }
    long long dropped()   const { return dropped_.load(std::memory_order_relaxed); }
    long long resets()    const { return resets_.load(std::memory_order_relaxed); }

private:
    void capture_thread(Config cfg);
    void writer_thread(std::wstring pipe_name);

    // Bounded hand-off between the capture grid and the pipe writer. Held by
    // pointer so the queue's containers stay out of this header.
    struct Queue;
    std::unique_ptr<Queue> queue_;

    int out_w_ = 0, out_h_ = 0;

    std::thread       capture_;
    std::thread       writer_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> connected_{false};

    std::atomic<long long> emitted_{0}, duplicated_{0}, dropped_{0}, resets_{0};

    mutable std::mutex err_mutex_;
    std::string        error_;
};

} // namespace oc
