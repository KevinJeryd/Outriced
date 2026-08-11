#include "capture/encoders.h"

#include <windows.h>
#include <dxgi1_2.h>

#include <algorithm>

#include "platform/subprocess.h"

#pragma comment(lib, "dxgi.lib")

namespace oc {

std::string to_string(CaptureBackend b) {
    switch (b) {
    case CaptureBackend::Amf:     return "amf";
    case CaptureBackend::Cuda:    return "cuda";
    case CaptureBackend::Qsv:     return "qsv";
    case CaptureBackend::Native:  return "native";
    case CaptureBackend::Ddagrab: return "ddagrab";
    default:                      return "auto";
    }
}

CaptureBackend capture_backend_from_string(std::string_view s) {
    if (s == "amf")     return CaptureBackend::Amf;
    if (s == "cuda")    return CaptureBackend::Cuda;
    if (s == "qsv")     return CaptureBackend::Qsv;
    if (s == "native")  return CaptureBackend::Native;
    if (s == "ddagrab") return CaptureBackend::Ddagrab;
    return CaptureBackend::Auto;
}

CapturePipeline build_pipeline(CaptureBackend backend, int monitor_index,
                               int framerate, int width, int height,
                               bool draw_mouse, const std::string& encoder,
                               int poll_framerate) {
    CapturePipeline p;
    p.encoder = encoder;
    const bool scaling = width > 0 && height > 0;
    // Only the ddagrab paths poll; vsrc_amf waits on the compositor instead.
    const int poll = poll_framerate > 0 ? poll_framerate : framerate;
    std::ostringstream f;

    switch (backend) {
    case CaptureBackend::Native:
        // Nothing to build: frames arrive as rawvideo NV12 on a pipe, so there
        // is no capture filter and no hardware device for ffmpeg to open. The
        // recorder adds the input flags itself. See VideoCapture.
        if (p.encoder.empty()) p.encoder = "libx264";
        return p;

    case CaptureBackend::Amf:
        // AMD's own capture source hands AMF surfaces straight down the chain,
        // so no frame ever leaves the GPU. wait_for_present ties capture to the
        // compositor's flips rather than to a free-running timer.
        p.hw_device        = "amf=am";
        p.filter_hw_device = "am";
        f << "vsrc_amf=monitor_index=" << monitor_index
          << ":framerate=" << framerate
          << ":capture_mode=wait_for_present,vpp_amf=";
        if (scaling) f << "w=" << width << ":h=" << height << ":";
        f << "format=nv12:scale_type=bicubic";
        if (p.encoder.empty()) p.encoder = "h264_amf";
        break;

    case CaptureBackend::Cuda:
        // D3D11-to-CUDA interop, which is the pairing ffmpeg's ddagrab docs use
        // for NVENC. scale_cuda does the BGRA to NV12 conversion on the GPU.
        p.hw_device = "d3d11va=dx";
        f << "ddagrab=output_idx=" << monitor_index
          << ":framerate=" << poll
          << ":draw_mouse=" << (draw_mouse ? 1 : 0)
          << ",hwmap=derive_device=cuda,scale_cuda=";
        if (scaling) f << "w=" << width << ":h=" << height << ":";
        f << "format=nv12";
        if (p.encoder.empty()) p.encoder = "h264_nvenc";
        break;

    case CaptureBackend::Qsv:
        p.hw_device = "d3d11va=dx";
        f << "ddagrab=output_idx=" << monitor_index
          << ":framerate=" << poll
          << ":draw_mouse=" << (draw_mouse ? 1 : 0)
          << ",hwmap=derive_device=qsv,scale_qsv=";
        if (scaling) f << "w=" << width << ":h=" << height << ":";
        f << "format=nv12";
        if (p.encoder.empty()) p.encoder = "h264_qsv";
        break;

    default:
        // ddagrab hands out BGRA D3D11 frames that every GPU-side consumer on
        // the development machine rejected (scale_d3d11 and vpp_amf both refuse
        // RGB; the vulkan route needs D3D11 interop this build lacks), so these
        // come back to system memory for the colour conversion.
        //
        // The readback is not free but it is not the limit either. Staged at 4K
        // against a 60 fps source: capture alone 59.3 fps, plus readback 55.7,
        // plus scale/convert 54.2, plus encode 54.2. Most of what is lost goes
        // missing at capture, which is what `poll` above addresses.
        p.hw_device = "d3d11va";
        f << "ddagrab=output_idx=" << monitor_index
          << ":framerate=" << poll
          << ":draw_mouse=" << (draw_mouse ? 1 : 0)
          << ",hwdownload,format=bgra";
        if (scaling) f << ",scale=" << width << ":" << height << ":flags=bilinear";
        f << ",format=nv12";
        if (p.encoder.empty()) p.encoder = "libx264";
        break;
    }

    p.filter_chain = f.str();
    return p;
}

std::string vendor_name(Vendor v) {
    switch (v) {
    case Vendor::Amd:    return "AMD";
    case Vendor::Nvidia: return "NVIDIA";
    case Vendor::Intel:  return "Intel";
    default:             return "unknown";
    }
}

Vendor detect_vendor() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory)
        return Vendor::Unknown;

    Vendor best = Vendor::Unknown;
    SIZE_T best_vram = 0;
    for (UINT a = 0;; ++a) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;

        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            // Prefer the adapter with the most dedicated memory; on laptops the
            // integrated GPU enumerates first but is rarely the one to encode on.
            if (desc.DedicatedVideoMemory >= best_vram) {
                best_vram = desc.DedicatedVideoMemory;
                switch (desc.VendorId) {
                case 0x1002: case 0x1022: best = Vendor::Amd;    break;
                case 0x10DE:              best = Vendor::Nvidia; break;
                case 0x8086:              best = Vendor::Intel;  break;
                default:                  break;
                }
            }
        }
        adapter->Release();
    }
    factory->Release();
    return best;
}

