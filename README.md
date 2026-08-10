# Outriced

Tray-resident screen recorder with a clip cutter that targets Discord's upload
cap. Windows only, C++20, CMake + FetchContent, SDL3 + Dear ImGui, libmpv for
playback, ffmpeg driven as a subprocess.

## Layout

```
src/
  app/        entry point, settings, the ImGui shell, native dialogs
  capture/    everything that produces frames or samples:
              recorder, WASAPI capture, device enumeration,
              encoder/backend probing, display enumeration, game watcher
  media/      everything that consumes files: session scanning,
              clip export, libmpv playback
  platform/   thin Win32 wrappers with no app logic:
              subprocess, tray, overlay, logging
tests/        console harnesses, one per subsystem
tools/        packaging and dependency scripts
```

Includes are folder-qualified (`#include "capture/recorder.h"`). `src/` is the
only include root.

`ARCHITECTURE.md` covers how the pieces fit together: the subprocess model, the
four channels used to talk to ffmpeg, the capture pipelines, the threading model
and the invariants worth knowing before changing anything.

## Build

```bash
cmake -S . -B build -A x64 && cmake --build build --config Release
```

No `-G`: CMake selects the newest installed Visual Studio. Pin it with
`-G "Visual Studio 17 2022"` only if you have several and want a specific one.

The binary lands in `build/Release/outriced.exe`. It resolves `sessions/`,
`clips/`, `settings.json` and `tools/ffmpeg/` by walking up from the executable,
so running it from the build folder still uses the top-level directories.

### Dependencies

The binary dependencies are **not** in the repository; they are around 400 MB.
Two scripts fetch them. Both are safe to re-run and no-op if the dependency is
already present.

```bash
powershell -ExecutionPolicy Bypass -File tools/fetch_ffmpeg.ps1
powershell -ExecutionPolicy Bypass -File tools/fetch_mpv.ps1
```

- **SDL3, Dear ImGui, nlohmann/json**: fetched from source by CMake, nothing to do.
- **ffmpeg** into `tools/ffmpeg/`. A BtbN `win64-gpl` build, because the capture
  pipeline needs `ddagrab`, `vsrc_amf`/`vpp_amf`, a hardware H.264 encoder and
  `libx264` for clip export. A stock ffmpeg is usually missing at least one of
  those, so the app ignores whatever is on PATH and uses this copy. The script
  checks for each after downloading and warns if any is absent.
- **libmpv** into `third_party/mpv/`, for playback and scrubbing. The upstream
  package ships only a MinGW import library, so the script derives an MSVC one
  from the DLL's export table via `dumpbin` and `lib`. Requires 7-Zip, since the
  package is a `.7z`.

### Packaging a release

```bash
powershell -ExecutionPolicy Bypass -File tools/package.ps1
```

Produces `dist/Outriced.zip` (about 152 MB) containing the exe, SDL3, libmpv,
ffmpeg and ffprobe. Unzip and run; nothing to install, no PATH setup. On first
launch it detects the GPU, picks an encoder and capture method, and writes its
own `settings.json`.

`ffplay.exe` is excluded from the archive. Nothing in the app invokes it.

Pushing a `v*` tag runs the same scripts on a Windows runner and attaches the zip
to a GitHub Release; see `.github/workflows/release.yml`. The workflow calls the
same fetch scripts a developer would, so CI exercises the documented setup path.

### Licence

GPL v3; see `LICENSE`. The release archive bundles GPL-licensed ffmpeg and libmpv
binaries, so the combined work is distributed under compatible terms.
`THIRD-PARTY-NOTICES.md` covers every dependency.

## Use

The app opens as a window. Closing it hides to the tray with the recorder still
running; **Quit** from the tray menu exits.

**Alt+Shift+F8** toggles recording, as does the tray menu. When a recording stops
the window surfaces with the new session on top.

Three tabs:

- **Sessions**: double-click to preview, scrub, **Mark In** / **Mark Out**, then
  **Save clip**.
