#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "app/settings.h"

namespace oc {

struct Session {
    std::filesystem::path file;
    std::string           display_name;
    double                duration   = 0.0;   // seconds
    unsigned long long    size_bytes = 0;
    long long             mtime      = 0;
    int                   width      = 0;
    int                   height     = 0;
    int                   fps        = 0;
    std::vector<double>   markers;            // highlight positions, seconds
    std::vector<std::string> tags;            // free-form labels, user assigned
    std::filesystem::path thumbnail;          // may not exist yet
};

// Writes `sess.tags` into the video's sidecar, leaving every other key alone.
//
// Separate from the scan because tags are edited in the UI, while everything
// else in a sidecar is measured by ffprobe. This reads the existing file and
// puts it back rather than rebuilding it, so a key written by a different build
// is not silently dropped.
bool save_tags(const Session& sess);

// Scans a folder of videos, newest first. Duration and stream geometry come from
// ffprobe and are cached in a sidecar .json next to each file so repeat launches
// stay instant.
std::vector<Session> scan_videos(const std::filesystem::path& dir,
                                 const Settings& s,
                                 const std::filesystem::path& root);

std::vector<Session> scan_sessions(const Settings& s, const std::filesystem::path& root);
std::vector<Session> scan_clips(const Settings& s, const std::filesystem::path& root);

// Extracts a single frame if the thumbnail is not already cached. Safe to call
// off the UI thread.
bool ensure_thumbnail(const Session& sess, const Settings& s,
                      const std::filesystem::path& root);

std::string format_duration(double seconds);
std::string format_size(unsigned long long bytes);

// Total bytes of the videos in a folder, ignoring sidecars and thumbnails.
unsigned long long folder_size(const std::filesystem::path& dir);

// Deletes oldest-first until the folder fits within `limit_gb`. A limit of 0
// disables pruning. Returns how many files were removed.
int prune_folder(const std::filesystem::path& dir, double limit_gb);

} // namespace oc
