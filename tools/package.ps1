# Collects the built DLL and the INI into a Vortex-ready zip:
#   FastTravelTimeInfo-mod.zip
#     SKSE\Plugins\FastTravelTimeInfo.dll
#     SKSE\Plugins\FastTravelTimeInfo.ini
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root 'build'

$dll = Get-ChildItem -Path $buildDir -Recurse -Filter 'FastTravelTimeInfo.dll' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $dll) {
    Write-Host 'FastTravelTimeInfo.dll was not found in the build folder.'
    exit 1
}

$stage = Join-Path $root 'mod'
if (Test-Path $stage) {
    Remove-Item $stage -Recurse -Force
}
$plugins = Join-Path (Join-Path $stage 'SKSE') 'Plugins'
New-Item -ItemType Directory -Force -Path $plugins | Out-Null
Copy-Item $dll.FullName $plugins
Copy-Item (Join-Path (Join-Path (Join-Path (Join-Path $root 'dist') 'SKSE') 'Plugins') 'FastTravelTimeInfo.ini') $plugins

$zip = Join-Path $root 'FastTravelTimeInfo-mod.zip'
if (Test-Path $zip) {
    Remove-Item $zip -Force
}
Compress-Archive -Path (Join-Path $stage 'SKSE') -DestinationPath $zip

Write-Host "Created $zip"
exit 0