- **Clips**: the same browser over `clips/`.
- **Settings**: monitor, resolution, encoder, capture method, audio devices, clip
  sizing, folders and hotkeys. Everything writes to `settings.json`; capture
  changes take effect on the next recording.

## Settings

Everything is editable in the Settings tab, and `settings.json` is the same data
on disk. Anything left blank is resolved by probing the machine on first run.

| Key | Default | Notes |
|---|---|---|
| `monitor_index` | `0` | DXGI output index; the Settings tab lists them with resolutions |
| `capture_width` / `capture_height` | `1920` / `1080` | Both `0` records at native resolution |
| `framerate` | `60` | Capped to the monitor's refresh rate |
| `encoder` | auto | `h264_amf` / `h264_nvenc` / `h264_qsv` / `libx264`, detected from the GPU |
| `capture_backend` | `auto` | `auto`, `amf`, `cuda`, `qsv` or `ddagrab` |
| `session_bitrate_kbps` | `12000` | |
| `capture_audio` / `draw_mouse` | `true` | |
| `audio_outputs` | `[]` | Endpoint ids to record via loopback; empty uses the system default |
| `audio_input` | `""` | Microphone endpoint id; empty records none |
| `audio_track_mode` | `mixed` | `mixed` or `separate` |
| `mic_gain_percent` | `100` | `volume` filter applied to the mic before mixing |
| `mic_channel_mode` | `stereo` | `stereo`, `mono_left` or `mono_mix` |
| `target_clip_size_mb` | `9.9` | Decimal MB. Discord: 10 MB free, 50 MB Nitro Basic, 500 MB Nitro |
| `clip_quality` | `auto` | `auto`, `source`, `1080p`, `720p`, `480p` |
| `clip_preset` | `veryslow` | x264 preset for clips |
| `clip_crf` | `20` | Single-pass only; unused in two-pass mode |
| `clip_audio_kbps` | `128` | AAC bitrate for exported clips |
| `clip_two_pass` | `true` | Two passes fill the budget; off is faster and smaller |
| `clip_max_fps` / `clip_min_fps` | `60` / `30` | Equal values pin the clip frame rate |
| `max_sessions_gb` / `max_clips_gb` | `50` / `0` | `0` disables pruning; oldest deleted first |
| `sessions_dir` / `clips_dir` | `sessions` / `clips` | Any folder; see below |
| `auto_record_enabled` / `auto_record_games` | `false` / `[]` | Executable names to start and stop with |
| `overlay_enabled` / `overlay_monitor` | `false` / `-1` | `-1` picks a screen you are not recording |
| `hotkey_mods` / `hotkey_vk` | `5` / `119` | Win32 `MOD_*` mask and virtual-key code; `5` = ALT\|SHIFT, `119` = F8 |
| `marker_hotkey_mods` / `marker_hotkey_vk` | `5` / `120` | Highlight marker; `120` = F9 |

## Audio devices

By default the system's default output is recorded via loopback. The Settings tab
lists every active endpoint and allows ticking more than one output and picking a
microphone.

Each selected endpoint gets its own WASAPI capture thread and its own named pipe,
and becomes a separate ffmpeg input. From there:

- **Mixed** sums them into one track with `amix` (`normalize=0`, so adding a
  source does not quieten the others). Ready to upload.
- **Separate** keeps one track per device, for editing voice and game apart.

Track order is deterministic: the ticked outputs in listed order, then the
microphone last. Each track is named, because the same endpoint can appear as
both a playback and a capture device under one name and two tracks would
otherwise read identically:

```
index=1  handler_name=Game: Analogue 1/2 (2- Audient iD14)
index=2  handler_name=2 - Gigabyte M32U (AMD High Definition Audio Device)
index=3  handler_name=Mic: Analogue 1/2 (2- Audient iD14)
```

