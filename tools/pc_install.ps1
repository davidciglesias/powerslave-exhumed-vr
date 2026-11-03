<#
Install PowerSlave Exhumed VR for playing PowerSlave Exhumed in PC VR (SteamVR, e.g. a Steam Frame streamed from the PC).
  pc_install.ps1 [-GameFolder <path>] [-Uninstall]
Run it with the game installed and not running. It downloads this mod's release zip from GitHub and puts it in the game
folder: the game's own SDL2.dll is renamed to SDL2_real.dll and the mod's SDL2.dll (which loads it) takes its place,
next to openxr_loader.dll and PowerslaveVR.ini. It also sets the game's controller bindings and video settings for VR in
kexengine.cfg (your previous file is kept as kexengine.cfg.before-vr). Re-running it updates the mod and keeps your
PowerslaveVR.ini.

-Uninstall  put the game's SDL2.dll back, remove the mod's files and restore kexengine.cfg.before-vr.
Set PVR_ZIP to a local PowerslaveExhumedVR.zip (or a URL) to install that instead of the latest release.
#>
param(
    [string]$GameFolder,
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # PowerShell 5.1 downloads crawl with the progress bar on
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$Repo = 'davidciglesias/powerslave-exhumed-vr'
$ModUrl = if ($env:PVR_ZIP) { $env:PVR_ZIP } else { "https://github.com/$Repo/releases/latest/download/PowerslaveExhumedVR.zip" }
$Exe = 'anubis_Shipping_Steam_x64.exe'
$ExpectedBuild = 0x689a8448   # PE timestamp of the game build the mod was made for (Steam build 19569414)

function Fail($msg) { Write-Host $msg -ForegroundColor Red; exit 1 }

# Find the game in any Steam library.
if (-not $GameFolder) {
    $steam = $null
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        $p = Get-ItemProperty $key -ErrorAction SilentlyContinue
        if ($p -and $p.SteamPath) { $steam = $p.SteamPath; break }
        if ($p -and $p.InstallPath) { $steam = $p.InstallPath; break }
    }
    $libs = @()
    if ($steam) {
        $libs += $steam
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s*"([^"]*)"')) { $libs += $m.Groups[1].Value -replace '\\\\', '\' }
        }
    }
    foreach ($lib in $libs) {
        $g = Join-Path $lib 'steamapps\common\PowerSlave Exhumed'
        if (Test-Path (Join-Path $g $Exe)) { $GameFolder = $g; break }
    }
}
if (-not $GameFolder -or -not (Test-Path (Join-Path $GameFolder $Exe))) {
    Fail 'PowerSlave Exhumed not found. Install it in Steam first, or pass its folder: -GameFolder "D:\path\to\PowerSlave Exhumed"'
}
$GameFolder = (Resolve-Path $GameFolder).Path
if (Get-Process -Name 'anubis_Shipping_Steam_x64' -ErrorAction SilentlyContinue) { Fail 'The game is running; quit it first.' }
Write-Host "Game folder: $GameFolder"

# The game's settings file: <Saved Games>\Nightdive Studios\Powerslave EX\kexengine.cfg
$saved = $null
$p = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders' -ErrorAction SilentlyContinue
if ($p) { $v = $p.'{4C5C32FF-BB9D-43b0-B5B4-2D72E54EAAA4}'; if ($v) { $saved = [Environment]::ExpandEnvironmentVariables($v) } }
if (-not $saved) { $saved = Join-Path $env:USERPROFILE 'Saved Games' }
$CfgDir = if ($env:PVR_CFG_DIR) { $env:PVR_CFG_DIR } else { Join-Path $saved 'Nightdive Studios\Powerslave EX' }   # PVR_CFG_DIR: for testing
$Cfg = Join-Path $CfgDir 'kexengine.cfg'
$CfgBackup = "$Cfg.before-vr"

$Sdl = Join-Path $GameFolder 'SDL2.dll'
$SdlReal = Join-Path $GameFolder 'SDL2_real.dll'
# The mod's SDL2.dll forwards every export to SDL2_real.dll, so that name is inside it; the game's own one doesn't have it.
function Test-OurSdl($path) {
    if (-not (Test-Path $path)) { return $false }
    $text = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($path))
    return $text.Contains('SDL2_real')
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
function Read-ZipText($zip, $name) {
    $z = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $e = $z.Entries | Where-Object { $_.FullName -eq $name } | Select-Object -First 1
        if (-not $e) { return $null }
        $r = New-Object IO.StreamReader($e.Open())
        try { return $r.ReadToEnd() } finally { $r.Dispose() }
    } finally { $z.Dispose() }
}

# Controller bindings for the mod's virtual pad (joy_N = SDL button N, joy_15/16 = left/right trigger).
$VrBinds = @(
    'bind joy_0 "jump"', 'bind joy_1 "+use"', 'bind joy_2 "*weaponwheel"', 'bind joy_3 "inventoryscreen"',
    'bind joy_4 "automap"', 'bind joy_9 "+weapprev"', 'bind joy_10 "+weapnext"', 'bind joy_11 "mapzoomin"',
    'bind joy_12 "mapzoomout"', 'bind joy_15 "+use"', 'bind joy_16 "attack"'
)
# D3D11 renderer (the mod hooks it), windowed, square 2048x2048 (= the resolution each eye is rendered at).
$VrCvars = [ordered]@{ r_rhirenderfamily = 'rhi.d3d11'; v_windowmode = '0'; v_width = '2048'; v_height = '2048' }

