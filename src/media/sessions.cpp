#include "media/sessions.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "platform/log.h"
#include "platform/subprocess.h"

using nlohmann::json;

namespace oc {
namespace {

std::filesystem::path sidecar_for(const std::filesystem::path& video) {
    return std::filesystem::path(video).replace_extension(".json");
}

std::filesystem::path thumb_for(const std::filesystem::path& video) {
    // BMP, so SDL_LoadBMP can read it without pulling in SDL_image.
    const auto dir = video.parent_path() / ".thumbs";
    return dir / (video.stem().wstring() + L".bmp");
}

struct Probe {
    double duration = 0.0;
    int    width = 0, height = 0, fps = 0;
};

// One ffprobe call for everything the UI and the clipper need.
Probe probe_video(const std::filesystem::path& video,
                  const Settings& s,
                  const std::filesystem::path& root) {
    Probe p;
    const auto ffprobe = root / s.ffprobe_path;
    if (!std::filesystem::exists(ffprobe)) return p;

    std::wstring cmd;
    auto arg = [&](const std::wstring& a) { cmd += quote_arg(a); cmd += L' '; };
    arg(ffprobe.wstring());
    arg(L"-v"); arg(L"error");
    arg(L"-select_streams"); arg(L"v:0");
    arg(L"-show_entries"); arg(L"format=duration:stream=width,height,r_frame_rate");
    arg(L"-of"); arg(L"default=noprint_wrappers=1");
    arg(video.wstring());

    const auto out = run_capture(cmd);
    if (!out) return p;

    std::istringstream in(*out);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();

        try {
            if (key == "duration")     p.duration = std::stod(value);
            else if (key == "width")   p.width    = std::stoi(value);
            else if (key == "height")  p.height   = std::stoi(value);
            else if (key == "r_frame_rate") {
                // Reported as a rational, e.g. "60/1".
                const auto slash = value.find('/');
                if (slash != std::string::npos) {
                    const double num = std::stod(value.substr(0, slash));
                    const double den = std::stod(value.substr(slash + 1));
                    if (den > 0) p.fps = (int)(num / den + 0.5);
                } else {
                    p.fps = (int)std::stod(value);
                }
            }
        } catch (const std::exception&) {}
    }
    return p;
}

} // namespace

std::string format_duration(double seconds) {
    if (seconds < 0) seconds = 0;
    const int total = (int)(seconds + 0.5);
    const int h = total / 3600, m = (total % 3600) / 60, sec = total % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
    else       snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
    return buf;
}

std::string format_size(unsigned long long bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; ++u; }
    char buf[32];
    snprintf(buf, sizeof(buf), (u == 0 ? "%.0f %s" : "%.1f %s"), v, units[u]);
    return buf;
}

std::vector<Session> scan_videos(const std::filesystem::path& dir,
                                 const Settings& s,
                                 const std::filesystem::path& root) {
    std::vector<Session> out;

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return out;

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;

        auto ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext != L".mp4" && ext != L".mkv") continue;

        Session sess;
        sess.file         = entry.path();
        sess.display_name = entry.path().stem().string();
        sess.size_bytes   = entry.file_size(ec);
        sess.thumbnail    = thumb_for(entry.path());

        const auto wt = std::filesystem::last_write_time(entry.path(), ec);
        sess.mtime = ec ? 0 : wt.time_since_epoch().count();

        // Reuse the cached probe unless the file changed size since we ran it.
        const auto side = sidecar_for(entry.path());
        bool cached = false;
        if (std::ifstream in(side); in) {
            try {
                json j; in >> j;
                // Markers are written by the recorder and must survive a
                // re-probe, so they are read whether or not the rest is stale.
                if (auto m = j.find("markers"); m != j.end() && m->is_array())
                    sess.markers = m->get<std::vector<double>>();
                if (j.value("size_bytes", 0ull) == sess.size_bytes) {
                    sess.duration = j.value("duration", 0.0);
                    sess.width    = j.value("width", 0);
                    sess.height   = j.value("height", 0);
                    sess.fps      = j.value("fps", 0);
                    cached = sess.duration > 0.0 && sess.height > 0;
                }
            } catch (const std::exception&) {}
        }
        if (!cached) {
            const Probe p = probe_video(entry.path(), s, root);
            sess.duration = p.duration;
            sess.width    = p.width;
            sess.height   = p.height;
            sess.fps      = p.fps;
            if (sess.duration > 0.0) {
                json j{{"duration", sess.duration}, {"size_bytes", sess.size_bytes},
                       {"width", sess.width}, {"height", sess.height}, {"fps", sess.fps}};
                if (!sess.markers.empty()) j["markers"] = sess.markers;
                if (std::ofstream o(side); o) o << j.dump(2);
            }
        }
        out.push_back(std::move(sess));
    }

    std::sort(out.begin(), out.end(),
              [](const Session& a, const Session& b) { return a.mtime > b.mtime; });
    return out;
}