The name is written as `handler_name` rather than `title`: MP4 has no per-stream
title the way Matroska does, and the mov muxer puts it in the track handler,
which is what editors read back. Both are set regardless.

Two limitations. Most players and Discord play only the **first** track, so
separate mode is for editing rather than direct sharing. A device that cannot be
opened is logged and skipped rather than failing the recording, so an unplugged
device costs that track, not the session.

`mic_gain_percent` applies a `volume` filter to the microphone before any mixing.

### Mono microphones

A mono capsule on a two-channel endpoint puts the signal on the left channel
only, so the recording plays back in one ear. `mic_channel_mode` handles it:

| Mode | Filter | Use when |
|---|---|---|
| `stereo` | none | the mic really is stereo |
| `mono_left` | `pan=stereo\|c0=c0\|c1=c0` | you only hear yourself on one side |
| `mono_mix` | `pan=stereo\|c0=0.5*c0+0.5*c1\|c1=...` | signal is spread across both |

Measured on a synthetic lopsided input: before, the right channel was `-inf dB`
against `-21.07` on the left; after `mono_left`, both read `-21.07`.

## Where files are saved

`sessions_dir` and `clips_dir` accept any folder, set in the Settings tab with a
native browse dialog. A path inside the app folder is stored **relative**, so a
copied install keeps working; anywhere else is stored **absolute**. Missing
folders are created, and both libraries rescan when the setting changes.
Thumbnail and duration caches live in a `.thumbs` folder beside the videos.

`max_sessions_gb` and `max_clips_gb` cap each folder. Pruning runs after a
recording finishes, never during capture, and deletes oldest first, taking each
file's sidecar and thumbnail with it.

## Auto-record

Auto-record does not detect games. It matches a watchlist of executable names
against the running process list, and starts or stops recording when one appears
or disappears. Nothing is injected, no DLL is loaded into the game, no hooks are
installed and the game's memory is never touched.

The Settings tab lists running processes to pick from, with common Windows
service names filtered out. Anything unrecognised is still listed.

Polling cost matters, since it runs while a game is in the foreground. Measured
on this machine, 323 processes:

| | per poll |
|---|---|
| `CreateToolhelp32Snapshot` (first version) | 6.7 ms |
| `EnumProcesses`, PIDs only | 0.078 ms |

6.7 ms every 1.5 s is small on average but arrives as a single blocking hitch,
and a 60 fps frame is only 16.6 ms. The sweep therefore uses `EnumProcesses`,
which returns bare PIDs, and resolves a name only for a PID it has not seen
before; a PID cache means an established process is never queried twice.

Once a game is found the sweep stops entirely. A handle to that process is held
open and checked with `WaitForSingleObject(h, 0)`, a status read on a handle
already owned, so there is no enumeration and no allocation while recording.
Hunting for new games is skipped during a recording, since a second game could
not be acted on anyway.

## Overlay

A small always-on-top window showing elapsed time, live capture fps, file size,
how many highlights have been marked, and both hotkeys. It defaults to a monitor
you are not recording, ignores mouse input and never takes focus.

The fps figure is ffmpeg's own, via `-progress` to a file that the UI polls, so
it is the real capture rate rather than an estimate. ffmpeg already computes
those numbers for its own stats and writes about 395 bytes per second. The UI
reads only the bytes appended since the previous poll, four times a second, since
ffmpeg refreshes twice a second.

The incremental read is deliberate. Re-reading the whole file every frame costs
more the longer the session runs, because the file grows throughout (about 2.7 MB
over two hours). A full read of a 20-second-old file measured 0.24 ms.

GDI objects for the overlay are created once and reused, and repaints are
throttled to four a second. Creating fonts and brushes per paint at frame rate
churns the desktop-wide GDI heap.

## Highlight markers

**Alt+Shift+F9** during a recording drops a marker at the current position.
Markers are written into the session's sidecar JSON on stop, and the scanner
preserves them when it later fills in duration and geometry. Repeated presses
within one second count as a single marker.