# kexengine.cfg without its controller binds and the cvars above, plus ours. If the game has no kexengine.cfg yet, it is
# made from the game's default.cfg (the game skips default.cfg once kexengine.cfg exists, so it must hold every bind).
function Set-VrConfig {
    New-Item -ItemType Directory $CfgDir -Force | Out-Null
    if (Test-Path $Cfg) {
        if (-not (Test-Path $CfgBackup)) { Copy-Item $Cfg $CfgBackup }
        $lines = @(Get-Content $Cfg)
    } else {
        $def = Read-ZipText (Join-Path $GameFolder 'PowerslaveEX.kpf') 'default.cfg'
        if (-not $def) { throw "default.cfg not found in the game's PowerslaveEX.kpf" }
        $lines = $def -split "`r?`n"
        if (-not (Test-Path $CfgBackup)) { [IO.File]::WriteAllText($CfgBackup, '') }   # empty backup = there was no kexengine.cfg
    }
    $names = ($VrCvars.Keys | ForEach-Object { [regex]::Escape($_) }) -join '|'
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($l in $lines) {
        if ($l -match '^\s*bind\s+joy_\d+\s' -or $l -match "^\s*seta?\s+($names)\s" -or $l.Trim() -eq '') { continue }
        $out.Add($l)
    }
    foreach ($b in $VrBinds) { $out.Add($b) }
    foreach ($k in $VrCvars.Keys) { $out.Add("seta $k `"$($VrCvars[$k])`"") }
    [IO.File]::WriteAllText($Cfg, (($out -join "`n") + "`n"))
    Write-Host "Game settings set for VR: $Cfg"
}

if ($Uninstall) {
    if (Test-Path $SdlReal) {
        Move-Item $SdlReal $Sdl -Force
    } elseif (Test-OurSdl $Sdl) {
        Write-Host "SDL2_real.dll is missing: run Verify integrity of game files in Steam to get the game's SDL2.dll back." -ForegroundColor Yellow
    }
    foreach ($f in 'openxr_loader.dll', 'PowerslaveVR.ini', 'PowerslaveVR.log', 'PowerslaveVR_hand.raw') {
        Remove-Item (Join-Path $GameFolder $f) -Force -ErrorAction SilentlyContinue
    }
    if ((Test-Path $CfgBackup) -and (Get-Item $CfgBackup).Length -gt 0) {
        Move-Item $CfgBackup $Cfg -Force
        Write-Host "Restored your previous game settings ($Cfg)."
    } elseif (Test-Path $CfgBackup) {
        # the installer made kexengine.cfg from default.cfg; without it the game uses default.cfg again
        Remove-Item $Cfg, $CfgBackup -Force -ErrorAction SilentlyContinue
        Write-Host 'Removed the VR game settings (the game uses its defaults again).'
    }
    Write-Host 'Removed PowerSlave Exhumed VR.'
    exit 0
}

# Warn when the game build differs: the mod then starts without VR rendering rather than crashing.
$bytes = [IO.File]::ReadAllBytes((Join-Path $GameFolder $Exe))
$stamp = [BitConverter]::ToUInt32($bytes, [BitConverter]::ToInt32($bytes, 0x3c) + 8)
if ($stamp -ne $ExpectedBuild) {
    Write-Host ("This game build (0x{0:x8}) is not the one the mod was made for (0x{1:x8}): the mod will only show a flat screen until it is updated." -f $stamp, $ExpectedBuild) -ForegroundColor Yellow
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ('pvr-install-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $tmp | Out-Null
try {
    $zip = Join-Path $tmp 'mod.zip'
    Write-Host 'Downloading PowerSlave Exhumed VR...'
    if ($ModUrl -match '^https?://') { Invoke-WebRequest -Uri $ModUrl -OutFile $zip -UseBasicParsing } else { Copy-Item $ModUrl $zip }

    # SDL2.dll that isn't ours is the game's (first install, or Steam restored it after an update/verify).
    if (-not (Test-OurSdl $Sdl)) { Move-Item $Sdl $SdlReal -Force }

    $z = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        foreach ($name in 'SDL2.dll', 'openxr_loader.dll', 'PowerslaveVR.ini') {
            $e = $z.Entries | Where-Object { $_.Name -eq $name } | Select-Object -First 1
            if (-not $e) { throw "$name is not in the downloaded zip." }
            $dest = Join-Path $GameFolder $name
            if ($name -eq 'PowerslaveVR.ini' -and (Test-Path $dest)) { continue }   # keep the user's settings
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dest, $true)
        }
    } finally { $z.Dispose() }
    Set-VrConfig
} finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host 'Done. Start SteamVR (connect the headset), then launch PowerSlave Exhumed from Steam (no launch options needed).' -ForegroundColor Green
