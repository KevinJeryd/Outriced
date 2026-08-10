#include "capture/audio_capture.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <vector>

#include "capture/audio_devices.h"
#include "platform/subprocess.h"

namespace oc {
namespace {

// Minimal COM pointer; the capture thread owns everything for its lifetime.
template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** put() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Declared here so both start() and run() can open the same endpoint.
IMMDevice* open_endpoint(IMMDeviceEnumerator* enumerator,
                         const std::string& device_id, EDataFlow flow);

constexpr REFERENCE_TIME kBufferDuration = 10'000'000;  // 1 second, in 100ns units

} // namespace

AudioCapture::~AudioCapture() { stop(); }

std::string AudioCapture::error() const {
    std::lock_guard lock(err_mutex_);
    return error_;
}

namespace {

// Opens a named endpoint, or the default one for `flow` when the id is empty.
// Returns null on failure; the caller owns the reference.
IMMDevice* open_endpoint(IMMDeviceEnumerator* enumerator,
                         const std::string& device_id, EDataFlow flow) {
    IMMDevice* device = nullptr;
    if (!device_id.empty()) {
        const auto wid = to_wide(device_id);
        if (SUCCEEDED(enumerator->GetDevice(wid.c_str(), &device)) && device)
            return device;
        // Fall through: a saved device may have been unplugged since, and a
        // recording with the wrong audio beats one with none.
        device = nullptr;
    }
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device)))
        return device;
    return nullptr;
}

} // namespace

bool AudioCapture::start(const std::wstring& pipe_name,
                         const std::string& device_id, bool loopback) {
    // Probe the mix format on the calling thread so the caller can build the
    // ffmpeg command line before the child process is spawned.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_owned = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) hr = S_OK;
    if (FAILED(hr)) { error_ = "CoInitializeEx failed"; return false; }

    bool ok = false;
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice>           device;
        ComPtr<IAudioClient>        client;
        WAVEFORMATEX*               mix = nullptr;

        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                       CLSCTX_ALL, IID_PPV_ARGS(enumerator.put())))
            && (device.p = open_endpoint(enumerator.p, device_id,
                                         loopback ? eRender : eCapture)) != nullptr
            && SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                          nullptr, (void**)client.put()))
            && SUCCEEDED(client->GetMixFormat(&mix))) {

            sample_rate_ = (int)mix->nSamplesPerSec;
            channels_    = (int)mix->nChannels;

            bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
            if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
                is_float = ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            }
            if (is_float && mix->wBitsPerSample == 32)      sample_fmt_ = "f32le";
            else if (!is_float && mix->wBitsPerSample == 16) sample_fmt_ = "s16le";
            else                                             sample_fmt_ = nullptr;

            bytes_per_frame_ = mix->nBlockAlign;
            ok = sample_fmt_ != nullptr;
            if (!ok) error_ = "unsupported mix format (expected 32-bit float or 16-bit PCM)";
            CoTaskMemFree(mix);
        } else {
            error_ = "could not open the default render endpoint";
        }
    }
    if (com_owned) CoUninitialize();
    if (!ok) return false;

    label_ = device_id.empty() ? (loopback ? "default output" : "default input")
                               : audio_device_name(device_id);
    if (label_.empty()) label_ = device_id;

    stop_.store(false);
    failed_.store(false);
    thread_ = std::thread(&AudioCapture::run, this, pipe_name, device_id, loopback);
    return true;
}

void AudioCapture::stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
}

