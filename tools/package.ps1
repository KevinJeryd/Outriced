# Builds a self-contained folder (and zip) that a friend can unzip and run.
# Everything is bundled: no ffmpeg install, no runtime downloads, no PATH setup.
param(
    [string]$Config = 'Release',
    [switch]$NoZip
)
$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build\$Config"
$dist  = Join-Path $root 'dist\Outriced'

if (-not (Test-Path (Join-Path $build 'outriced.exe'))) {
    throw "Build first: cmake --build build --config $Config"
}

if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Force $dist | Out-Null
New-Item -ItemType Directory -Force (Join-Path $dist 'tools\ffmpeg\bin') | Out-Null

# ---- application ----
foreach ($f in @('outriced.exe', 'SDL3.dll', 'libmpv-2.dll')) {
    $src = Join-Path $build $f
    if (-not (Test-Path $src)) { throw "missing $f in $build" }
    Copy-Item $src $dist
}

# ---- ffmpeg ----
# ffplay is not shipped; nothing in the app calls it.
foreach ($f in @('ffmpeg.exe', 'ffprobe.exe')) {
    $src = Join-Path $root "tools\ffmpeg\bin\$f"
    if (-not (Test-Path $src)) { throw "missing $f -- is tools/ffmpeg populated?" }
    Copy-Item $src (Join-Path $dist 'tools\ffmpeg\bin')
}

# The app creates these on first run, but shipping them makes the layout obvious.
New-Item -ItemType Directory -Force (Join-Path $dist 'sessions') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $dist 'clips')    | Out-Null

@"
Outriced
==============

Unzip anywhere and run outriced.exe. Nothing else to install.

The app lives in the system tray. Alt+Shift+F8 starts and stops a recording,
or use the tray menu. When a recording stops the window opens with the new
session at the top: double-click it, scrub to the moment, set Mark In and
Mark Out, then Save clip. Clips land in the clips\ folder, sized to drop
straight into Discord.

On first run it detects your GPU and picks an encoder and capture method by
itself. If recording fails, open Settings and use the Test buttons next to
Encoder and Capture method to see what your machine supports.

Notes
-----
* Recordings go in sessions\ and are large (roughly 90 MB per minute at the
  default bitrate). Delete them when you are done cutting clips.
* Clips target 9.9 MB so they fit Discord's 10 MB free limit. If you have
  Nitro, raise the target in Settings to 50 or 500.
* Two-pass encoding is on by default for the best quality per megabyte. A
  20 second clip takes roughly 45 seconds to export. Lower the preset in
  Settings if you want it faster.
* Games must run in borderless or windowed mode if capture comes up black.
  Most modern games are composited by Windows even when set to fullscreen,
  so this is rarer than it used to be.
"@ | Set-Content (Join-Path $dist 'READ ME FIRST.txt') -Encoding utf8

$bytes = (Get-ChildItem $dist -Recurse -File | Measure-Object -Property Length -Sum).Sum
"Staged $([math]::Round($bytes/1MB,1)) MB in $dist"

if (-not $NoZip) {
    $zip = Join-Path $root 'dist\Outriced.zip'
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path $dist -DestinationPath $zip -CompressionLevel Optimal
    "Zipped $([math]::Round((Get-Item $zip).Length/1MB,1)) MB -> $zip"
}
