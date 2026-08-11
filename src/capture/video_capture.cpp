#include "capture/video_capture.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <vector>

#include "capture/displays.h"
#include "platform/log.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace oc {
namespace {

// How much may sit between capture and the pipe. Sized in bytes rather than
// frames because a 4K frame is four times a 1080p one, and a frame count that
// is comfortable at 1080p would be 200 MB at native resolution.
constexpr size_t kMaxQueuedBytes = 48u << 20;
constexpr size_t kMinQueued      = 4;
constexpr size_t kMaxQueuedCap   = 24;

// How long capture will wait for the writer before giving up on a frame.
// Dropping is a last resort: rawvideo carries no timestamps, so a dropped
// frame does not leave a gap in the file, it shortens it. That reads as the
// picture jumping forward, which is the exact artefact this class exists to
// remove, so it is worth stalling the grid to avoid.
constexpr int kQueueWaitMs = 400;

// A monitor's DXGI output plus the adapter that owns it. Indexing matches
// enumerate_displays() exactly -- global across adapters, desktop-attached
// only -- so a monitor_index means the same thing everywhere in the app.
bool find_output(int monitor_index, ComPtr<IDXGIAdapter1>& adapter_out,
                 ComPtr<IDXGIOutput1>& output_out, DXGI_OUTPUT_DESC& desc_out) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), &factory))) return false;

    int index = 0;
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) continue;
            if (index++ != monitor_index) continue;
            if (FAILED(output.As(&output_out))) return false;
            adapter_out = adapter;
            desc_out    = desc;
            return true;
        }
    }
    return false;
}

// Sleeping to the next grid point. A plain Sleep(1) rounds to the scheduler's
// tick and would smear the very spacing this class exists to protect, so this
// uses a high-resolution waitable timer where the OS has one and falls back to
// a short spin otherwise.
class GridTimer {
public:
    GridTimer() {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
        if (!timer_)  // pre-1803: still better than the scheduler tick
            timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    }
    ~GridTimer() { if (timer_) CloseHandle(timer_); }

    void sleep_until(long long qpc_target, long long qpf) {
        for (;;) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const long long remaining = qpc_target - now.QuadPart;
            if (remaining <= 0) return;
            const double ms = 1000.0 * (double)remaining / (double)qpf;
            if (ms < 0.6) continue;  // spin out the last fraction
            if (!timer_) { Sleep(ms > 2.0 ? (DWORD)(ms - 1.0) : 0); continue; }
            LARGE_INTEGER due{};
            due.QuadPart = -(LONGLONG)((ms - 0.4) * 10000.0);  // 100 ns units
            if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) return;
            WaitForSingleObject(timer_, INFINITE);
        }
    }

private:
    HANDLE timer_ = nullptr;
};

} // namespace

struct VideoCapture::Queue {
    std::mutex              m;
    std::condition_variable cv;        // writer waits for work
    std::condition_variable space;     // capture waits for room
    std::deque<std::vector<uint8_t>> q;
    size_t                  limit = kMinQueued;
    bool                    done = false;
};

VideoCapture::VideoCapture() = default;
VideoCapture::~VideoCapture() { stop(); }

std::string VideoCapture::error() const {
    std::lock_guard<std::mutex> lk(err_mutex_);
    return error_;
}

bool VideoCapture::start(const std::wstring& pipe_name, const Config& cfg) {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1>  output;
    DXGI_OUTPUT_DESC      desc{};
    if (!find_output(cfg.monitor_index, adapter, output, desc)) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        error_ = "monitor " + std::to_string(cfg.monitor_index) + " could not be found";
        return false;
    }

    const int src_w = desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left;
    const int src_h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
    out_w_ = cfg.width  > 0 ? cfg.width  : src_w;
    out_h_ = cfg.height > 0 ? cfg.height : src_h;
    out_w_ -= out_w_ % 2;   // NV12 has no odd dimensions
    out_h_ -= out_h_ % 2;
    if (out_w_ <= 0 || out_h_ <= 0) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        error_ = "invalid capture size";
        return false;
    }

    if (!queue_) queue_ = std::make_unique<Queue>();
    {
        const size_t frame_bytes = (size_t)out_w_ * out_h_ * 3 / 2;
        std::lock_guard<std::mutex> lk(queue_->m);
        queue_->q.clear();
        queue_->done  = false;
        queue_->limit = std::clamp(kMaxQueuedBytes / std::max<size_t>(frame_bytes, 1),
                                   kMinQueued, kMaxQueuedCap);
    }
    stop_.store(false, std::memory_order_relaxed);
    failed_.store(false, std::memory_order_relaxed);
    connected_.store(false, std::memory_order_relaxed);

    Config resolved = cfg;
    resolved.width  = out_w_;
    resolved.height = out_h_;

    writer_  = std::thread(&VideoCapture::writer_thread, this, pipe_name);
    capture_ = std::thread(&VideoCapture::capture_thread, this, resolved);
    return true;
}