unsigned long long folder_size(const std::filesystem::path& dir) {
    unsigned long long total = 0;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return 0;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        auto ext = e.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext == L".mp4" || ext == L".mkv") total += e.file_size(ec);
    }
    return total;
}

int prune_folder(const std::filesystem::path& dir, double limit_gb) {
    if (limit_gb <= 0.0) return 0;
    const auto limit = (unsigned long long)(limit_gb * 1e9);

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return 0;

    struct Entry { std::filesystem::path path; unsigned long long size; long long mtime; };
    std::vector<Entry> files;
    unsigned long long total = 0;

    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        auto ext = e.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext != L".mp4" && ext != L".mkv") continue;

        const auto size = e.file_size(ec);
        const auto wt   = std::filesystem::last_write_time(e.path(), ec);
        files.push_back({e.path(), size, ec ? 0 : wt.time_since_epoch().count()});
        total += size;
    }
    if (total <= limit) return 0;

    // Oldest first, so the newest recording is always the last thing to go.
    std::sort(files.begin(), files.end(),
              [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });

    int removed = 0;
    for (const auto& f : files) {
        if (total <= limit) break;
        // Take the sidecar and thumbnail with it so nothing is orphaned.
        std::filesystem::remove(std::filesystem::path(f.path).replace_extension(".json"), ec);
        std::filesystem::remove(dir / ".thumbs" / (f.path.stem().wstring() + L".bmp"), ec);
        if (std::filesystem::remove(f.path, ec)) {
            total -= f.size;
            ++removed;
            OC_LOG_I("[prune] removed {} ({})", f.path.filename().string(),
                     format_size(f.size));
        }
    }
    return removed;
}

std::vector<Session> scan_sessions(const Settings& s, const std::filesystem::path& root) {
    return scan_videos(resolve_dir(root, s.sessions_dir), s, root);
}

std::vector<Session> scan_clips(const Settings& s, const std::filesystem::path& root) {
    return scan_videos(resolve_dir(root, s.clips_dir), s, root);
}

bool ensure_thumbnail(const Session& sess, const Settings& s,
                      const std::filesystem::path& root) {
    std::error_code ec;
    if (std::filesystem::exists(sess.thumbnail, ec)) return true;

    const auto ffmpeg = root / s.ffmpeg_path;
    if (!std::filesystem::exists(ffmpeg)) return false;
    std::filesystem::create_directories(sess.thumbnail.parent_path(), ec);

    // Grab a frame from a little way in; the first frame of a session is often
    // still the desktop.
    const double at = sess.duration > 6.0 ? 5.0 : sess.duration * 0.25;

    std::wstring cmd;
    auto arg = [&](const std::wstring& a) { cmd += quote_arg(a); cmd += L' '; };
    arg(ffmpeg.wstring());
    arg(L"-hide_banner"); arg(L"-loglevel"); arg(L"error"); arg(L"-y");
    arg(L"-ss"); arg(to_wide(std::to_string(at)));
    arg(L"-i");  arg(sess.file.wstring());
    arg(L"-frames:v"); arg(L"1");
    arg(L"-vf"); arg(L"scale=320:-2,format=bgr24");
    arg(sess.thumbnail.wstring());

    run_capture(cmd, 20000);
    return std::filesystem::exists(sess.thumbnail, ec);
}

} // namespace oc
