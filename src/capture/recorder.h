#pragma once
#include <filesystem>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/settings.h"

namespace oc {

class AudioCapture;
class VideoCapture;
class Process;

class Recorder {
public:
    Recorder();
    ~Recorder();

    // True from start() until stop(), regardless of whether the ffmpeg process
    // happens to be alive at this instant.
    //
    // These must stay separate. The capture can die under us -- alt-tabbing out
    // of a fullscreen game changes the display mode and invalidates the capture
    // session -- and a recording state defined by "is the process running" flips
    // to false the moment that happens, taking the code that was supposed to
    // notice and recover with it.
    bool recording() const { return active_; }

    // Whether the encoder process is alive right now. Internal health, not the
    // state the UI should be showing.
    bool process_alive() const;

    // Set when the capture died and could not be restarted. The session is over;
    // the caller should stop() to tidy up and report it.
    bool session_failed() const { return failed_; }

    // True while waiting out the backoff before relaunching the capture.
    bool resuming() const { return resume_at_ms_ != 0; }

    // Starts a session; the file lands in <sessions_dir>/<timestamp>.mp4.
    bool start(const Settings& s, const std::filesystem::path& root);

    // Sends 'q' to ffmpeg and waits for it to finalize the moov atom. Returns
    // the finished file, or nullopt if nothing usable was produced.
    std::optional<std::filesystem::path> stop();

    const std::string& last_error() const { return last_error_; }
    // Which capture backend the running session resolved to ("amf"/"cuda"/...).
    const std::string& backend() const { return backend_; }
    std::filesystem::path current_file() const { return current_file_; }
    // Seconds since the current session started; 0 when idle.
    double elapsed() const;

    // Records the current position as a highlight. Written into the session's
    // sidecar on stop so the preview timeline can show it.
    void mark_highlight();
    const std::vector<double>& markers() const { return markers_; }

    // Re-reads ffmpeg's progress stream, and notices if ffmpeg has died without
    // being asked to. Alt-tabbing out of a fullscreen game changes the display
    // mode, which invalidates the capture session underneath us and takes the
    // encoder down with it; when that happens a fresh segment is started rather
    // than silently ending the recording. Call once a frame.
    void refresh_progress();

    // Files written for the current session, in order. More than one means the
    // capture was interrupted and resumed.
    const std::vector<std::filesystem::path>& segments() const { return segments_; }
    int  resume_count() const { return resumes_; }
    double             capture_fps()  const { return capture_fps_; }
    unsigned long long output_bytes() const { return output_bytes_; }

private:
    std::unique_ptr<Process>      ffmpeg_;
    // One per recorded endpoint: each streams into its own pipe and becomes its
    // own ffmpeg input, so they can be mixed or kept as separate tracks.
    std::vector<std::unique_ptr<AudioCapture>> audio_;
    // Non-null only on the native backend; the others capture inside ffmpeg.
    std::unique_ptr<VideoCapture> video_;
    std::filesystem::path         current_file_;
    long long                     start_tick_ = 0;
    std::string                   last_error_;
    std::string                   backend_;
    std::filesystem::path         progress_file_;
    std::streamoff                progress_offset_  = 0;
    long long                     last_progress_ms_ = 0;
    double                        capture_fps_      = 0.0;
    unsigned long long            output_bytes_     = 0;
    std::vector<double>           markers_;

    // Writes markers_ to the current segment's sidecar and empties it. Called
    // when a segment ends, whether that is a clean stop or an interruption.
    void flush_markers();

    // State needed to relaunch the capture after an unexpected exit.
    bool                               stopping_ = false;
    bool                               active_   = false;
    bool                               failed_   = false;
    int                                resumes_  = 0;

    // Deferred, backing-off resume. Relaunching the instant the capture dies
    // just fails again: the display mode change that killed it is still in
    // progress, so the replacement is born into the same broken state and one
    // interruption shreds a session into five files. Waiting for the desktop to
    // settle costs a second of footage and saves the rest.
    long long                          resume_at_ms_   = 0;
    long long                          segment_start_  = 0;
    int                                backoff_ms_     = 0;
    Settings                           active_settings_;
    std::filesystem::path              active_root_;
    std::vector<std::filesystem::path> segments_;
    std::wstring                       pipe_name_;

    bool launch(const std::filesystem::path& file);
};

} // namespace oc