std::vector<EncoderInfo> available_encoders(const std::filesystem::path& ffmpeg) {
    std::vector<EncoderInfo> out;
    if (!std::filesystem::exists(ffmpeg)) return out;

    std::wstring cmd = quote_arg(ffmpeg.wstring()) + L" -hide_banner -encoders";
    const auto listing = run_capture(cmd, 20000);
    if (!listing) return out;

    auto has = [&](const char* name) {
        return listing->find(name) != std::string::npos;
    };

    const Vendor v = detect_vendor();
    struct Candidate { const char* id; const char* label; Vendor vendor; };
    const Candidate all[] = {
        {"h264_amf",   "AMD (h264_amf)",       Vendor::Amd},
        {"h264_nvenc", "NVIDIA (h264_nvenc)",  Vendor::Nvidia},
        {"h264_qsv",   "Intel Quick Sync (h264_qsv)", Vendor::Intel},
    };

    // The matching vendor's encoder first, then the others as manual options.
    for (const auto& c : all)
        if (c.vendor == v && has(c.id)) out.push_back({c.id, c.label, true});
    for (const auto& c : all)
        if (c.vendor != v && has(c.id)) out.push_back({c.id, c.label, true});

    out.push_back({"libx264", "Software (libx264)", false});
    return out;
}

bool encoder_works(const std::filesystem::path& ffmpeg, const std::string& encoder) {
    if (!std::filesystem::exists(ffmpeg)) return false;

    std::wstring cmd;
    auto arg = [&](const std::wstring& a) { cmd += quote_arg(a); cmd += L' '; };
    arg(ffmpeg.wstring());
    arg(L"-hide_banner"); arg(L"-loglevel"); arg(L"error");
    arg(L"-f"); arg(L"lavfi");
    arg(L"-i"); arg(L"color=c=black:s=320x240:r=30");
    arg(L"-frames:v"); arg(L"3");
    arg(L"-c:v"); arg(to_wide(encoder));
    arg(L"-f"); arg(L"null"); arg(L"-");

    const auto out = run_capture(cmd, 25000);
    // ffmpeg is quiet at -loglevel error when the encode succeeds.
    return out && out->find("rror") == std::string::npos;
}

bool backend_works(const std::filesystem::path& ffmpeg, CaptureBackend backend,
                   int monitor_index) {
    if (!std::filesystem::exists(ffmpeg)) return false;
    if (backend == CaptureBackend::Auto) return false;
    // Native capture is not an ffmpeg filter, so there is no graph to probe.
    // VideoCapture::start() reports its own failure and the recorder falls back.
    if (backend == CaptureBackend::Native) return true;

    const auto p = build_pipeline(backend, monitor_index, 60, 640, 360, false, "");

    std::wstring cmd;
    auto arg = [&](const std::wstring& a) { cmd += quote_arg(a); cmd += L' '; };
    arg(ffmpeg.wstring());
    arg(L"-hide_banner"); arg(L"-loglevel"); arg(L"error");
    arg(L"-init_hw_device"); arg(to_wide(p.hw_device));
    if (!p.filter_hw_device.empty()) {
        arg(L"-filter_hw_device"); arg(to_wide(p.filter_hw_device));
    }
    arg(L"-filter_complex"); arg(to_wide(p.filter_chain));
    arg(L"-c:v"); arg(to_wide(p.encoder));
    arg(L"-frames:v"); arg(L"4");
    arg(L"-f"); arg(L"null"); arg(L"-");

    const auto out = run_capture(cmd, 30000);
    return out && out->find("rror") == std::string::npos;
}

CaptureBackend resolve_backend(CaptureBackend requested, Vendor vendor,
                               const std::filesystem::path& ffmpeg, int monitor_index) {
    if (requested != CaptureBackend::Auto) return requested;

    // Native first, on every vendor. The ffmpeg-side backends were measured
    // against OBS on the same machine and scene and all shared the same defect:
    // frames arrive at irregular instants, so motion lurches even when almost
    // no frame is duplicated. Only a dedicated capture thread on a strict grid
    // fixes that, and it is vendor-neutral. The rest stay selectable because
    // they are proven and this one is new.
    (void)vendor; (void)ffmpeg; (void)monitor_index;
    return CaptureBackend::Native;
}

} // namespace oc