void AudioCapture::run(std::wstring pipe_name, std::string device_id, bool loopback) {
    auto fail = [&](const char* msg) {
        std::lock_guard lock(err_mutex_);
        if (error_.empty()) error_ = msg;
        failed_.store(true, std::memory_order_relaxed);
    };

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_owned = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) { fail("CoInitializeEx failed"); return; }

    HANDLE pipe = CreateNamedPipeW(
        pipe_name.c_str(),
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1,                 // one instance
        1 << 20, 1 << 20,  // 1 MB buffers, enough to ride out encoder hitches
        0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        fail("CreateNamedPipe failed");
        if (com_owned) CoUninitialize();
        return;
    }

    // ConnectNamedPipe blocks, so poll for the client instead: that keeps stop()
    // responsive if ffmpeg dies before it ever opens the pipe.
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = false;
    if (!ConnectNamedPipe(pipe, &ov)) {
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
            connected = true;
        } else if (err == ERROR_IO_PENDING) {
            while (!stop_.load(std::memory_order_relaxed)) {
                if (WaitForSingleObject(ov.hEvent, 100) == WAIT_OBJECT_0) { connected = true; break; }
            }
            if (!connected) CancelIoEx(pipe, &ov);
        }
    } else {
        connected = true;
    }
    if (!connected) {
        CloseHandle(ov.hEvent);
        CloseHandle(pipe);
        if (com_owned) CoUninitialize();
        return;  // stopped before ffmpeg attached; not an error worth surfacing
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice>           device;
    ComPtr<IAudioClient>        client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX*               mix = nullptr;

    auto cleanup = [&] {
        if (mix) CoTaskMemFree(mix);
        CloseHandle(ov.hEvent);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        if (com_owned) CoUninitialize();
    };

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(enumerator.put())))
        || (device.p = open_endpoint(enumerator.p, device_id,
                                     loopback ? eRender : eCapture)) == nullptr
        || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   (void**)client.put()))
        || FAILED(client->GetMixFormat(&mix))) {
        fail("failed to activate the audio endpoint");
        cleanup();
        return;
    }

    // Loopback only applies to playback endpoints; a microphone is captured
    // directly and rejects the flag.
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                                  kBufferDuration, 0, mix, nullptr))
        || FAILED(client->GetService(IID_PPV_ARGS(capture.put())))
        || FAILED(client->Start())) {
        fail("failed to start loopback capture");
        cleanup();
        return;
    }

    // Loopback delivers nothing at all while the system is silent, so a wall
    // clock drives how many frames *should* have been produced and any shortfall
    // is filled with silence. Without this the audio track drifts ahead of the
    // video by however long the machine was quiet.
    LARGE_INTEGER qpc_freq{}, qpc_start{};
    QueryPerformanceFrequency(&qpc_freq);
    QueryPerformanceCounter(&qpc_start);

    const int    bpf = bytes_per_frame_;
    long long    frames_written = 0;
    std::vector<BYTE> silence((size_t)sample_rate_ / 10 * bpf, 0);  // 100 ms

    DWORD mmcss_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &mmcss_index);

    auto write_all = [&](const BYTE* data, size_t bytes) -> bool {
        while (bytes > 0) {
            DWORD wrote = 0;
            if (!WriteFile(pipe, data, (DWORD)bytes, &wrote, nullptr) || wrote == 0)
                return false;
            data  += wrote;
            bytes -= wrote;
        }
        return true;
    };

    bool broken = false;
    while (!stop_.load(std::memory_order_relaxed) && !broken) {
        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE*  data  = nullptr;
            UINT32 avail = 0;
            DWORD  flags = 0;
            if (FAILED(capture->GetBuffer(&data, &avail, &flags, nullptr, nullptr)))
                break;

            const size_t bytes = (size_t)avail * bpf;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                if (silence.size() < bytes) silence.assign(bytes, 0);
                broken = !write_all(silence.data(), bytes);
            } else {
                broken = !write_all(data, bytes);
            }
            frames_written += avail;
            capture->ReleaseBuffer(avail);
            if (broken) break;
        }
        if (broken) break;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed = double(now.QuadPart - qpc_start.QuadPart) / double(qpc_freq.QuadPart);
        const long long expected = (long long)(elapsed * sample_rate_);

        // Leave a small margin so we only ever pad genuine silence, never race
        // ahead of samples that are about to arrive.
        long long deficit = expected - frames_written - sample_rate_ / 50;  // 20 ms
        while (deficit > 0 && !broken) {
            const size_t chunk = (size_t)std::min<long long>(deficit, (long long)silence.size() / bpf);
            broken = !write_all(silence.data(), chunk * bpf);
            frames_written += (long long)chunk;
            deficit -= (long long)chunk;
        }

        Sleep(10);
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    client->Stop();
    cleanup();
}

} // namespace oc
