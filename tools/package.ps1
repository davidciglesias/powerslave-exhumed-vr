<#
Build and zip a release: dist\PowerslaveExhumedVR.zip, plus the install scripts as separate release assets.
  package.ps1 [-NoBuild]
The zip holds SDL2.dll, openxr_loader.dll, PowerslaveVR.ini, README.md, LICENSE and THIRD_PARTY_NOTICES.md. The install
scripts download it from .../releases/latest/download/PowerslaveExhumedVR.zip.
#>
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot
$version = ([regex]::Match((Get-Content (Join-Path $Root 'src\common\version.h') -Raw), '"([^"]+)"')).Groups[1].Value

if (-not $NoBuild) {
    $env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
    & (Join-Path $PSScriptRoot 'build.ps1')
    & (Join-Path $Root 'build\pvr_tests.exe') | Select-Object -Last 1
    if ($LASTEXITCODE) { throw 'unit tests failed' }
}

$dist = Join-Path $Root 'dist'
$stage = Join-Path $dist 'stage'
Get-ChildItem $dist -Exclude 'RELEASE_NOTES_*' -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
New-Item -ItemType Directory $stage -Force | Out-Null
Copy-Item (Join-Path $Root 'build\SDL2.dll') $stage
Copy-Item (Join-Path $Root 'third_party\openxr\x64\bin\openxr_loader.dll') $stage
# GetPrivateProfileString is happiest with CRLF
$ini = (Get-Content (Join-Path $Root 'PowerslaveVR.ini')) -join "`r`n"
[IO.File]::WriteAllText((Join-Path $stage 'PowerslaveVR.ini'), $ini + "`r`n")
foreach ($f in 'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md') { Copy-Item (Join-Path $Root $f) $stage }

$zip = Join-Path $dist 'PowerslaveExhumedVR.zip'
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item $stage -Recurse -Force
foreach ($f in 'frame_install.sh', 'frame_uninstall.sh', 'pc_install.ps1', 'pc_uninstall.ps1') { Copy-Item (Join-Path $PSScriptRoot $f) $dist }
# the shell scripts must keep LF line endings
foreach ($sh in (Join-Path $dist 'frame_install.sh'), (Join-Path $dist 'frame_uninstall.sh')) {
    [IO.File]::WriteAllText($sh, ([IO.File]::ReadAllText($sh) -replace "`r`n", "`n"))
}

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"version: $version"
"zip:     $zip"
"sha256:  $hash"
# stamp the checksum into this version's release notes, if they exist
$notes = Join-Path $dist "RELEASE_NOTES_v$version.md"
if (Test-Path $notes) {
    $t = [IO.File]::ReadAllText($notes) -replace '(SHA-256 \(`PowerslaveExhumedVR.zip`\): `)[^`]*(`)', "`${1}$hash`$2"
    [IO.File]::WriteAllText($notes, $t)
}
