#include "capture/recorder.h"

#include <windows.h>
#include <chrono>
#include <format>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <nlohmann/json.hpp>

#include "capture/audio_capture.h"
#include "capture/video_capture.h"
#include "capture/encoders.h"
#include "platform/log.h"
#include "platform/subprocess.h"

namespace oc {
namespace {

std::wstring timestamp_name() {
    const auto now = std::chrono::system_clock::now();
    const auto local = std::chrono::current_zone()->to_local(now);
    return to_wide(std::format("{:%Y-%m-%d_%H-%M-%S}",
                               std::chrono::floor<std::chrono::seconds>(local)));
}

} // namespace

Recorder::Recorder()  = default;
Recorder::~Recorder() { if (recording()) stop(); }

bool Recorder::process_alive() const { return ffmpeg_ && ffmpeg_->running(); }

double Recorder::elapsed() const {
    if (!start_tick_) return 0.0;
    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - start_tick_) / double(freq.QuadPart);
}

bool Recorder::start(const Settings& s, const std::filesystem::path& root) {
    if (active_) return false;
    last_error_.clear();
    stopping_ = false;
    failed_   = false;
    resumes_  = 0;
    segments_.clear();
    active_settings_ = s;
    active_root_     = root;

    std::filesystem::path sessions_probe = resolve_dir(root, s.sessions_dir);
    std::error_code probe_ec;
    std::filesystem::create_directories(sessions_probe, probe_ec);
    resume_at_ms_ = 0;
    backoff_ms_   = 0;

    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    segment_start_ = c.QuadPart * 1000 / f.QuadPart;

    const auto first = sessions_probe / (timestamp_name() + L".mp4");
    active_ = launch(first);
    return active_;
}