void VideoCapture::stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (queue_) {
        {
            std::lock_guard<std::mutex> lk(queue_->m);
            queue_->done = true;
        }
        queue_->cv.notify_all();
        queue_->space.notify_all();   // capture may be waiting for room
    }
    if (capture_.joinable()) { OC_LOG_D("[cap] joining capture"); capture_.join(); }
    if (writer_.joinable())  { OC_LOG_D("[cap] joining writer");  writer_.join(); }
    OC_LOG_D("[cap] stopped");
}

void VideoCapture::writer_thread(std::wstring pipe_name) {
    auto fail = [&](const char* what) {
        {
            std::lock_guard<std::mutex> lk(err_mutex_);
            if (error_.empty()) error_ = what;
        }
        failed_.store(true, std::memory_order_relaxed);
    };

    // FILE_FLAG_OVERLAPPED is what makes the OVERLAPPED below mean anything. On
    // a synchronous handle ConnectNamedPipe ignores it and blocks outright, so
    // stop() could never join this thread if ffmpeg exited before connecting.
    HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(),
                                   PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_WAIT, 1,
                                   1 << 22, 1 << 22,   // 4 MB, about one 1080p frame
                                   0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { fail("CreateNamedPipe failed"); return; }

    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) { CloseHandle(pipe); fail("CreateEvent failed"); return; }

    OVERLAPPED ov{};
    ov.hEvent = ev;
    bool connected = false;
    if (ConnectNamedPipe(pipe, &ov)) {
        connected = true;
    } else {
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
            connected = true;
        } else if (err == ERROR_IO_PENDING) {
            // Bounded: if ffmpeg never opens its side it has died on its command
            // line, and waiting forever would only hide that.
            for (int waited = 0; waited < 150; ++waited) {
                if (stop_.load(std::memory_order_relaxed)) break;
                if (WaitForSingleObject(ev, 100) == WAIT_OBJECT_0) { connected = true; break; }
            }
            if (!connected) CancelIoEx(pipe, &ov);
        }
    }
    if (!connected) {
        if (!stop_.load(std::memory_order_relaxed))
            fail("ffmpeg never opened the video pipe");
        CloseHandle(ev);
        CloseHandle(pipe);
        return;
    }
    OC_LOG_I("[cap] ffmpeg connected to the video pipe");
    connected_.store(true, std::memory_order_relaxed);

    // Overlapped handles require every write to carry an OVERLAPPED too.
    auto write_all = [&](const uint8_t* p, size_t n) -> bool {
        while (n > 0) {
            OVERLAPPED wov{};
            wov.hEvent = ev;
            ResetEvent(ev);
            DWORD wrote = 0;
            if (!WriteFile(pipe, p, (DWORD)n, &wrote, &wov)) {
                if (GetLastError() != ERROR_IO_PENDING) return false;
                for (;;) {
                    if (WaitForSingleObject(ev, 200) == WAIT_OBJECT_0) break;
                    if (stop_.load(std::memory_order_relaxed)) {
                        CancelIoEx(pipe, &wov);
                        return false;
                    }
                }
                if (!GetOverlappedResult(pipe, &wov, &wrote, FALSE)) return false;
            }
            if (wrote == 0) return false;
            p += wrote;
            n -= wrote;
        }
        return true;
    };

    for (;;) {
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lk(queue_->m);
            queue_->cv.wait(lk, [this] { return !queue_->q.empty() || queue_->done; });
            if (queue_->q.empty()) break;
            frame = std::move(queue_->q.front());
            queue_->q.pop_front();
        }
        queue_->space.notify_one();
        if (!write_all(frame.data(), frame.size())) {
            // ffmpeg exited or the pipe broke; the recorder notices separately.
            break;
        }
    }

    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    CloseHandle(ev);
}

