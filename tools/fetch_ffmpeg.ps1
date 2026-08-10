# Fetches the ffmpeg build Outriced records with into tools/ffmpeg.
#
# The binaries are not in the repository: they are ~290 MB and are not ours to
# version. This grabs the same BtbN win64-gpl build the project is developed
# against, which is the one that carries everything the capture pipeline needs:
#   ddagrab + vsrc_amf/vpp_amf  capture and on-GPU scaling
#   h264_amf / h264_nvenc / h264_qsv  hardware encoding
#   libx264                     clip export
# A stock ffmpeg from a package manager is usually missing at least one of these,
# which is why the app ignores whatever is on PATH and uses this copy.
param(
    [switch]$Force   # re-download even if tools/ffmpeg already exists
)
$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'tools\ffmpeg'

if ((Test-Path (Join-Path $dest 'bin\ffmpeg.exe')) -and -not $Force) {
    Write-Host "ffmpeg already present in $dest (use -Force to replace)"
    exit 0
}

$tmp = Join-Path $env:TEMP ('ffmpeg_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $tmp | Out-Null

$url     = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip'
$archive = Join-Path $tmp 'ffmpeg.zip'

Write-Host 'Downloading ffmpeg (about 170 MB)...'
Invoke-WebRequest -Uri $url -OutFile $archive

Write-Host 'Extracting...'
Expand-Archive -Path $archive -DestinationPath $tmp -Force

# The archive contains a single versioned top-level folder.
$inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
if (-not $inner) { throw 'unexpected archive layout: no top-level folder' }

if (Test-Path $dest) {
    # Replacing the folder fails if anything is still using the binaries, and the
    # raw access-denied that Windows raises does not say why. Name the culprits.
    $busy = Get-Process ffmpeg, ffprobe, ffplay -ErrorAction SilentlyContinue
    if ($busy) {
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
        throw ("Cannot replace $dest while these are running: " +
               (($busy | Select-Object -ExpandProperty Name -Unique) -join ', ') +
               '. Stop any recording (and close Outriced) and run this again.')
    }
    Remove-Item $dest -Recurse -Force
}
Move-Item $inner.FullName $dest -Force
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue

foreach ($exe in @('ffmpeg.exe', 'ffprobe.exe')) {
    if (-not (Test-Path (Join-Path $dest "bin\$exe"))) { throw "missing $exe after extraction" }
}

# Fail loudly here rather than at the first recording if the build is wrong.
$ff = Join-Path $dest 'bin\ffmpeg.exe'
# Joined into single strings first: -match against an array returns the matching
# elements rather than a boolean, so testing an array always looks true and every
# check below would silently pass -- or, negated, silently fail.
$filters  = (& $ff -hide_banner -filters)  -join "`n"
$encoders = (& $ff -hide_banner -encoders) -join "`n"

$missing = @()
if ($filters  -notmatch 'ddagrab') { $missing += 'ddagrab (screen capture)' }
if ($encoders -notmatch 'libx264') { $missing += 'libx264 (clip export)' }
if (($encoders -notmatch 'h264_amf') -and
    ($encoders -notmatch 'h264_nvenc') -and
    ($encoders -notmatch 'h264_qsv')) { $missing += 'any hardware H.264 encoder' }

if ($missing.Count -gt 0) {
    Write-Warning "This ffmpeg build is missing: $($missing -join ', ')"
} else {
    Write-Host 'Verified: ddagrab, libx264 and a hardware encoder are all present.'
}

& $ff -hide_banner -version | Select-Object -First 1
Write-Host "ffmpeg ready in $dest"
