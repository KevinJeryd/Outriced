# Fetches the prebuilt shinchiro libmpv package into third_party/mpv and builds
# an MSVC import library from the shipped .def file (the bundled libmpv.dll.a is
# a MinGW import lib and is not usable from MSVC).
$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

$root  = Split-Path -Parent $PSScriptRoot
$dest  = Join-Path $root 'third_party\mpv'
$tmp   = Join-Path $env:TEMP ('mpvdev_' + [guid]::NewGuid().ToString('N'))

# The package is a .7z, which Expand-Archive cannot read. Look for 7-Zip on PATH
# first so this works on a CI runner as well as a desktop install.
$sevenZip = (Get-Command 7z.exe -ErrorAction SilentlyContinue).Source
if (-not $sevenZip) {
    foreach ($candidate in @("$env:ProgramFiles\7-Zip\7z.exe",
                             "${env:ProgramFiles(x86)}\7-Zip\7z.exe",
                             "$env:ProgramW6432\7-Zip\7z.exe")) {
        if (Test-Path $candidate) { $sevenZip = $candidate; break }
    }
}
if (-not $sevenZip) {
    throw '7-Zip not found. Install it, or add 7z.exe to PATH.'
}

Write-Host 'Resolving latest mpv-dev release...'
$rel   = Invoke-RestMethod 'https://api.github.com/repos/shinchiro/mpv-winbuild-cmake/releases/latest'
$asset = $rel.assets | Where-Object { $_.name -match '^mpv-dev-x86_64-\d' } | Select-Object -First 1
if (-not $asset) { throw 'No mpv-dev-x86_64 asset in the latest release' }

New-Item -ItemType Directory -Force $tmp | Out-Null
$archive = Join-Path $tmp $asset.name
Write-Host "Downloading $($asset.name) ..."
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $archive

Write-Host 'Extracting...'
& $sevenZip x $archive "-o$tmp" -y | Out-Null

New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item (Join-Path $tmp 'include') $dest -Recurse -Force
Get-ChildItem $tmp -Filter '*.dll' -Recurse | ForEach-Object { Copy-Item $_.FullName $dest -Force }

# The package ships only a MinGW import lib (libmpv.dll.a), so derive an MSVC
# one: read the DLL's export table with dumpbin, write a .def, feed it to lib.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath  = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio C++ tools not found' }

$msvcDir = Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC') | Sort-Object Name -Descending | Select-Object -First 1
$binDir  = Join-Path $msvcDir.FullName 'bin\Hostx64\x64'
$libExe  = Join-Path $binDir 'lib.exe'
$dumpbin = Join-Path $binDir 'dumpbin.exe'
if (-not (Test-Path $libExe))  { throw "lib.exe not found at $libExe" }
if (-not (Test-Path $dumpbin)) { throw "dumpbin.exe not found at $dumpbin" }

# dumpbin needs its sibling DLLs on PATH.
$env:PATH = "$binDir;$env:PATH"

$dllName = 'libmpv-2.dll'
$exports = & $dumpbin /exports (Join-Path $dest $dllName)

# Export rows look like: "    1    0 0001C1B0 mpv_abort_async_command"
$names = $exports |
    Select-String -Pattern '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)' |
    ForEach-Object { $_.Matches.Groups[1].Value }

if ($names.Count -eq 0) { throw 'Could not read any exports from ' + $dllName }
Write-Host "Found $($names.Count) exported symbols"

$def = Join-Path $dest 'mpv.def'
$sb  = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("LIBRARY $dllName")
[void]$sb.AppendLine('EXPORTS')
foreach ($n in $names) { [void]$sb.AppendLine("    $n") }
Set-Content -Path $def -Value $sb.ToString() -Encoding ascii

Push-Location $dest
try {
    & $libExe "/def:mpv.def" "/out:mpv.lib" "/machine:x64" | Out-Null
    if (-not (Test-Path 'mpv.lib')) { throw 'lib.exe did not produce mpv.lib' }
} finally {
    Pop-Location
}

Remove-Item $tmp -Recurse -Force
Write-Host "libmpv ready in $dest"
Get-ChildItem $dest | Select-Object Name, Length
