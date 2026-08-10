#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace oc {

enum class Vendor { Unknown, Amd, Nvidia, Intel };

struct EncoderInfo {
    std::string id;      // "h264_amf", "h264_nvenc", "h264_qsv", "libx264"
    std::string label;   // "AMD (h264_amf)"
    bool        hardware = true;
};

// What the capture pipeline is built around. The first three keep frames on the
// GPU from capture to encode; Ddagrab is the universal fallback that pulls them
// back through system memory, which is what makes it expensive at 4K.
//
//   Amf     - vsrc_amf + vpp_amf. AMD only, and the only one measured on real
//             hardware here: a quarter of Ddagrab's CPU cost.
//   Cuda    - ddagrab + hwmap to CUDA + scale_cuda. NVIDIA only. This is the
//             pairing ffmpeg's own ddagrab documentation recommends for NVENC.
//   Qsv     - ddagrab + hwmap to QSV + scale_qsv. Intel only.
//   Ddagrab - DXGI duplication with a readback. Works on any vendor.
//
// Cuda and Qsv could not be verified without that hardware, so resolve_backend
// probes them at runtime and silently falls back to Ddagrab if they fail.
enum class CaptureBackend { Auto, Amf, Cuda, Qsv, Ddagrab };

std::string to_string(CaptureBackend b);
CaptureBackend capture_backend_from_string(std::string_view s);

// Reads the primary adapter's vendor from DXGI.
Vendor detect_vendor();
std::string vendor_name(Vendor v);

// Encoders this ffmpeg build advertises, best first for the detected vendor.
// libx264 is always appended as a guaranteed-working last resort.
std::vector<EncoderInfo> available_encoders(const std::filesystem::path& ffmpeg);

// Actually runs a 1-frame encode, because an encoder can be compiled in and
// still fail on the day (no driver, headless session, GPU already saturated).
bool encoder_works(const std::filesystem::path& ffmpeg, const std::string& encoder);

// The ffmpeg fragments that make up a capture pipeline. Built in one place so
// the runtime probe exercises exactly what the recorder will run.
struct CapturePipeline {
    std::string hw_device;         // -init_hw_device
    std::string filter_hw_device;  // -filter_hw_device, empty when not needed
    std::string filter_chain;      // -filter_complex
    std::string encoder;           // the encoder this pipeline's frames suit
};

CapturePipeline build_pipeline(CaptureBackend backend, int monitor_index,
                               int framerate, int width, int height,
                               bool draw_mouse, const std::string& encoder);

// Runs the backend's real filter chain for a few frames against the given
// monitor. This is the only trustworthy test: a filter can be compiled in and
// still fail on the day, which is exactly how scale_d3d11 and vpp_amf behave
// against ddagrab's textures on the development machine.
bool backend_works(const std::filesystem::path& ffmpeg, CaptureBackend backend,
                   int monitor_index);

// Resolves Auto to a concrete backend.
CaptureBackend resolve_backend(CaptureBackend requested, Vendor vendor,
                               const std::filesystem::path& ffmpeg, int monitor_index);

} // namespace oc
