#include "media/clipper.h"

#include <algorithm>
#include <format>

#include "platform/subprocess.h"

namespace oc {
namespace {

// Bits per pixel per frame, the usual yardstick for "will this look acceptable".
//
// This is deliberately near the low end. The budget it is compared against is a
// worst-case ceiling, but CRF usually spends well under it, so judging the
// ladder by the ceiling alone drops resolution further than the footage needs --
// a 20s clip was landing at 540p while using 3.8 MB of a 9 MB allowance.
// Downscaling costs detail unconditionally, whereas a bitrate squeeze only costs
// quality on the busy scenes, so err towards keeping the pixels.
constexpr double kComfortableBpp = 0.045;

struct Rung { int height; const char* name; };
constexpr Rung kLadder[] = {
    {1080, "1080p"}, {900, "900p"}, {720, "720p"}, {540, "540p"}, {360, "360p"},
};

} // namespace

ClipExporter::~ClipExporter() {
    if (thread_.joinable()) thread_.join();
}

int ClipExporter::target_video_kbps(double duration, double target_mb, int audio_kbps) {
    if (duration <= 0.1) duration = 0.1;
    // Decimal megabytes, because that is how the upload limit is counted: a
    // 10 MB cap means 10,000,000 bytes, not 10 MiB. Sizing in MiB and calling it
    // "9.9 MB" would produce 10,380,902 bytes and be rejected.
    //
    // target_MB * 1e6 bytes * 8 bits / 1000 = target_MB * 8000 kilobits, and
    // ffmpeg's "k" suffix is decimal too, so the units line up end to end.
    const double total_kbps = (target_mb * 8000.0) / duration;
    const double video_kbps = total_kbps - (double)audio_kbps;
    // A floor keeps very long selections from going to mush; the ceiling is
    // there because no short Discord clip needs more than this.
    return (int)std::clamp(video_kbps, 300.0, 40000.0);
}

namespace {

// Tallest rung whose bits-per-pixel clears the threshold at this frame rate.
// Returns 0 when even the bottom rung cannot.
int best_height_for(int video_kbps, int fps, int source_height) {
    for (const auto& rung : kLadder) {
        if (rung.height > source_height) continue;
        const double pixels = (double)rung.height * (rung.height * 16.0 / 9.0);
        const double bpp    = (video_kbps * 1000.0) / (pixels * fps);
        if (bpp >= kComfortableBpp) return rung.height;
    }
    return 0;
}

} // namespace

ClipExporter::Plan ClipExporter::plan(double duration, const Settings& s,
                                      int source_height, int source_fps) {
    Plan p;
    p.video_kbps = target_video_kbps(duration, s.target_clip_size_mb, s.clip_audio_kbps);

    if (source_height <= 0) source_height = 1080;
    if (source_fps    <= 0) source_fps    = 60;

    const int max_fps = std::min(s.clip_max_fps > 0 ? s.clip_max_fps : 60, source_fps);
    const int min_fps = std::clamp(s.clip_min_fps > 0 ? s.clip_min_fps : 30, 1, max_fps);
    p.fps = max_fps;

    switch (clip_quality_from_string(s.clip_quality)) {
    case ClipQuality::Source: p.height = source_height;                  return p;
    case ClipQuality::P1080:  p.height = std::min(1080, source_height);  return p;
    case ClipQuality::P720:   p.height = std::min(720,  source_height);  return p;
    case ClipQuality::P480:   p.height = std::min(480,  source_height);  return p;
    default: break;
    }

    // Prefer full frame rate while the budget still supports a decent frame
    // size. Motion clarity is most of what makes a game clip readable, so the
    // frame rate is only sacrificed once holding it would force the picture
    // below 720p -- at which point a sharper 30 fps image is the better trade.
    const int at_max = best_height_for(p.video_kbps, max_fps, source_height);
    if (at_max >= 720) {
        p.height = at_max;
        p.fps    = max_fps;
        return p;
    }

    const int at_min = best_height_for(p.video_kbps, min_fps, source_height);
    if (at_min > at_max) {
        p.height = at_min;
        p.fps    = min_fps;
        return p;
    }

    p.height = at_max > 0 ? at_max : std::min(360, source_height);
    p.fps    = max_fps;
    return p;
}

bool ClipExporter::begin(const Request& req, const Settings& s,
                         const std::filesystem::path& root) {
    if (busy_.load(std::memory_order_relaxed)) return false;
    if (req.out_point <= req.in_point) {
        status_ = "Mark Out must come after Mark In";
        return false;
    }
    if (thread_.joinable()) thread_.join();

    busy_.store(true, std::memory_order_relaxed);
    status_ = "Exporting...";
    thread_ = std::thread(&ClipExporter::run, this, req, s, root);
    return true;
}

void ClipExporter::run(Request req, Settings s, std::filesystem::path root) {
    const double duration = req.out_point - req.in_point;
    const int    src_h    = req.source_height > 0 ? req.source_height : 1080;

    const Plan pl      = plan(duration, s, src_h, req.source_fps);
    const int  height  = pl.height;
    const int  fps     = pl.fps;

    // Two-pass ABR overshot the budget by 5-7% in every measured configuration,
    // so aim below it and cap the rate as well. The margin costs a little
    // quality; going over costs the upload.
    const int v_kbps = (int)(pl.video_kbps * 0.93);

    std::error_code ec;
    const auto clips_dir = resolve_dir(root, s.clips_dir);
    std::filesystem::create_directories(clips_dir, ec);

    // <session>_<in-point>.mp4, with a suffix if that already exists.
    const auto stem = req.source.stem().wstring();
    const int  at   = (int)req.in_point;
    auto candidate  = clips_dir / std::format(L"{}_{:02d}m{:02d}s.mp4", stem, at / 60, at % 60);
    for (int n = 2; std::filesystem::exists(candidate, ec); ++n) {
        candidate = clips_dir / std::format(L"{}_{:02d}m{:02d}s_{}.mp4",
                                            stem, at / 60, at % 60, n);
    }

    const auto ffmpeg = root / s.ffmpeg_path;
    const auto passlog = clips_dir / (candidate.stem().wstring() + L"_pass");

    // Shared front of the command: the trim, the scale and the frame rate.
    // -ss before -i seeks fast; -t after gives an accurate duration because
    // ffmpeg decodes from the preceding keyframe.
    auto add_input = [&](std::wstring& cmd) {
        auto arg = [&](const std::wstring& a) { cmd += quote_arg(a); cmd += L' '; };
        arg(ffmpeg.wstring());
        arg(L"-hide_banner"); arg(L"-loglevel"); arg(L"error"); arg(L"-y");
        arg(L"-ss"); arg(to_wide(std::format("{:.3f}", req.in_point)));
        arg(L"-i");  arg(req.source.wstring());
        arg(L"-t");  arg(to_wide(std::format("{:.3f}", duration)));
        if (height < src_h) {
            arg(L"-vf"); arg(to_wide(std::format("scale=-2:{}:flags=lanczos", height)));
        }
        if (req.source_fps > 0 && fps < req.source_fps) {
            arg(L"-r"); arg(std::to_wstring(fps));
        }
        // libx264 rather than the hardware encoder: for a few seconds of video
        // the software encoder costs a moment and is far better at spending a
        // fixed budget, which is the whole game under an upload cap.
        arg(L"-c:v"); arg(L"libx264");
        arg(L"-preset"); arg(to_wide(s.clip_preset));
    };

    auto add_output = [&](std::wstring& cmd) {
        auto arg = [&](const std::wstring& a) { cmd += quote_arg(a); cmd += L' '; };
        arg(L"-pix_fmt"); arg(L"yuv420p");
        arg(L"-profile:v"); arg(L"high");
        arg(L"-c:a"); arg(L"aac");
        arg(L"-b:a"); arg(std::to_wstring(s.clip_audio_kbps) + L"k");
        arg(L"-ac");  arg(L"2");
        arg(L"-movflags"); arg(L"+faststart");
        arg(candidate.wstring());
    };

    std::optional<std::string> out;

    if (s.clip_two_pass) {
        // A first pass measures where the difficult frames are so the second can
        // put the bits there. This is what actually spends the whole allowance:
        // single-pass CRF left a third of a 9.9 MB budget unused on the same
        // footage, and unused budget is quality thrown away.
        status_ = "Exporting (pass 1 of 2)...";
        std::wstring p1;
        add_input(p1);
        {
            auto arg = [&](const std::wstring& a) { p1 += quote_arg(a); p1 += L' '; };
            arg(L"-b:v"); arg(std::to_wstring(v_kbps) + L"k");
            arg(L"-pass"); arg(L"1");
            arg(L"-passlogfile"); arg(passlog.wstring());
            arg(L"-an");
            arg(L"-f"); arg(L"null"); arg(L"-");
        }
        run_capture(p1, 900000);

        status_ = "Exporting (pass 2 of 2)...";
        std::wstring p2;
        add_input(p2);
        {
            auto arg = [&](const std::wstring& a) { p2 += quote_arg(a); p2 += L' '; };
            arg(L"-b:v");     arg(std::to_wstring(v_kbps) + L"k");
            arg(L"-maxrate"); arg(std::to_wstring((int)(v_kbps * 1.35)) + L"k");
            arg(L"-bufsize"); arg(std::to_wstring(v_kbps * 2) + L"k");
            arg(L"-pass"); arg(L"2");
            arg(L"-passlogfile"); arg(passlog.wstring());
        }
        add_output(p2);
        out = run_capture(p2, 900000);

        // x264 leaves its stats files behind.
        for (const auto* suffix : {L"-0.log", L"-0.log.mbtree", L".log", L".log.mbtree"}) {
            std::filesystem::remove(passlog.wstring() + suffix, ec);
        }
    } else {
        std::wstring cmd;
        add_input(cmd);
        {
            auto arg = [&](const std::wstring& a) { cmd += quote_arg(a); cmd += L' '; };
            // CRF decides how many bits the footage deserves and VBV stops it
            // exceeding the budget, so easy clips stay small.
            arg(L"-crf");     arg(std::to_wstring(s.clip_crf));
            arg(L"-maxrate"); arg(std::to_wstring(v_kbps) + L"k");
            arg(L"-bufsize"); arg(std::to_wstring(v_kbps) + L"k");
        }
        add_output(cmd);
        out = run_capture(cmd, 900000);
    }

    const auto size = std::filesystem::file_size(candidate, ec);
    if (ec || size == 0) {
        status_ = "Export failed";
        if (out && !out->empty()) status_ += ": " + out->substr(0, 200);
        busy_.store(false, std::memory_order_relaxed);
        return;
    }

    // Reported in decimal MB so the number here is the number Discord checks.
    const double mb = (double)size / 1e6;
    last_output_ = candidate;
    status_ = std::format("Saved {}  ({:.2f} MB, {}p{})",
                          candidate.filename().string(), mb, height, fps);

    // Last line of defence. The margin above should prevent this, but a file
    // over the cap is useless, so shrink by however much it actually overshot
    // and encode once more at a hard-capped rate.
    if (mb > s.target_clip_size_mb) {
        status_ = std::format("{:.2f} MB over target, re-encoding...", mb);

        const double ratio  = (s.target_clip_size_mb / mb) * 0.95;
        const int    strict = std::max(300, (int)(v_kbps * ratio));

        std::wstring cmd2;
        add_input(cmd2);
        {
            auto arg = [&](const std::wstring& a) { cmd2 += quote_arg(a); cmd2 += L' '; };
            arg(L"-b:v");     arg(std::to_wstring(strict) + L"k");
            arg(L"-maxrate"); arg(std::to_wstring(strict) + L"k");
            arg(L"-bufsize"); arg(std::to_wstring(strict) + L"k");
        }
        add_output(cmd2);
        run_capture(cmd2, 900000);

        const auto size2 = std::filesystem::file_size(candidate, ec);
        const double mb2 = ec ? mb : (double)size2 / 1e6;
        status_ = std::format("Saved {}  ({:.2f} MB, {}p{})",
                              candidate.filename().string(), mb2, height, fps);
    }

    busy_.store(false, std::memory_order_relaxed);
}

} // namespace oc
