param([string]$Build = 'build/fsr4-xess-preview', [string]$Output)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$revision = (& git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot read source revision' }
if (!$Output) { $Output = Join-Path $repo ('build/upscaler-beta-' + $revision.Substring(0, 8)) }
$target = [IO.Path]::GetFullPath($Output)
if ((Test-Path -LiteralPath $target) -or (Test-Path -LiteralPath "$target.zip")) { throw 'Choose a new output path' }
$source = [IO.Path]::GetFullPath($Build)
$files = @('SuperOdysseyRecomp.exe', 'amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll',
           'AMD-FSR-SDK-LICENSE.md', 'libxess.dll', 'Intel-XeSS-LICENSE.txt', 'Intel-XeSS-THIRD-PARTY.txt')
foreach ($file in $files) {
    if (!(Test-Path -LiteralPath (Join-Path $source $file) -PathType Leaf)) { throw "Missing staged file: $file" }
}
New-Item -ItemType Directory -Path $target | Out-Null
# Explicit allowlist: no game library, assets, keys, saves or local settings.
foreach ($file in $files) { Copy-Item -LiteralPath (Join-Path $source $file) -Destination $target }
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination (Join-Path $target 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $repo 'docs/EXPERIMENTAL-UPSCALERS.md') -Destination (Join-Path $target 'TESTING.md')
@'
Experimental FSR 4 / XeSS runtime overlay - Windows x64

This is NOT a standalone installer. Make a separate copy of your current working
Super Odyssey PC Recomp installation AND its game-data folder first. Extract
this overlay beside SuperOdysseyRecomp.exe in that copy, replacing that copied
executable only. Keep the existing dependency DLLs (SDL3, shaderc, FFmpeg and
compiler runtimes). This build imports avcodec-63, avformat-63, avutil-61 and
swscale-10; an older install with other major versions needs matching dependencies.
It does not contain game files, keys, saves or NVIDIA's DLSS runtime.

Launch with a COPIED game-data folder, for example from a command prompt:
  "Test Experimental Upscalers.cmd" "D:\Odyssey Beta Data"
Or drag that copied data folder onto the CMD file. Do not point it at your only
production copy: renderer caches and extracted metadata can be written there.
This launcher uses separate settings and a new beta-save directory next to the
executable. To test an existing save, manually copy your own save into that beta
save directory first. The launcher does not copy or modify production saves.

Graphics > Temporal upscaler selects the backend. Start with Quality, internal
resolution Match display. XeSS is the initial selection. FSR 4's status MUST say
"FSR 4 ML" to count as an actual ML test; "FSR 3.1 fallback" is not FSR 4 output.
No frame generation is included. Read TESTING.md for the test checklist and
known temporal-quality limits. Intel's DLL may need the VC++ x64 redistributable.

Report GPU/driver/CPU/Windows, settings and SuperOdysseyRecomp.log next to the
executable. Review logs for personal paths before sharing. Never share game
assets, keys or saves publicly. SOURCE.txt identifies the local source commit;
SHA256SUMS.txt lists package file hashes. Nothing was published by this script.
'@ | Set-Content -LiteralPath (Join-Path $target 'README.txt') -Encoding utf8
@'
@echo off
setlocal
if "%~1"=="" (
  echo Drag a COPIED game-data folder onto this file, or pass its path as the first argument.
  pause
  exit /b 1
)
if not exist "%~f1\extracted\" (
  echo The copied data folder must contain the extracted directory from a working installation.
  pause
  exit /b 1
)
set "ODYSSEY_DATA_ROOT=%~f1"
set "ODYSSEY_SETTINGS=%~dp0experimental-upscalers.ini"
set "ODYSSEY_SAVE_DIR=%~dp0experimental-upscalers-save"
set "ODYSSEY_NO_UPDATE_CHECK=1"
set "ODYSSEY_FOG_CPU_READBACK=1"
set "ODYSSEY_LINEAR_WRITEBACK=0"
cd /d "%~dp0"
start "" "%~dp0SuperOdysseyRecomp.exe"
endlocal
'@ | Set-Content -LiteralPath (Join-Path $target 'Test Experimental Upscalers.cmd') -Encoding ascii
@'
window_mode = windowed
display_resolution = 1080p
temporal_backend = xess
xess_mode = quality
fsr4_mode = quality
resolution = match
frame_rate = 60
skip_intro = 1
check_updates = 0
'@ | Set-Content -LiteralPath (Join-Path $target 'experimental-upscalers.ini') -Encoding ascii
"Source commit: $revision`r`nRepository: https://github.com/Simon3Sarkis/super-odyssey-pc-recomp`r`nLocal preview commit; not necessarily pushed or released." |
    Set-Content -LiteralPath (Join-Path $target 'SOURCE.txt') -Encoding ascii
Get-ChildItem -LiteralPath $target -File | Sort-Object Name | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name
} | Set-Content -LiteralPath (Join-Path $target 'SHA256SUMS.txt') -Encoding ascii
Compress-Archive -Path (Join-Path $target '*') -DestinationPath "$target.zip" -CompressionLevel Optimal
Write-Output "Created $target.zip"
