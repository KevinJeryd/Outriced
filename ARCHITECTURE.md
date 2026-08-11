# Architecture

Reference for anyone working on Outriced. It covers how the pieces fit together
and why the non-obvious decisions were made. Roughly 4,800 lines across 33 files,
so the whole thing is readable in an afternoon.

## Contents

1. [Mental model](#1-mental-model)
2. [Process architecture](#2-process-architecture)
3. [The five IPC channels](#3-the-five-ipc-channels)
4. [Module map](#4-module-map)
5. [Video capture](#5-video-capture)
6. [Audio capture](#6-audio-capture)
7. [Clip export](#7-clip-export)
8. [Threading model](#8-threading-model)
9. [Concurrency primitives](#9-concurrency-primitives)
10. [Invariants and pitfalls](#10-invariants-and-pitfalls)
11. [Debugging](#11-debugging)

---

## 1. Mental model

Outriced is a GUI that **captures the screen and the audio itself, and uses
ffmpeg as a subprocess to compress and mux what it captured.**

It did not start that way. ffmpeg used to capture the screen as well, and audio
was the only media data this code touched, because the bundled ffmpeg has no
WASAPI support. Video moved in here because how smooth a recording looks depends
on when each frame is taken, and there was no way to control that from outside
ffmpeg. Section 5 has the numbers. The old filter-based backends still work and
are still selectable.

What follows from that:

- Encoding behaviour changes by editing strings, not algorithms. Capture
  behaviour is now ordinary code here, with ordinary bugs.
- The recorder cannot call into ffmpeg or read its state. Everything goes through
  the five channels in section 3.
- Copying the command line out of the log into a terminal usually reproduces a
  recording problem. Not on the native backend, where the video input is a pipe
  that only this process fills.

---

## 2. Process architecture

### Library versus subprocess

Linking libavcodec would put ffmpeg's code inside `outriced.exe` and expose it as
function calls. It would also require implementing frame allocation, timestamp
handling, colour conversion and muxing by hand.

Running `ffmpeg.exe` instead reduces the entire encoder to a string. The cost is
that a subprocess is opaque: no functions to call, no state to read.

### Launching

`platform/subprocess.cpp` is generic and knows nothing about ffmpeg. It runs
whatever command line it is given:

```cpp
BOOL ok = CreateProcessW(
    nullptr,              // lpApplicationName: unused
    mutable_cmd.data(),   // lpCommandLine: first token is the executable
    ...
```

With `lpApplicationName` null, Windows takes the executable from the first token
of the command line. The path originates in `capture/recorder.cpp`:

```cpp
// settings.h:  std::string ffmpeg_path = "tools/ffmpeg/bin/ffmpeg.exe";
const std::filesystem::path ffmpeg = root / s.ffmpeg_path;
arg(ffmpeg.wstring());   // appended first, therefore the first token
```

The same class is reused for `ffprobe`, thumbnail extraction and clip export.

### Process inventory

| Process | Lifetime | Purpose |
|---|---|---|
| `outriced.exe` | the session | UI, screen capture, audio capture |
| `ffmpeg.exe` (recording) | one per segment | encode and mux; capture only on the ffmpeg-side backends |
| `ffmpeg.exe` (clip) | seconds to minutes | two-pass clip export |
| `ffmpeg.exe` (thumbnail) | under a second | single frame extraction |
| `ffprobe.exe` | under a second | duration and stream geometry |

---

## 3. The five IPC channels

While recording, `outriced.exe` and `ffmpeg.exe` are connected by exactly five
things. The video pipe exists only on the native backend; on the ffmpeg-side
backends there are four, and ffmpeg reads the screen itself.

```
                     ┌────────────────────────────┐
   "q"  ──stdin────▶ │                            │
                     │         ffmpeg.exe         │
  NV12 ──named pipe▶ │                            │
                     │  encodes and muxes;        │
   PCM ──named pipes▶│  captures nothing on the   │
                     │  native backend            │
   fps ◀──file─────  │                            │
                     │                            │
 errors ◀─stderr───  │                            │
                     └────────────────────────────┘
```

Recordings end differently on the two models. A capture filter never runs out of
input, so those backends have to be told to stop with `q`. A pipe does run out:
closing it is the stop signal and ffmpeg writes the moov atom by itself. Sending
`q` while still pushing frames into the pipe does not work, and the first version
did exactly that: ffmpeg sat there for the full 15 s timeout, got killed, and
truncated the file. `Recorder::stop()` now closes the capture first whenever
`video_` is set, and only falls back to `q` after that.

### 3.1 stdin: graceful stop

Every process is created with three streams: stdin (0), stdout (1) and stderr
(2). Their default sources are the keyboard and the console, but they can be
redirected to another process.

`Process::start()` creates a pipe and splits the ends between the two processes:

```cpp
HANDLE rd = nullptr, wr = nullptr;
CreatePipe(&rd, &wr, &sa, 0);      // rd = read end, wr = write end

// The child must not inherit our write end, or it never observes EOF.
SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);

si.hStdInput = rd;                 // ffmpeg's stdin becomes the pipe
...
stdin_ = wr;                       // we retain the write end
if (rd) CloseHandle(rd);           // the child owns its own copy now
```

ffmpeg stops cleanly when it reads `q` from stdin, so the app writes that byte:

```cpp
ffmpeg_->write_stdin("q");
```

This must not be replaced with `TerminateProcess`. An MP4's index, the **moov
atom**, is written when ffmpeg shuts down. Killing the process produces a file
with video data and no index, which no player can open.

The hotkey is translated into this byte through several layers:

```
Alt+Shift+F8
  -> WM_HOTKEY delivered to the tray window        platform/tray.cpp
  -> TrayEvent::ToggleRecording pushed to a queue  platform/tray.cpp
  -> main loop calls stop_recording()              app/main.cpp
  -> Recorder::stop()                              capture/recorder.cpp
  -> write_stdin("q")                              platform/subprocess.cpp
```

### 3.2 Named pipes: audio in

A named pipe is an in-memory channel between processes, identified by a name
under `\\.\pipe\`. It is not a file on disk and requires no cleanup; it ceases to
exist when both ends close.

One pipe is created per selected audio endpoint:

```cpp
HANDLE pipe = CreateNamedPipeW(
    pipe_name.c_str(),          // \\.\pipe\oc_audio_<pid>_<segment>_<index>
    PIPE_ACCESS_OUTBOUND,       // this process writes only
    PIPE_TYPE_BYTE | PIPE_WAIT,
    1,                          // a single client
    1 << 20, 1 << 20,           // 1 MB buffers, to absorb encoder hitches
    0, nullptr);
```

ffmpeg is told to read each pipe as an input. Because Windows named pipes are
opened with `CreateFile`, the same call used for regular files, ffmpeg opens the
pipe name exactly as it would open a filename:

```cpp
arg(L"-thread_queue_size"); arg(L"4096");
arg(L"-f");   arg(to_wide(audio_[i]->sample_format())); // f32le
arg(L"-ar");  arg(std::to_wstring(audio_[i]->sample_rate())); // 48000
arg(L"-ac");  arg(std::to_wstring(audio_[i]->channels()));    // 2
arg(L"-i");   arg(pipe_names[i]);                       // treated as a filename
```

The format flags are mandatory: raw PCM carries no header, so ffmpeg cannot infer
the sample rate or channel count.

**Why not stdin.** stdin is a byte stream and could carry audio; ffmpeg supports
`-i pipe:0`. It is unsuitable here for two reasons. A process has exactly one
stdin, which is already carrying `q`, and multiple endpoints require multiple
independent streams.

### 3.3 Progress file: statistics out

ffmpeg creates and appends to this file; the app only reads it.

```cpp
progress_file_ = sessions / (current_file_.stem().wstring() + L".progress");
arg(L"-progress"); arg(progress_file_.wstring());
arg(L"-stats_period"); arg(L"0.5");
```

Blocks of `key=value` lines are appended every 0.5 s. The file grows for the
whole session, about 2.7 MB over two hours, so only newly appended bytes are
read:

```cpp
in.seekg(0, std::ios::end);
const std::streamoff end = in.tellg();
if (end <= progress_offset_) return;        // nothing new

in.seekg(progress_offset_, std::ios::beg);  // resume from the last position
```

A file is used rather than a pipe because stdin is taken and a second pipe would
require another reader thread.

### 3.4 stderr: diagnostics out

Captured by a dedicated reader thread and written to the log when ffmpeg exits
unexpectedly. Without it, a failing capture leaves no explanation.

---

## 4. Module map

```
src/
  app/        entry point, settings, ImGui shell, native dialogs
  capture/    produces frames and samples
  media/      consumes finished files
  platform/   thin Win32 wrappers with no application logic
```

| File | Lines | Responsibility |
|---|---|---|
| `app/ui.cpp` | 1259 | All ImGui rendering and interaction |
| `capture/recorder.cpp` | 632 | Command-line assembly, process lifecycle, resume |
| `capture/video_capture.cpp` | 570 | Desktop Duplication, GPU scale/convert, timing grid |
| `app/main.cpp` | 396 | Event loop, wiring, policy decisions |
| `capture/audio_capture.cpp` | 287 | WASAPI capture into a named pipe |
| `media/clipper.cpp` | 280 | Budget calculation and two-pass export |
| `media/sessions.cpp` | 261 | Library scanning, sidecars, pruning |
| `capture/encoders.cpp` | 247 | Vendor detection, backend probing, graph building |
| `platform/overlay.cpp` | 236 | Always-on-top status window |
| `platform/subprocess.cpp` | 210 | Process creation and the stdin/stderr pipes |
| `media/player.cpp` | 206 | libmpv embedding |

Includes are folder-qualified (`#include "capture/recorder.h"`); `src/` is the
only include root.

---

## 5. Video capture

### Terminology

- **Desktop Duplication** is a Windows (DXGI) API that returns the composited
  desktop image. It is the standard mechanism for screen recording.
- **A filter** is one node in an ffmpeg processing graph. A *source filter* has
  no input and produces frames; `ddagrab` is a source filter wrapping Desktop
  Duplication.
- **AMF** is AMD's Advanced Media Framework, a library shipped in AMD's driver.
  `vsrc_amf` captures, `vpp_amf` scales and converts, `h264_amf` encodes.
- **DXGI** describes graphics hardware: adapters, outputs and their modes.

### Why capture moved out of ffmpeg

Recordings looked juddery, but counting duplicate frames said they were fine, and
that was true: hardly any frame was a duplicate. The frames were just unevenly
spaced in time. The file is written as constant 60 fps, so a player shows every
frame for 16.7 ms, but the content was sampled at irregular moments. Two frames
holding 40 ms of camera movement get played in 16.7 ms and the camera appears to
jump.

Recording the same scene in OBS gave something to compare against. Both numbers
below come from the per-frame `scene_score`: how much its spread varies, and how
big the 90th-percentile step is next to the median.

| | motion CV | 90th/median step | duplicated |
|---|---|---|---|
| ddagrab | 1.207 | 4.88x | 1.4% |
| native | 1.067 | 3.12x | 3.4% |
| OBS | 0.893 | 2.47x | 2.4% |

OBS duplicates more frames than ddagrab did and still looks better, so duplicate
count is close to useless as a quality measure. Optimising it is what hid the
real problem for so long.

`ddagrab` fetches a frame when the filtergraph asks for one, and the graph is
busy reading 33 MB back off the GPU, scaling it and encoding it. So the interval
between captures follows whatever the rest of the pipeline is doing. Measured at
60 fps: mean gap 18.4 ms, standard deviation 8.25 ms, worst 40.5 ms.

Everything reachable from the command line was tried first and none of it helped:

- Readback. Staged at 4K: capture alone 59.3 fps, plus readback 55.7, plus scale
  54.2, plus encode 54.2.
- The encoder, which turned out to cost nothing.
- Output resolution. 720p scored 1.173 against 1080p's 1.137.
- Poll rate. 0.245 / 0.257 / 0.278 at 1x / 2x / 4x.
- `fps_mode`, and matching the recording rate to a divisor of the 144 Hz panel.

### Backends

A backend is how frames are obtained. All of them sit on Desktop Duplication;
`vsrc_amf` exposes it through the option `duplicate_output`. AMF does **not**
bypass Windows.

`native` is the default on every vendor. `VideoCapture` splits capture from
timing across two threads:

- A capture thread blocks in `AcquireNextFrame`, which Windows wakes when the
  desktop presents. Each new image overwrites a single `latest` slot.
- A timer sends a frame every `1/framerate` seconds, whatever is in `latest` at
  that moment, scaled and converted by the D3D11 video processor. If nothing new
  arrived, the previous frame goes again.

The display runs at 144 Hz and the file is 60 fps, so something has to pick which
60 of those moments to keep. A steady timer picks them evenly; ffmpeg picked them
whenever its graph came round.

Doing the scale ourselves also worked around something that could not be done
inside ffmpeg at all. `scale_d3d11` rejected ddagrab's RGB frames, `vpp_amf`
failed with `QueryOutput() error 18`, and `-init_hw_device vulkan=vk@dx` returned
`Function not implemented`, which left the CPU as the only place to scale. One
`VideoProcessorBlt` now does the scale and the BGRA-to-NV12 conversion together,
so the readback drops from 33.2 MB per frame to 3.1 MB. Useful, but not why the
footage improved; the readback only ever cost about 3.6 fps.

| | native | ddagrab | AMF |
|---|---|---|---|
| Runs in | `outriced.exe` | ffmpeg | ffmpeg |
| Acquisition | blocking `AcquireNextFrame` | Desktop Duplication | Desktop Duplication |
| Emission | fixed timer, repeats when late | pulled by the graph | pulled by the graph |
| Conversion | `VideoProcessorBlt` on the GPU | `hwdownload`, CPU convert | `vpp_amf`, stays on the GPU |
| Readback per frame at 1080p | 3.1 MB | 33.2 MB | none |
| Vendor | any | any | AMD only |
| ffmpeg CPU over 12 s | not measured | 15.5 s (~129% of a core) | 3.0 s (~25% of a core) |

The difference is not the acquisition route. It is that AMD's capture, scaler and
encoder share a surface format, so frames never round-trip through system memory.
On the reference machine `ddagrab` frames were rejected by `h264_amf`,
`scale_d3d11` and `vpp_amf` alike, leaving a CPU conversion as the only option.

Representative graphs:

```
# ddagrab, portable
ddagrab=output_idx=0:framerate=60   # source: capture monitor 0
,hwdownload                         # GPU memory to system RAM
,format=bgra                        # interpret as BGRA
,scale=1920:1080:flags=bilinear     # downscale
,format=nv12                        # convert to the encoder's format

# AMF, AMD only
vsrc_amf=monitor_index=0:framerate=60:capture_mode=wait_for_present
,vpp_amf=w=1920:h=1080:format=nv12  # scale and convert on the GPU
```

### Vendor detection

`capture/encoders.cpp` enumerates DXGI adapters and selects by dedicated video
memory, because integrated GPUs enumerate first on laptops but should rarely be
the encoder:

```cpp
if (desc.DedicatedVideoMemory >= best_vram) {
    best_vram = desc.DedicatedVideoMemory;
    switch (desc.VendorId) {
    case 0x1002: case 0x1022: best = Vendor::Amd;    break;
    case 0x10DE:              best = Vendor::Nvidia; break;
    case 0x8086:              best = Vendor::Intel;  break;
    }
}
```

### Backend selection

`auto` returns `Native` on every vendor. Uneven capture timing is not a
vendor-specific problem and the vendor paths do not help with it.

There is no probe. `VideoCapture::start()` returns false if the monitor cannot be
duplicated, which happens when another capture tool holds it or the GPU has no
video processor, and the recorder drops to `ddagrab`:

```cpp
if (vcap->start(video_pipe, vc)) video_ = std::move(vcap);
else                             backend = CaptureBackend::Ddagrab;
```

That only catches failures during startup. One bug during development got past
`start()` and then captured nothing at all, because the source texture was
missing `D3D11_BIND_RENDER_TARGET` and the video processor refused it. `start()`
had already returned by then, so there was nothing left to fall back to. On
hardware nobody has tried, a black or empty recording is the thing to look for,
and the `[cap]` lines in the log say which call failed.

The ffmpeg-side backends still go through `backend_works()`, which runs the real
filter chain for four frames. Checking whether a filter exists by name is not
enough: `scale_d3d11` and `vpp_amf` are both present on the reference machine and
both fail on ddagrab's frames. The CUDA and QSV paths follow ffmpeg's documented
pattern but **have never been tested on hardware**.

### Frames are never dropped

`rawvideo` has no timestamps, so a frame's position in the stream is its time.
Dropping one does not leave a gap, it makes the file shorter, and the picture
jumps forward. When the queue fills up, capture waits for room (`kQueueWaitMs`)
rather than throwing anything away. An early version dropped instead and lost 12
frames in 6 seconds.

---

## 6. Audio capture

### Why it exists

The bundled ffmpeg has no `wasapi` input device; its device list is dshow,
gdigrab, lavfi, openal and vfwcap. ffmpeg therefore cannot open a Windows audio
endpoint. If it could, `capture/audio_capture.cpp`, the named pipes and the
silence padding would all be unnecessary and audio would be a single `-i` flag.

### Terminology

- An **endpoint** is one audio device, either playback (render) or capture.
- **WASAPI** is the Windows API for those endpoints.
- **Loopback** is a mode, not a device. Opening a *playback* endpoint with
  `AUDCLNT_STREAMFLAGS_LOOPBACK` records what is being played through it.
- **PCM** is uncompressed audio: raw amplitude samples with no header.

### Opening an endpoint

```cpp
// eRender for a playback device (used with loopback), eCapture for a microphone
device.p = open_endpoint(enumerator.p, device_id, loopback ? eRender : eCapture);
device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client.put());
client->GetMixFormat(&mix);     // the device dictates the format

// The loopback flag is the only difference between recording output and input.
client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                   loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                   kBufferDuration, 0, mix, nullptr);
client->Start();
```

Which endpoints are opened comes from settings: every id in `audio_outputs` as
loopback, then `audio_input` as a microphone. An empty `audio_outputs` means the
system default.

### Silence padding

Loopback delivers **nothing** while the system is silent, not zeroed buffers. A
naive relay would therefore produce an audio track shorter than the video, and
everything after the silence would drift.

A wall clock determines how many samples should exist, and the shortfall is
filled:

```cpp
const double elapsed = double(now.QuadPart - qpc_start.QuadPart) / double(qpc_freq.QuadPart);
const long long expected = (long long)(elapsed * sample_rate_);

// A 20 ms margin ensures only genuine silence is padded, never samples in flight.
long long deficit = expected - frames_written - sample_rate_ / 50;
while (deficit > 0 && !broken) {
    const size_t chunk = (size_t)std::min<long long>(deficit, (long long)silence.size() / bpf);
    broken = !write_all(silence.data(), chunk * bpf);
    frames_written += (long long)chunk;
    deficit -= (long long)chunk;
}
```

Measured drift over a 10 s recording: 60 ms.

### Mixing and track layout

Audio pipes are ffmpeg inputs `0..N-1`; the video comes from a filter and has no
input index. The graph labels every output explicitly, because ffmpeg's automatic
mapping selects only one audio stream:

```cpp
fc << pipeline.filter_chain << "[v]";        // label the video

for (size_t i = 0; i < n_audio; ++i) {
    fc << ";[" << i << ":a]";                // input i's audio
    // mic-only processing: channel repair, then gain
    //   pan=stereo|c0=c0|c1=c0   duplicates a mono capsule across both channels
    //   volume=<factor>          applies mic level
    fc << chain << "[a" << i << "]";
}

if (!separate && n_audio > 1) {
    // amix sums the streams. normalize=0 keeps each source at its own level
    // instead of attenuating everything as sources are added.
    fc << "amix=inputs=" << n_audio << ":duration=longest:normalize=0[amix]";
}
```

Track order is deterministic: ticked outputs in listed order, microphone last.
Names are written as `handler_name` rather than `title`, because MP4 has no
per-stream title and the mov muxer stores it in the track handler.

---

## 7. Clip export

### Budget

Bitrate is data per second, so the same size limit permits twice the bitrate over
half the duration:

```cpp
// Decimal megabytes: a 10 MB upload limit means 10,000,000 bytes, not 10 MiB.
// target_MB * 1e6 bytes * 8 bits / 1000 = target_MB * 8000 kilobits.
// ffmpeg's "k" suffix is decimal, so the units are consistent end to end.
const double total_kbps = (target_mb * 8000.0) / duration;
const double video_kbps = total_kbps - (double)audio_kbps;
return (int)std::clamp(video_kbps, 300.0, 40000.0);
```

### Resolution and frame rate

A ladder of heights (1080, 900, 720, 540, 360) is walked and the tallest rung
clearing roughly 0.045 bits per pixel per frame is chosen. The threshold is
deliberately low: downscaling costs detail unconditionally, whereas a bitrate
squeeze costs quality only on busy scenes. Frame rate is held at 60 until keeping
it would force the picture below 720p.

### Two-pass

Pass one encodes and discards the output, recording per-frame difficulty
statistics. Pass two reads those statistics and allocates the budget where it is
needed. A single pass must guess as it goes; two passes see the whole clip first.

```cpp
arg(L"-b:v"); arg(std::to_wstring(v_kbps) + L"k");
arg(L"-pass"); arg(L"1");                       // measure only
arg(L"-passlogfile"); arg(passlog.wstring());   // statistics destination
arg(L"-an");                                    // audio is irrelevant to pass 1
arg(L"-f"); arg(L"null"); arg(L"-");            // discard the output
```

Two-pass ABR overshot its target by 5 to 7% in every measured configuration, so
the request is placed 7% low and the rate capped. A size check afterwards
re-encodes once if the result still clears the cap.

`libx264` is used rather than a hardware encoder: software encoders are
substantially better at hitting an exact size, and a few seconds of video is
cheap to encode.

---

## 8. Threading model

A thread is created where the alternative is blocking the 60 Hz UI loop.

| Thread | Count | Location | Rationale |
|---|---|---|---|
| Main | 1 | `app/main.cpp` | SDL events, ImGui, polls all subsystems |
| Tray | 1 | `platform/tray.cpp` | Requires its own Win32 message loop |
| Audio capture | one per endpoint | `capture/audio_capture.cpp` | Must service the audio buffer every few ms |
| Library scan | up to 2 | `app/ui.cpp` | Runs `ffprobe` per file, seconds of work |
| Clip export | 1 | `media/clipper.cpp` | A two-pass encode takes 45 s or more |
| stderr reader | one per child | `platform/subprocess.cpp` | `ReadFile` blocks indefinitely |

libmpv additionally creates its own threads internally.

### Rules

- The main thread owns all UI state. Worker threads never call ImGui.
- Workers receive **copies** of anything the main thread might modify, and
  references only to objects with program lifetime.
- Results are published through a shared struct guarded by a mutex, with an
  atomic flag signalling availability.

### Worker lifecycle

A `std::thread` starts on construction; there is no separate start call.

```cpp
// 'lib' is one Library instance (there are two: sessions and clips), living in
// the global g_ui for the whole program, so a reference is safe.
// settings/root/clips are copied, because the main thread may change them.
lib.thread = std::thread([&lib, settings, root, clips] {
    auto found = clips ? scan_clips(settings, root) : scan_sessions(settings, root);
    for (const auto& s : found) ensure_thumbnail(s, settings, root);

    {
        std::lock_guard lock(lib.scan_mutex);
        lib.scanned = std::move(found);       // publish under the lock
    }
    lib.ready.store(true, std::memory_order_release);
    lib.scanning.store(false, std::memory_order_release);
});
```

A worker does not return to a caller. It runs to the end of its body and exits.
The handover point is the shared `Library` struct, which the main loop polls
every frame:

```cpp
void absorb_scan(Library& lib) {
    // exchange() reads and clears atomically, so a batch is collected exactly once.
    if (!lib.ready.exchange(false, std::memory_order_acquire)) return;

    std::lock_guard lock(lib.scan_mutex);
    lib.items = std::move(lib.scanned);
}
```

`join()` is called only before starting the next scan and during shutdown, never
mid-frame, since blocking is what the thread exists to avoid.

### Message loops and global hotkeys

Windows delivers input to a GUI program as *messages* queued for a window. A
program consumes them in a message loop:

```cpp
MSG msg;
while (GetMessageW(&msg, nullptr, 0, 0) > 0) {  // blocks until a message arrives
    TranslateMessage(&msg);
    DispatchMessageW(&msg);                     // invokes the window procedure
}
```

`RegisterHotKey` binds a key combination system-wide and delivers `WM_HOTKEY` to
a window regardless of focus, including over a fullscreen game. A window only
receives messages if something is pumping a loop for it, and SDL owns the main
thread's loop without forwarding `WM_HOTKEY`. The tray therefore creates its own
message-only window and runs its own loop on a dedicated thread, forwarding
results to the main thread through a mutex-protected queue.

---

## 9. Concurrency primitives

| Shared data | Primitive |
|---|---|
| A single bool, int or pointer | `std::atomic` |
| A struct, string, vector, or several values that must stay consistent | `std::mutex` |

An atomic guarantees a read never observes a partially written value, with no
locking cost. Suitable for the audio thread's stop flag, which is read thousands
of times per second:

```cpp
std::atomic<bool> stop_{false};
while (!stop_.load(std::memory_order_relaxed)) { ... }
```

A `std::vector` is a pointer, a size and a capacity. Updating it is not a single
instruction, so a concurrent reader could observe a new pointer with a stale
size. A mutex makes the update indivisible:

```cpp
std::mutex           scan_mutex;
std::vector<Session> scanned;

{
    std::lock_guard lock(scan_mutex);   // acquired here
    lib.scanned = std::move(found);
}                                       // released on scope exit
```

`std::lock_guard` releases on scope exit, including during exception unwinding.

---

## 10. Invariants and pitfalls

**`recording()` and `process_alive()` are distinct.** The first means a session
is open, the second that ffmpeg is currently running. Defining the recording
state as "is ffmpeg alive" makes the recovery path unreachable at precisely the
moment it is needed, because the flag flips false as the capture dies.

**The video is a separate window layered over the UI.** libmpv renders into a
child HWND above the ImGui surface. Anything ImGui draws in that rectangle is
invisible, which is why modals hide the video. A dialog centred on the viewport
would otherwise be hidden while still dimming the background and blocking input,
presenting as a frozen application.

**mpv holds an open handle to the loaded file.** Windows refuses to delete a file
with an open handle, so the player must be unloaded before removal.

**Sizes are decimal.** Upload limits count 10^6 bytes per megabyte. Computing in
MiB produces files that are silently rejected.

**`-fps_mode cfr` pads missing frames with duplicates.** A file can report 60 fps
and contain 20 unique frames. Frame count alone never demonstrates a healthy
recording.

**A static screen is indistinguishable from a capture stall** when measured with
`mpdecimate`. Loading screens produce identical frames and are reported exactly
like dropped ones.

**Leaving fullscreen invalidates the capture.** Desktop Duplication handles are
invalidated by mode changes, desktop switches and applications entering or
leaving exclusive fullscreen. Neither backend re-acquires, so ffmpeg exits and
the recorder starts a new segment after a backoff.

---

## 11. Debugging

The full ffmpeg command line is written to `logs/outriced.log` at `DBG` level.
Copying it into a terminal reproduces exactly what the application runs, with
output visible. This is the fastest route to diagnosing any capture problem.

`tests/record_test.exe` drives the recorder without the GUI and prints the
selected backend, encoder and audio endpoints, which makes pipeline changes
quick to evaluate.

Measuring real motion rather than the claimed frame rate:

```bash
tools/ffmpeg/bin/ffmpeg -i sessions/<file>.mp4 -vf "mpdecimate,showinfo" -f null -
```

The gaps between reported `pts_time` values are the signal. A healthy 60 fps
capture sits at 16.7 ms almost throughout.

Crashes produce `logs/crash_*.dmp`, openable in Visual Studio for the faulting
thread and call stack.