In the preview they appear as amber ticks on the scrub bar, with **Prev** /
**Next** to jump between them and **Clip around marker**, which sets Mark In and
Mark Out to the 15 seconds spanning the nearest one, weighted to before the
marker.

## Timeline editing

The selected range is shaded blue on the scrub bar, with a green handle at Mark
In and a red one at Mark Out. Both can be dragged directly on the bar and cannot
cross each other. Right-click anywhere on the bar moves whichever edge is nearer.
Releasing a handle seeks the playhead to that edge.

The handles are drawn and hit-tested by hand rather than being real widgets: they
sit on top of the slider, and a widget there would swallow the clicks meant for
scrubbing.

**Space** plays and pauses in either preview. It is ignored while a text field
has the keyboard.

## Deleting

**Delete** in the preview, or right-click a row in either list, removes a file
after a confirmation. The video goes along with its sidecar JSON, its cached
thumbnail and any leftover progress file.

The player is told to unload first. mpv keeps the file open while it is loaded
and Windows refuses to delete a file with an open handle, so the removal is
retried briefly while mpv lets go.

The video is hidden while any modal is open. mpv draws into a child window
layered over the ImGui surface, so a dialog centred on the viewport would land
behind the video: invisible, while still dimming the background and blocking
input.

`modal_active` is set explicitly when a dialog is raised and cleared on Delete,
Cancel or dismissal. Deriving it from `ImGui::IsPopupOpen` does not work here,
because that call resolves the name against the current ID stack and returns
false when asked from outside the window that owns the popup.

## Recording state

`recording()` means a session is open, from `start()` until `stop()`.
`process_alive()` means the ffmpeg process is running right now. These are
separate on purpose.

The capture can die underneath the app, so a recording state defined as "is
ffmpeg alive" flips to false at exactly the moment the recovery code needs to
run, making that code unreachable. The session flag drives the polling instead,
so an unexpected exit is caught, logged with ffmpeg's stderr, and the capture
resumes into `<session>_pt2.mp4`. If it cannot be resumed, the session ends and
reports why.

### Why leaving a fullscreen game kills the capture

ffmpeg's own words, from the log:

```
[Parsed_vpp_amf_1] SubmitInput() failed with error 4
[fc#0] Error requesting a frame from the filtergraph
```

Both capture backends are built on **DXGI Desktop Duplication**, and Windows
invalidates a duplication handle whenever the desktop it was acquired against
changes underneath it: a resolution or refresh change, a desktop switch (UAC,
Ctrl-Alt-Del, the lock screen), or an application taking or releasing exclusive
fullscreen. Pressing the Windows key drops the game out of fullscreen, which is
that last case.

Neither `vsrc_amf` nor `ddagrab` re-acquires the duplication after it is lost, so
the surface stops being valid, the filtergraph errors and ffmpeg exits.
Restarting the capture is the only fix available from outside ffmpeg.

Resuming is delayed on purpose. Relaunching immediately fails again, because the
mode change is still in progress; the replacement dies within seconds and one
interruption can shred a session into five files. The delay starts at 1.5 s and
doubles up to 8 s each time a segment dies in under five seconds, resetting once
a segment survives. Verified by killing the encoder three times in quick
succession: 1500 ms, then 3000 ms, three segments, no cascade.

Roughly one to two seconds of footage is lost across the seam, and the segments
are separate files rather than one continuous recording.

## Frame rate

Measured on the reference machine against real Naraka footage, capturing a 4K
display and scaling to 1080p:

| Recorded at | Real unique fps | Worst gap between real frames |
|---|---|---|
| 60 | 58.4 | 50 ms |
| 144 | 2.4 to 21 | 1.7 s |

At 60 the capture is essentially perfect: 97% of frames land exactly 16.7 ms
apart. At 144 it collapses, because the capture, the 4K downscale and the encoder
all compete with the game for the same GPU. The file still claims 144 fps, since
`-fps_mode cfr` pads the shortfall with duplicate frames, which is what reads as
stutter on playback.

