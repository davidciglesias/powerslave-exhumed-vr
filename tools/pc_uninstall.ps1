<#
Uninstall PowerSlave Exhumed VR from PC: puts the game's SDL2.dll back, removes the mod's files from the game folder and
restores your previous kexengine.cfg (kexengine.cfg.before-vr).
  pc_uninstall.ps1 [-GameFolder <path>]
Runs pc_install.ps1 -Uninstall: the copy next to this script if there is one, else the one from the latest release.
#>
param([string]$GameFolder)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$local = if ($PSScriptRoot) { Join-Path $PSScriptRoot 'pc_install.ps1' } else { $null }
$code = if ($local -and (Test-Path $local)) { Get-Content $local -Raw } else {
    Invoke-RestMethod 'https://github.com/davidciglesias/powerslave-exhumed-vr/releases/latest/download/pc_install.ps1'
}
$splat = @{ Uninstall = $true }
if ($GameFolder) { $splat.GameFolder = $GameFolder }
& ([scriptblock]::Create($code)) @splat