bool Recorder::launch(const std::filesystem::path& file) {
    const Settings& s = active_settings_;
    const std::filesystem::path& root = active_root_;

    const std::filesystem::path ffmpeg = root / s.ffmpeg_path;
    if (!std::filesystem::exists(ffmpeg)) {
        last_error_ = "ffmpeg not found at " + ffmpeg.string();
        return false;
    }

    std::filesystem::path sessions = file.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(sessions, ec);
    current_file_ = file;

    // Build the list of endpoints to record: every chosen playback device, then
    // the microphone last so its track index is predictable when editing.
    struct Source { std::string id; bool loopback; std::string role; };
    std::vector<Source> wanted;
    if (s.capture_audio) {
        if (s.audio_outputs.empty()) {
            wanted.push_back({"", true, "output"});
        } else {
            for (const auto& id : s.audio_outputs) wanted.push_back({id, true, "output"});
        }
        if (!s.audio_input.empty()) wanted.push_back({s.audio_input, false, "mic"});
    }

    audio_.clear();
    std::vector<std::wstring> pipe_names;
    for (size_t i = 0; i < wanted.size(); ++i) {
        // A per-segment, per-source pipe name: a resumed capture must not
        // collide with a pipe the previous ffmpeg is still letting go of.
        const std::wstring pipe =
            L"\\\\.\\pipe\\oc_audio_" + std::to_wstring(GetCurrentProcessId()) +
            L"_" + std::to_wstring(resumes_) + L"_" + std::to_wstring(i);

        auto cap = std::make_unique<AudioCapture>();
        if (!cap->start(pipe, wanted[i].id, wanted[i].loopback)) {
            // One unavailable device must not sink the whole recording.
            OC_LOG_W("[rec] audio source '{}' unavailable: {}",
                     wanted[i].id.empty() ? "default" : wanted[i].id, cap->error());
            last_error_ = "an audio device was unavailable; recording without it";
            continue;
        }
        OC_LOG_I("[rec] audio {}: {} ({} Hz, {} ch, {})", i, cap->label(),
                 cap->sample_rate(), cap->channels(), wanted[i].role);
        pipe_names.push_back(pipe);
        audio_.push_back(std::move(cap));
    }
    if (s.capture_audio && audio_.empty())
        last_error_ = "no audio device could be opened; recording video only";

    const Vendor vendor = detect_vendor();
    CaptureBackend backend =
        resolve_backend(capture_backend_from_string(s.capture_backend), vendor,
                        ffmpeg, s.monitor_index);

    // Native capture runs in this process and hands ffmpeg finished NV12 frames,
    // so it has to be up before the command line can be built: the frame size it
    // resolves decides the -s flag, and its pipe is input 0.
    std::wstring video_pipe;
    video_.reset();
    if (backend == CaptureBackend::Native) {
        video_pipe = L"\\\\.\\pipe\\oc_video_" + std::to_wstring(GetCurrentProcessId()) +
                     L"_" + std::to_wstring(resumes_);
        VideoCapture::Config vc;
        vc.monitor_index = s.monitor_index;
        vc.width         = s.capture_width;
        vc.height        = s.capture_height;
        vc.framerate     = s.framerate;

        auto vcap = std::make_unique<VideoCapture>();
        if (vcap->start(video_pipe, vc)) {
            video_ = std::move(vcap);
        } else {
            // Another capture tool holding the monitor, or a GPU without a
            // video processor. Falling back keeps recording working rather than
            // failing outright on a backend that is still new.
            OC_LOG_W("[rec] native capture unavailable ({}); falling back to ddagrab",
                     vcap->error());
            backend = CaptureBackend::Ddagrab;
        }
    }
    const bool native = video_ != nullptr;
    backend_ = to_string(backend);

    // Polling only means anything to the ffmpeg-side backends, where ddagrab is
    // asked to produce frames at a chosen rate. Native capture blocks on the
    // duplication API instead and takes frames as the desktop presents them, so
    // there is no rate to choose and the setting is left out of it entirely.
    int poll_mult = 1;
    if (!native) {
        // Clamped rather than trusted: the setting is hand-editable, and a wild
        // multiplier costs a readback per poll for frames that do not exist.
        poll_mult = std::clamp(s.capture_poll_multiplier, 1, 4);

        // Oversampling only works because cfr's -r decimates back onto the
        // target grid, picking the freshest frame per slot. Under vfr nothing
        // decimates, so the multiplier would instead raise the recorded frame
        // rate and split the same bitrate across two or three times as many
        // frames. That reads as a collapse in image quality, not as smoother
        // motion.
        if (poll_mult != 1 && s.fps_mode == "vfr") {
            OC_LOG_W("[rec] fps_mode=vfr does not decimate; ignoring {}x capture polling",
                     poll_mult);
            poll_mult = 1;
        }
        if (poll_mult != 1)
            OC_LOG_I("[rec] polling capture at {} Hz for {} fps output ({}x)",
                     s.framerate * poll_mult, s.framerate, poll_mult);
    }
    const auto pipeline = build_pipeline(backend, s.monitor_index, s.framerate,
                                         s.capture_width, s.capture_height,
                                         s.draw_mouse, s.encoder,
                                         s.framerate * poll_mult);

    std::wstring cmd;
    auto arg = [&](const std::wstring& a) { cmd += quote_arg(a); cmd += L' '; };

    arg(ffmpeg.wstring());
    arg(L"-hide_banner");
    arg(L"-loglevel"); arg(L"error");
    arg(L"-y");

    // Progress goes to a file the UI polls, which is how the overlay knows the
    // real capture rate. A pipe would need another reader thread, and stdin is
    // already spoken for by the graceful stop.
    progress_file_    = sessions / (current_file_.stem().wstring() + L".progress");
    progress_offset_  = 0;
    last_progress_ms_ = 0;
    markers_.clear();
    arg(L"-progress"); arg(progress_file_.wstring());
    arg(L"-stats_period"); arg(L"0.5");

    // Video
    if (!native) {
        arg(L"-init_hw_device"); arg(to_wide(pipeline.hw_device));
        if (!pipeline.filter_hw_device.empty()) {
            arg(L"-filter_hw_device"); arg(to_wide(pipeline.filter_hw_device));
        }
    } else {
        // Frames are already scaled and converted, so ffmpeg only demuxes them.
        // -framerate stamps the input, and since VideoCapture emits on a strict
        // grid those timestamps are the real ones rather than an assumption.
        // The rawvideo demuxer's own option names, not the generic -pix_fmt/-s
        // aliases, so there is no ambiguity about which stage they apply to.
        arg(L"-thread_queue_size"); arg(L"64");
        arg(L"-f");            arg(L"rawvideo");
        arg(L"-pixel_format"); arg(L"nv12");
        arg(L"-video_size");   arg(std::to_wstring(video_->width()) + L"x" +
                                   std::to_wstring(video_->height()));
        arg(L"-framerate");    arg(std::to_wstring(s.framerate));
        arg(L"-i");            arg(video_pipe);
    }

    // With native capture the video pipe is input 0 and audio follows it;
    // otherwise the video source is a filter and the audio pipes start at 0.
    // Everything gets an explicit label and -map once there is more than one
    // audio stream, because ffmpeg's automatic mapping picks a single track.
    const size_t audio_base = native ? 1 : 0;
    const size_t n_audio  = audio_.size();
    const bool   separate = s.audio_track_mode == "separate";
    const int    mic_gain = std::clamp(s.mic_gain_percent, 0, 400);

    // True when the microphone actually opened, so it really is the last input.
    const bool has_mic_track =
        !s.audio_input.empty() && n_audio > 0 && !audio_.back()->label().empty() &&
        n_audio == (s.audio_outputs.empty() ? 1u : s.audio_outputs.size()) + 1u;

    std::ostringstream fc;
    if (native) fc << "[0:v]null[v]";
    else        fc << pipeline.filter_chain << "[v]";

    if (n_audio > 0) {
        // Microphone gain is applied per-source, before any mixing.
        const bool has_mic = has_mic_track;
        const size_t mic_index = n_audio - 1;
        for (size_t i = 0; i < n_audio; ++i) {
            const bool is_mic = has_mic && i == mic_index;
            fc << ";[" << (i + audio_base) << ":a]";

            std::string chain;
            if (is_mic) {
                // A mono capsule on a two-channel endpoint lands on the left
                // only, which plays back in one ear. pan rebuilds both channels
                // from it rather than leaving half the field empty.
                if (s.mic_channel_mode == "mono_left")
                    chain = "pan=stereo|c0=c0|c1=c0";
                else if (s.mic_channel_mode == "mono_mix")
                    chain = "pan=stereo|c0=0.5*c0+0.5*c1|c1=0.5*c0+0.5*c1";

                if (mic_gain != 100) {
                    if (!chain.empty()) chain += ",";
                    chain += "volume=" + std::format("{:.3f}", mic_gain / 100.0);
                }
            }
            if (chain.empty()) chain = "anull";

            fc << chain << "[a" << i << "]";
        }
        if (!separate && n_audio > 1) {
            fc << ";";
            for (size_t i = 0; i < n_audio; ++i) fc << "[a" << i << "]";
            // normalize=0 keeps each source at its own level instead of
            // quietening everything as sources are added.
            fc << "amix=inputs=" << n_audio << ":duration=longest:normalize=0[amix]";
        }
    }
    arg(L"-filter_complex"); arg(to_wide(fc.str()));

    // Audio, from the WASAPI loopback pipe.
    //
    // The generous thread_queue_size is insurance rather than a measured win:
    // the default input queue is only a few packets, and a PCM pipe that briefly
    // outruns it makes ffmpeg block on the shared demux path. Frame-rate runs
    // here were too noisy to show a difference either way.
    for (size_t i = 0; i < audio_.size(); ++i) {
        arg(L"-thread_queue_size"); arg(L"4096");
        arg(L"-f");   arg(to_wide(audio_[i]->sample_format()));
        arg(L"-ar");  arg(std::to_wstring(audio_[i]->sample_rate()));
        arg(L"-ac");  arg(std::to_wstring(audio_[i]->channels()));
        arg(L"-i");   arg(pipe_names[i]);
    }

    // See Settings::fps_mode for the trade-off. Under cfr the frame rate is
    // pinned with -r; under vfr that would defeat the point, so the rate is left
    // to the capture source and only the ceiling is implied by the filter graph.
    const bool cfr = s.fps_mode != "vfr";
    arg(L"-fps_mode"); arg(cfr ? L"cfr" : L"vfr");
    if (cfr) { arg(L"-r"); arg(std::to_wstring(s.framerate)); }

    // With two inputs, ffmpeg will hold back one stream to keep the interleave
    // tight if the other falls behind. That is the wrong trade here: a stalled
    // audio pipe must never be able to stall video capture. Writing packets as
    // they arrive costs a little interleaving neatness and removes the coupling.
    arg(L"-max_interleave_delta"); arg(L"0");

    const std::wstring bitrate = std::to_wstring(s.session_bitrate_kbps) + L"k";
    const std::wstring maxrate = std::to_wstring(s.session_bitrate_kbps * 4 / 3) + L"k";

    arg(L"-c:v"); arg(to_wide(s.encoder));

    // Rate-control flags are per-encoder; passing AMF's to NVENC is an error.
    if (s.encoder == "h264_amf" || s.encoder == "hevc_amf" || s.encoder == "av1_amf") {
        // The GPU is already busy drawing the game, so the encoder is told to
        // finish quickly rather than squeeze the last few percent out of the
        // bitrate. "transcoding" usage assumes an otherwise idle card.
        arg(L"-usage");   arg(L"lowlatency_high_quality");
        arg(L"-quality"); arg(L"speed");
        arg(L"-rc");      arg(L"vbr_latency");
        arg(L"-b:v");     arg(bitrate);
        arg(L"-maxrate"); arg(maxrate);
    } else if (s.encoder == "h264_nvenc" || s.encoder == "hevc_nvenc") {
        arg(L"-preset");  arg(L"p4");
        arg(L"-rc");      arg(L"vbr");
        arg(L"-b:v");     arg(bitrate);
        arg(L"-maxrate"); arg(maxrate);
        arg(L"-bufsize"); arg(std::to_wstring(s.session_bitrate_kbps * 2) + L"k");
    } else if (s.encoder == "h264_qsv" || s.encoder == "hevc_qsv") {
        arg(L"-preset");  arg(L"medium");
        arg(L"-b:v");     arg(bitrate);
        arg(L"-maxrate"); arg(maxrate);
    } else {
        // libx264: a fast preset, since this runs for the whole session while a
        // game is in the foreground.
        arg(L"-preset");  arg(L"veryfast");
        arg(L"-b:v");     arg(bitrate);
        arg(L"-maxrate"); arg(maxrate);
        arg(L"-bufsize"); arg(std::to_wstring(s.session_bitrate_kbps * 2) + L"k");
    }
    arg(L"-pix_fmt"); arg(L"yuv420p");

    // Explicit mapping: video first, then either the mixed track or one per
    // device. Track titles survive into the file so an editor can tell which is
    // the microphone without listening to each in turn.
    arg(L"-map"); arg(L"[v]");
    if (n_audio > 0) {
        if (!separate && n_audio > 1) {
            arg(L"-map"); arg(L"[amix]");
        } else {
            for (size_t i = 0; i < n_audio; ++i) {
                arg(L"-map"); arg(L"[a" + std::to_wstring(i) + L"]");
            }
        }
        arg(L"-c:a"); arg(L"aac");
        arg(L"-b:a"); arg(L"160k");
        arg(L"-ac");  arg(L"2");     // downmix whatever the endpoint mixes at

        if (separate || n_audio == 1) {
            // MP4 has no per-stream "title" the way Matroska does; the mov muxer
            // stores this in the track's handler instead, which is what editors
            // read back. Both are set so the name survives either way.
            for (size_t i = 0; i < n_audio; ++i) {
                // The same endpoint can appear as both a playback and a capture
                // device under one name, so the role is spelled out; otherwise
                // two tracks read identically in an editor.
                const bool is_mic = has_mic_track && i == n_audio - 1;
                const auto name = to_wide((is_mic ? "Mic: " : "Game: ") +
                                          audio_[i]->label());
                arg(L"-metadata:s:a:" + std::to_wstring(i)); arg(L"title=" + name);
                arg(L"-metadata:s:a:" + std::to_wstring(i)); arg(L"handler_name=" + name);
            }
        }
    }

    // Deliberately NOT +faststart here, unlike clip export.
    //
    // faststart moves the MP4 index to the front of the file, which ffmpeg does
    // by rewriting the entire output after encoding finishes. That is worth it
    // for a 9 MB clip someone streams from Discord. A session is played locally
    // by mpv, which reads the index wherever it is, and sessions are large: an
    // hour at the default bitrate is roughly 5 GB. Rewriting that on stop takes
    // far longer than the timeout in stop() allows, so the recording would be
    // killed part-way through finalising and left unusable. Omitting the flag
    // makes stopping near-instant regardless of how long the session ran.
    arg(current_file_.wstring());

    OC_LOG_I("[rec] backend={} encoder={} monitor={} {}x{}@{}",
             backend_, s.encoder, s.monitor_index,
             s.capture_width, s.capture_height, s.framerate);
    OC_LOG_D("[rec] filter: {}", pipeline.filter_chain);
    OC_LOG_D("[rec] cmdline: {}", to_utf8(cmd));

    ffmpeg_ = std::make_unique<Process>();
    if (!ffmpeg_->start(cmd, /*redirect_stdin=*/true, /*capture_stderr=*/true)) {
        last_error_ = "failed to launch ffmpeg";
        OC_LOG_E("[rec] CreateProcess failed for ffmpeg");
        ffmpeg_.reset();
        for (auto& a : audio_) if (a) a->stop();
        audio_.clear();
        video_.reset();
        return false;

    }
    OC_LOG_I("[rec] recording to {}", current_file_.string());
    segments_.push_back(current_file_);

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    start_tick_ = now.QuadPart;
    return true;
}