void VideoCapture::capture_thread(Config cfg) {
    auto fail = [&](const char* what) {
        {
            std::lock_guard<std::mutex> lk(err_mutex_);
            if (error_.empty()) error_ = what;
        }
        failed_.store(true, std::memory_order_relaxed);
        // Logged as well as recorded: start() has already returned by the time
        // most of these can happen, so nothing else would ever surface them.
        OC_LOG_E("[cap] {}", what);
    };

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1>  output;
    DXGI_OUTPUT_DESC      odesc{};
    if (!find_output(cfg.monitor_index, adapter, output, odesc)) { fail("monitor lost"); return; }

    ComPtr<ID3D11Device>        device;
    ComPtr<ID3D11DeviceContext> ctx;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                                 nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
        fail("D3D11CreateDevice failed");
        return;
    }

    ComPtr<ID3D11VideoDevice>   vdevice;
    ComPtr<ID3D11VideoContext>  vctx;
    if (FAILED(device.As(&vdevice)) || FAILED(ctx.As(&vctx))) {
        fail("no D3D11 video device; this GPU cannot convert on the GPU");
        return;
    }

    ComPtr<IDXGIOutputDuplication> dupl;
    auto open_duplication = [&]() -> bool {
        dupl.Reset();
        return SUCCEEDED(output->DuplicateOutput(device.Get(), &dupl)) && dupl;
    };
    if (!open_duplication()) {
        fail("DuplicateOutput failed; another capture tool may hold this monitor");
        return;
    }
    OC_LOG_I("[cap] duplicating monitor {} ({}x{}) -> {}x{} NV12 at {} fps",
             cfg.monitor_index,
             odesc.DesktopCoordinates.right  - odesc.DesktopCoordinates.left,
             odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top,
             cfg.width, cfg.height, cfg.framerate);

    // Built lazily from the first real frame, because the desktop's format is
    // not knowable up front: an HDR display duplicates as R16G16B16A16_FLOAT
    // rather than BGRA, and the video processor has to be told the truth.
    ComPtr<ID3D11Texture2D>                latest;      // newest desktop image
    ComPtr<ID3D11Texture2D>                nv12;        // scaled + converted
    ComPtr<ID3D11Texture2D>                staging;     // CPU-readable NV12
    ComPtr<ID3D11VideoProcessor>           vp;
    ComPtr<ID3D11VideoProcessorEnumerator> vpenum;
    ComPtr<ID3D11VideoProcessorOutputView> vpout;
    ComPtr<ID3D11VideoProcessorInputView>  vpin;
    DXGI_FORMAT src_fmt = DXGI_FORMAT_UNKNOWN;
    int src_w = 0, src_h = 0;

    auto build_pipeline = [&](const D3D11_TEXTURE2D_DESC& sd) -> bool {
        src_fmt = sd.Format;
        src_w   = (int)sd.Width;
        src_h   = (int)sd.Height;

        // Each step names itself: these fail for unrelated driver reasons (an
        // NV12 render target, a video processor that will not take RGB) and a
        // single combined message would not say which.
        auto step = [](const char* what, HRESULT hr) {
            if (FAILED(hr)) OC_LOG_E("[cap] {} failed: 0x{:08X}", what, (unsigned)hr);
            return SUCCEEDED(hr);
        };

        D3D11_TEXTURE2D_DESC td{};
        td.Width = sd.Width; td.Height = sd.Height;
        td.MipLevels = td.ArraySize = 1;
        td.Format = sd.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        // RENDER_TARGET as well as SHADER_RESOURCE: the video processor will not
        // accept an input view over a texture it cannot treat as a render target.
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (!step("CreateTexture2D(source)",
                  device->CreateTexture2D(&td, nullptr, &latest))) return false;

        D3D11_TEXTURE2D_DESC nd{};
        nd.Width = (UINT)cfg.width; nd.Height = (UINT)cfg.height;
        nd.MipLevels = nd.ArraySize = 1;
        nd.Format = DXGI_FORMAT_NV12;
        nd.SampleDesc.Count = 1;
        nd.Usage = D3D11_USAGE_DEFAULT;
        nd.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (!step("CreateTexture2D(nv12)",
                  device->CreateTexture2D(&nd, nullptr, &nv12))) return false;

        nd.Usage = D3D11_USAGE_STAGING;
        nd.BindFlags = 0;
        nd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (!step("CreateTexture2D(staging)",
                  device->CreateTexture2D(&nd, nullptr, &staging))) return false;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth   = sd.Width;   cd.InputHeight  = sd.Height;
        cd.OutputWidth  = (UINT)cfg.width; cd.OutputHeight = (UINT)cfg.height;
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (!step("CreateVideoProcessorEnumerator",
                  vdevice->CreateVideoProcessorEnumerator(&cd, &vpenum))) return false;
        if (!step("CreateVideoProcessor",
                  vdevice->CreateVideoProcessor(vpenum.Get(), 0, &vp))) return false;

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (!step("CreateVideoProcessorOutputView",
                  vdevice->CreateVideoProcessorOutputView(nv12.Get(), vpenum.Get(),
                                                          &ovd, &vpout))) return false;

        // `latest` is a fixed texture, so its view is built once here rather
        // than per frame: creating one every tick is an allocation inside the
        // timing loop, which is the last place that belongs.
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.FourCC = 0;
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.MipSlice = 0;
        vpin.Reset();
        if (!step("CreateVideoProcessorInputView",
                  vdevice->CreateVideoProcessorInputView(latest.Get(), vpenum.Get(),
                                                         &ivd, &vpin))) return false;

        // Without this the driver picks a colour space and the recording comes
        // back washed out or crushed. Desktop content is full-range sRGB; the
        // encoder wants studio-range BT.709, which is what players assume.
        ComPtr<ID3D11VideoContext1> vctx1;
        if (SUCCEEDED(vctx.As(&vctx1))) {
            vctx1->VideoProcessorSetStreamColorSpace1(
                vp.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            vctx1->VideoProcessorSetOutputColorSpace1(
                vp.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        } else {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs{};
            cs.Usage = 0; cs.RGB_Range = 0; cs.YCbCr_Matrix = 1; cs.Nominal_Range = 1;
            vctx->VideoProcessorSetOutputColorSpace(vp.Get(), &cs);
        }
        vctx->VideoProcessorSetStreamFrameFormat(vp.Get(), 0,
                                                 D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

        // Drivers enable "auto stream processing" by default, which on AMD can
        // include temporal work such as denoise and frame-rate conversion. That
        // is the last thing wanted here: this path exists to preserve the exact
        // frame the desktop presented, at the instant the grid asked for it.
        vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
        vctx->VideoProcessorSetStreamOutputRate(
            vp.Get(), 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE, nullptr);
        return true;
    };

    LARGE_INTEGER qpf{};
    QueryPerformanceFrequency(&qpf);
    const long long period = qpf.QuadPart / std::max(cfg.framerate, 1);

    // Do not start the grid until ffmpeg is reading, or the first seconds would
    // be spent filling and dropping from the queue.
    while (!connected_.load(std::memory_order_relaxed)) {
        if (stop_.load(std::memory_order_relaxed)) return;
        Sleep(5);
    }

    GridTimer      timer;
    LARGE_INTEGER  now{};
    QueryPerformanceCounter(&now);
    long long next_tick = now.QuadPart + period;
    bool have_frame = false;
    std::vector<uint8_t> scratch;

    while (!stop_.load(std::memory_order_relaxed)) {
        bool got_new = false;

        // Drain every desktop update that lands before the next grid point,
        // keeping only the newest. AcquireNextFrame blocks with a timeout, so
        // this waits efficiently rather than spinning.
        for (;;) {
            QueryPerformanceCounter(&now);
            const long long remaining = next_tick - now.QuadPart;
            if (remaining <= 0) break;
            const double rem_ms = 1000.0 * (double)remaining / (double)qpf.QuadPart;
            if (rem_ms < 1.5) break;   // let the grid timer land the last fraction
            const DWORD wait_ms = (DWORD)(rem_ms - 1.0);

            DXGI_OUTDUPL_FRAME_INFO info{};
            ComPtr<IDXGIResource> res;
            const HRESULT hr = dupl->AcquireNextFrame(wait_ms, &info, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (FAILED(hr)) {
                // Mode change, resolution change, or a fullscreen transition.
                // Recovering here keeps the recording in one piece, where the
                // ddagrab path had to restart ffmpeg and split the session.
                if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
                    resets_.fetch_add(1, std::memory_order_relaxed);
                    Sleep(50);
                    if (!open_duplication()) { Sleep(200); }
                    break;
                }
                fail("AcquireNextFrame failed");
                return;
            }

            // A mouse-only update carries no new desktop image; copying it
            // would burn bandwidth to produce an identical frame.
            if (info.LastPresentTime.QuadPart != 0) {
                ComPtr<ID3D11Texture2D> tex;
                if (SUCCEEDED(res.As(&tex)) && tex) {
                    D3D11_TEXTURE2D_DESC sd{};
                    tex->GetDesc(&sd);
                    if (!latest || sd.Format != src_fmt ||
                        (int)sd.Width != src_w || (int)sd.Height != src_h) {
                        if (!build_pipeline(sd)) {
                            dupl->ReleaseFrame();
                            fail("could not create the GPU scaling pipeline");
                            return;
                        }
                    }
                    ctx->CopyResource(latest.Get(), tex.Get());
                    if (!have_frame)
                        OC_LOG_I("[cap] first frame: {}x{} format {}",
                                 sd.Width, sd.Height, (int)sd.Format);
                    have_frame = true;
                    got_new    = true;
                }
            }
            dupl->ReleaseFrame();
        }

        timer.sleep_until(next_tick, qpf.QuadPart);
        next_tick += period;

        // A slot that arrives late (a long stall, a duplication reset) must not
        // push every later slot late with it, so the grid is re-anchored rather
        // than allowed to accumulate debt.
        QueryPerformanceCounter(&now);
        if (now.QuadPart - next_tick > period * 4)
            next_tick = now.QuadPart + period;

        if (!have_frame || !vpin) continue;   // nothing captured yet

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = vpin.Get();
        if (FAILED(vctx->VideoProcessorBlt(vp.Get(), vpout.Get(), 0, 1, &stream)))
            continue;

        ctx->CopyResource(staging.Get(), nv12.Get());

        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) continue;

        const int w = cfg.width, h = cfg.height;
        scratch.resize((size_t)w * h * 3 / 2);
        const uint8_t* src = (const uint8_t*)map.pData;
        uint8_t* dst = scratch.data();
        for (int y = 0; y < h; ++y)                       // Y plane
            memcpy(dst + (size_t)y * w, src + (size_t)y * map.RowPitch, w);
        const uint8_t* uv = src + (size_t)map.RowPitch * h;
        uint8_t* duv = dst + (size_t)w * h;
        for (int y = 0; y < h / 2; ++y)                   // interleaved UV
            memcpy(duv + (size_t)y * w, uv + (size_t)y * map.RowPitch, w);
        ctx->Unmap(staging.Get(), 0);

        {
            std::unique_lock<std::mutex> lk(queue_->m);
            // Wait for room rather than discarding: see kQueueWaitMs. Only a
            // writer that is properly stuck gets a frame thrown away, and that
            // is worth a warning because it will be visible.
            queue_->space.wait_for(lk, std::chrono::milliseconds(kQueueWaitMs),
                                   [this] {
                                       return queue_->q.size() < queue_->limit ||
                                              queue_->done ||
                                              stop_.load(std::memory_order_relaxed);
                                   });
            if (stop_.load(std::memory_order_relaxed)) break;
            if (queue_->q.size() >= queue_->limit) {
                queue_->q.pop_front();
                const long long n = dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n == 1 || n % 60 == 0)
                    OC_LOG_W("[cap] writer cannot keep up; {} frame(s) dropped", n);
            }
            queue_->q.push_back(scratch);
        }
        queue_->cv.notify_one();
        emitted_.fetch_add(1, std::memory_order_relaxed);
        if (!got_new) duplicated_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace oc
