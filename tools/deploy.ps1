param([switch]$Restore)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$game = "F:\Steam\steamapps\common\PowerSlave Exhumed"
$backup = Join-Path $root "backup"
New-Item -ItemType Directory -Force $backup | Out-Null

if ($Restore) {
    if (Test-Path "$game\SDL2_real.dll") {
        Copy-Item "$game\SDL2_real.dll" "$game\SDL2.dll" -Force
        Remove-Item "$game\SDL2_real.dll", "$game\openxr_loader.dll" -Force -ErrorAction SilentlyContinue
    }
    Write-Host "restored original SDL2.dll"
    return
}

if (-not (Test-Path "$game\SDL2_real.dll")) {
    # first deploy: keep a pristine copy outside the game dir as well
    Copy-Item "$game\SDL2.dll" "$backup\SDL2.dll.orig" -Force
    Move-Item "$game\SDL2.dll" "$game\SDL2_real.dll"
}
Copy-Item "$root\build\SDL2.dll" "$game\SDL2.dll" -Force
if (Test-Path "$root\build\SDL2.pdb") { Copy-Item "$root\build\SDL2.pdb" "$game\SDL2.pdb" -Force }
Copy-Item "$root\third_party\openxr\x64\bin\openxr_loader.dll" "$game\openxr_loader.dll" -Force
if (-not (Test-Path "$game\PowerslaveVR.ini")) { Copy-Item "$root\PowerslaveVR.ini" "$game\PowerslaveVR.ini" }
Write-Host "deployed to $game"