void Recorder::mark_highlight() {
    if (!recording()) return;
    const double at = elapsed();
    // Pressing the key twice in quick succession means one moment, not two.
    if (!markers_.empty() && at - markers_.back() < 1.0) return;
    markers_.push_back(at);
}

void Recorder::refresh_progress() {
    // Did ffmpeg die without being asked to? Alt-tabbing out of an exclusive
    // fullscreen game changes the display mode, which invalidates the capture
    // session and kills the encoder with it. That is not recoverable in place,
    // so the segment is closed and a new one opened; the session continues as a
    // second file rather than ending without warning.
    LARGE_INTEGER qfreq{}, qnow{};
    QueryPerformanceFrequency(&qfreq);
    QueryPerformanceCounter(&qnow);
    const long long now_ms = qnow.QuadPart * 1000 / qfreq.QuadPart;

    if (active_ && ffmpeg_ && !stopping_ && !ffmpeg_->running()) {
        const std::string why = ffmpeg_->take_stderr();
        const auto code = ffmpeg_->exit_code();
        const long long lived = now_ms - segment_start_;
        OC_LOG_W("[rec] ffmpeg exited unexpectedly (code {}) after {} ms of segment",
                 code ? (long)*code : -1L, lived);
        if (!why.empty()) OC_LOG_W("[rec] ffmpeg said: {}", why);

        ffmpeg_.reset();
        for (auto& a : audio_) if (a) a->stop();
        audio_.clear();
        video_.reset();

        // A segment that died almost immediately means the display is still
        // changing, so wait longer before the next attempt. One that ran for a
        // while is a fresh, unrelated interruption and starts from the floor.
        if (lived < 5000) backoff_ms_ = backoff_ms_ ? std::min(backoff_ms_ * 2, 8000) : 1500;
        else              backoff_ms_ = 1500;

        if (resumes_ < 20) {
            resume_at_ms_ = now_ms + backoff_ms_;
            OC_LOG_I("[rec] waiting {} ms for the display to settle before resuming",
                     backoff_ms_);
            last_error_ = "capture interrupted; resuming...";
        } else {
            OC_LOG_E("[rec] too many resumes; giving up");
            // The session is over and cannot continue. Flagged rather than
            // silently ending, so the caller stops properly and says so instead
            // of leaving a stale "Recording..." on screen.
            failed_ = true;
            last_error_ = "capture stopped unexpectedly; see logs\\outriced.log";
        }
        return;
    }

    // Backoff elapsed: bring the capture back up.
    if (active_ && !stopping_ && resume_at_ms_ != 0 && now_ms >= resume_at_ms_) {
        resume_at_ms_ = 0;
        ++resumes_;
        const auto next = current_file_.parent_path() /
                          std::format(L"{}_pt{}.mp4",
                                      segments_.front().stem().wstring(), resumes_ + 1);
        OC_LOG_I("[rec] resuming into {}", next.string());
        if (launch(next)) {
            segment_start_ = now_ms;
            last_error_ = "capture was interrupted and resumed (part " +
                          std::to_string(resumes_ + 1) + ")";
        } else {
            OC_LOG_E("[rec] resume failed: {}", last_error_);
            failed_ = true;
            last_error_ = "capture stopped unexpectedly; see logs\\outriced.log";
        }
        return;
    }

    if (resume_at_ms_ != 0) return;   // nothing to read while the capture is down

    if (progress_file_.empty()) return;

    // ffmpeg appends a block roughly every stats_period and never rewrites, so
    // the file grows for the whole session. Re-reading it from the start on
    // every frame would cost more the longer the recording ran -- exactly the
    // wrong shape for something running underneath a game. Read only the bytes
    // added since last time, and only a few times a second: the numbers behind
    // this are refreshed twice a second by ffmpeg anyway, so polling faster
    // cannot show anything new.
    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const long long ms = now.QuadPart * 1000 / freq.QuadPart;
    if (last_progress_ms_ != 0 && ms - last_progress_ms_ < 250) return;
    last_progress_ms_ = ms;

    std::ifstream in(progress_file_, std::ios::binary);
    if (!in) return;

    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (end <= progress_offset_) return;   // nothing new since last poll

    in.seekg(progress_offset_, std::ios::beg);
    std::string chunk((size_t)(end - progress_offset_), '\0');
    in.read(chunk.data(), (std::streamsize)chunk.size());
    chunk.resize((size_t)in.gcount());

    // Only advance past the last complete line, so a block caught mid-write is
    // re-read rather than parsed in half.
    const auto last_nl = chunk.find_last_of('\n');
    if (last_nl == std::string::npos) return;
    progress_offset_ += (std::streamoff)(last_nl + 1);
    chunk.resize(last_nl);

    // Within the new bytes the last occurrence of each key is the current value.
    std::istringstream lines(chunk);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        try {
            if (key == "fps")             capture_fps_  = std::stod(val);
            else if (key == "total_size") output_bytes_ = std::stoull(val);
        } catch (const std::exception&) {}
    }
}

