# Third-party notices

Outriced is distributed under the GNU General Public License v3.0 (see
`LICENSE`). That choice is not arbitrary: the release archive bundles ffmpeg and
libmpv binaries that are themselves GPL-licensed, and a combined work containing
them has to be distributed under compatible terms.

Nothing here is required to *use* the application. It matters if you redistribute
the packaged zip, or ship a modified build.

## Bundled in the release archive

### FFmpeg — `tools/ffmpeg/`

Prebuilt Windows binaries from [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds),
the `win64-gpl` variant, fetched by `tools/fetch_ffmpeg.ps1`.

Configured with `--enable-gpl --enable-version3`, so this build is **GPL v3**.
It includes GPL components, notably `libx264`, which Outriced uses for clip
export. FFmpeg's own licence text ships alongside the binaries in
`tools/ffmpeg/LICENSE.txt`.

- Upstream: https://ffmpeg.org
- Source: https://github.com/FFmpeg/FFmpeg
- Licence: GPL v3 (this build); FFmpeg is LGPL v2.1+ when built without `--enable-gpl`

### libmpv — `third_party/mpv/`

Prebuilt Windows binaries from [shinchiro/mpv-winbuild-cmake](https://github.com/shinchiro/mpv-winbuild-cmake),
fetched by `tools/fetch_mpv.ps1`. Used for playback and scrubbing in the preview.

- Upstream: https://mpv.io
- Source: https://github.com/mpv-player/mpv
- Licence: **GPL v2 or later** (mpv can be built LGPL v2.1+; these builds are not)

### SDL3

Fetched from source by CMake. Window, input and rendering.

- Source: https://github.com/libsdl-org/SDL
- Licence: **zlib** — permissive, attribution only

### Dear ImGui

Fetched from source by CMake. The user interface.

- Source: https://github.com/ocornut/imgui
- Licence: **MIT**

### nlohmann/json

Fetched from source by CMake. Settings and session sidecar files.

- Source: https://github.com/nlohmann/json
- Licence: **MIT**

## Why the whole thing is GPL v3

SDL3, Dear ImGui and nlohmann/json are permissive and impose no conditions
beyond attribution. FFmpeg and mpv are the binding constraint: linking against
libmpv and shipping a GPL FFmpeg build makes the distributed archive a combined
work under the GPL. GPL v3 is used rather than v2 because the FFmpeg build is
`--enable-version3`.

If you would rather license your own changes permissively, both dependencies
offer LGPL builds — but the LGPL FFmpeg build drops `libx264`, which clip export
currently depends on, so that swap is not free.

## Not bundled

`ffplay.exe` is deliberately excluded from the release archive. Nothing in
Outriced invokes it, and leaving it out keeps the download smaller.