The frame-rate slider is capped to the monitor's refresh rate, and anything above
60 carries a warning. Recording above the panel's refresh cannot produce new
frames; it only makes the encoder work harder producing duplicates.

Two related settings come from the same investigation:

- `-max_interleave_delta 0`. With two inputs ffmpeg will hold one stream back to
  keep the interleave tight when the other falls behind. A stalled audio pipe
  must not be able to stall video capture, so packets are written as they arrive.
- The AMF encoder runs as `lowlatency_high_quality` at `quality speed` rather
  than `transcoding`, which assumes an otherwise idle card.

## Capture backends

`ddagrab` produces BGRA D3D11 frames that every GPU-side consumer on this driver
rejects: `h264_amf`, `scale_d3d11` and `vpp_amf` all fail, and Vulkan interop is
not implemented in this ffmpeg build. The frames therefore round-trip through
system memory for the colour conversion, which is expensive at 4K.

AMD ships its own capture source, so on AMD the pipeline is
`vsrc_amf` to `vpp_amf` to `h264_amf` and no frame leaves the GPU. Measured on
the reference machine (RX 9070 XT, i5-13600K, 4K desktop scaled to 1080p60):

| | ddagrab + readback | vsrc_amf zero-copy |
|---|---|---|
| ffmpeg CPU time over 12 s | 15.5 s (~129% of a core) | 3.0 s (~25% of a core) |

That 5x CPU difference is why `auto` prefers the AMF path. Unique-frame counts
favoured it too, but the synthetic measurements varied enough between runs (43 to
58 fps) that only the CPU figure is worth quoting.

NVIDIA and Intel have equivalent on-GPU paths, `ddagrab` to `hwmap` to
`scale_cuda`, and `ddagrab` to `hwmap` to `scale_qsv`. **Neither could be tested;
there is no such hardware here.** They are wired the way ffmpeg's ddagrab
documentation recommends, and `resolve_backend` runs the real filter chain for a
few frames before committing, so a machine where they do not work falls back to
`ddagrab` automatically rather than failing to record. The Settings tab has a
Test button that reports what `auto` resolved to.

`ddagrab` is therefore the guaranteed path everywhere and the measured one only
on AMD. Capturing a 1080p or 1440p monitor avoids most of its readback cost.

Neither backend delivers a full 60 unique frames at 4K, so `-fps_mode cfr` pads
the timeline to a constant rate: timestamps stay honest and the `-ss`/`-t` seek
used by clip export lands where the scrubber said. A 70 s game recording came out
at exactly 60.00 fps with 41 ms of A/V drift.

## How capture works

Video comes from `ddagrab` (DXGI Desktop Duplication) or `vsrc_amf`.

On fullscreen games: Desktop Duplication cannot see genuinely exclusive-fullscreen
applications. In practice this rarely comes up on Windows 10 and 11, because
Fullscreen Optimizations run most "fullscreen" games as borderless windows that
DWM still composites. Only a game that explicitly opts out, or has the
optimisation disabled per-exe in its compatibility settings, comes back black.

Audio is WASAPI loopback, implemented in `src/capture/audio_capture.cpp`. This
ffmpeg build has no `wasapi` demuxer; the device list is dshow, gdigrab, lavfi,
openal and vfwcap only. The capture thread streams PCM into a named pipe that
ffmpeg opens as an input, while ffmpeg's stdin stays reserved for the `q` that
finalizes the MP4.

Loopback delivers nothing while the system is silent, so a wall clock drives how
many frames should exist and any shortfall is padded with silence. Without that
the audio track drifts ahead of the video by however long the machine was quiet.
Measured drift over a 10 s recording: 60 ms.

## Clip sizing