std::optional<std::filesystem::path> Recorder::stop() {
    stopping_     = true;
    active_       = false;
    resume_at_ms_ = 0;
    if (!ffmpeg_) {
        // Already gone: the resume path ran out of attempts. Still report the
        // last usable segment so the recording is not lost.
        markers_.clear();
        if (!segments_.empty()) {
            std::error_code ec;
            const auto size = std::filesystem::file_size(segments_.back(), ec);
            if (!ec && size > 0) return segments_.back();
        }
        return std::nullopt;
    }
    start_tick_ = 0;
    capture_fps_ = 0.0;
    output_bytes_ = 0;

    // 'q' rather than TerminateProcess, so ffmpeg writes the moov atom and the
    // MP4 is actually seekable.
    //
    // Without +faststart this is a short append of the index, so the timeouts
    // below are generous rather than load-bearing. The elapsed time is logged
    // because a slow finalise is exactly the symptom that would return if
    // anything reintroduced a whole-file rewrite here.
    LARGE_INTEGER f{}, t0{}, t1{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    // With native capture ffmpeg's video comes from a pipe we own, so the clean
    // ending is to close it: ffmpeg reaches EOF on input 0, drains what it has
    // and writes the moov atom by itself. Sending 'q' while still feeding it
    // frames does not work -- ffmpeg stayed wedged for the full 15 s timeout and
    // had to be terminated, truncating the file. The in-ffmpeg backends still
    // need 'q', because their capture filter never ends on its own.
    if (video_) {
        video_->stop();
        for (auto& a : audio_) if (a) a->stop();
    }

    ffmpeg_->write_stdin("q");

    if (!ffmpeg_->wait(10000)) {
        // ffmpeg is wedged; closing stdin gives it a second nudge before we
        // resort to killing it and accepting a possibly truncated file.
        ffmpeg_->close_stdin();
        if (!ffmpeg_->wait(5000)) {
            ffmpeg_->terminate();
            ffmpeg_->wait(2000);
            last_error_ = "ffmpeg did not exit cleanly; the file may be truncated";
            OC_LOG_E("[rec] ffmpeg had to be terminated; output may be truncated");
        }
    }

    QueryPerformanceCounter(&t1);
    OC_LOG_I("[rec] finalise took {} ms",
             (long long)((t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart));

    ffmpeg_.reset();
    for (auto& a : audio_) if (a) a->stop();
    audio_.clear();
    if (video_) {
        OC_LOG_I("[rec] capture: {} frames sent, {} repeated, {} dropped, {} resets",
                 video_->emitted(), video_->duplicated(), video_->dropped(),
                 video_->resets());
        video_.reset();
    }

    std::error_code ec;
    if (!progress_file_.empty()) {
        std::filesystem::remove(progress_file_, ec);
        progress_file_.clear();
    }
    const auto size = std::filesystem::file_size(current_file_, ec);
    if (ec || size == 0) {
        last_error_ = "no output was produced";
        markers_.clear();
        return std::nullopt;
    }

    // Seed the sidecar with the markers. The scanner fills in duration and
    // geometry on the next scan and preserves whatever is written here.
    if (!markers_.empty()) {
        nlohmann::json j{{"markers", markers_}};
        auto side = std::filesystem::path(current_file_).replace_extension(".json");
        if (std::ofstream out(side); out) out << j.dump(2);
    }
    markers_.clear();

    return current_file_;
}

} // namespace oc