Sizes are **decimal megabytes**, because that is how upload limits are counted:
Discord's 10 MB means 10,000,000 bytes, not 10 MiB. Computing in MiB and calling
the result "9.9 MB" produces 10,380,902 bytes, which is rejected. The budget is
`bitrate_kbps = (target_MB * 8000) / duration_s` minus the audio allocation, and
ffmpeg's `k` suffix is decimal too, so the units line up end to end. The default
target of 9.9 leaves a 100 KB margin.

Encoding is two-pass ABR by default. The first pass finds the difficult frames so
the second can spend the budget there. This matters more than the preset:
single-pass CRF left 3.2 MB of a 9.9 MB allowance unused on real footage.
Two-pass overshot its target by 5 to 7% in every measured configuration, so the
request is placed 7% low and the rate capped; a size check afterwards re-encodes
once if anything still clears the cap.

Resolution and frame rate come off a ladder. Heights walk 1080/900/720/540/360
and take the tallest rung clearing about 0.045 bits per pixel per frame. That
threshold is deliberately low, because downscaling costs detail unconditionally
while a bitrate squeeze only costs quality on busy scenes. Frame rate is held at
60 until keeping it would force the picture below 720p, at which point a sharper
30 fps image is the better trade.

Measured on real game footage, 9.9 MB target, `veryslow`, two-pass:

| Clip length | Result | Bytes | Under 10,000,000 |
|---|---|---|---|
| 4 s | 1080p60 | 8,760,980 | yes |
| 20 s | 720p60 | 8,946,544 | yes |
| 60 s | 540p30 | 9,429,867 | yes |

Encode cost for a 20 s 1080p60 clip, two-pass, on an i5-13600K: `slow` about
14 s, `slower` about 24 s, `veryslow` about 45 s. Roughly linear in clip length.

A minute of 1080p60 inside 10 MB works out to about 1.2 Mbps, roughly a tenth of
what that resolution and frame rate need, so long clips must give up resolution,
frame rate or both. Raising the target to 50 (Nitro Basic) or 500 (Nitro) keeps
1080p60 much further out.

Clips use libx264 rather than the hardware encoder, which is better at spending a
fixed budget.

## Logging

`logs/outriced.log`, rotated to `outriced.1.log` at each start and flushed every
line so entries written immediately before a crash survive. ffmpeg's stderr is
captured into it. An unhandled-exception filter writes a minidump (`crash_*.dmp`)
beside the log. The Settings tab has an **Open logs** button.

## Measuring a recording

`mpdecimate` counts frames that differ from the one before, so a recording can be
checked for real motion rather than trusting the frame rate the container claims:

```bash
tools/ffmpeg/bin/ffmpeg -i sessions/<file>.mp4 -vf "mpdecimate,showinfo" -f null -
```

The gaps between reported `pts_time` values are what matter. A healthy 60 fps
capture sits at 16.7 ms almost throughout.

One caveat: a static screen is indistinguishable from a capture stall by this
method. A loading screen produces identical frames, reported exactly like dropped
ones. A 74-second session measured 51.7 "real" fps with a 3.2 second gap; the
frames turned out to be a loading screen, all seven gaps fell inside the first 11
seconds, and the remaining 63 seconds were flawless. Check what is on screen
during a gap before treating it as a defect.

Verified at 60 fps on real gameplay:

| Session | Real fps | Frames at exactly 16.7 ms |
|---|---|---|
| 13.9 s @ 15.6 Mbps | 59.7 | 100% |
| 27.3 s @ 9.9 Mbps | 59.8 | 100% |

## Tests

Console harnesses, all built alongside the app:

```bash
build/Release/record_test.exe 8                  # record 8s, stop gracefully, report the file
build/Release/clip_test.exe 4.0 8.0 16-14-41     # export a range; 3rd arg picks a session by name
build/Release/mpv_test.exe                       # confirm libmpv loads, plays, seeks and unloads
```

`record_test` prints which backend and encoder were selected, and lists the audio
endpoints it can see, which is the quickest way to check what `auto` resolved to
on a given machine.
