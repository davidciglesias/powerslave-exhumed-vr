param([string]$Config = "RelWithDebInfo", [switch]$Phase2)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$game = "F:\Steam\steamapps\common\PowerSlave Exhumed"
$py = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
$build = Join-Path $root "build"
New-Item -ItemType Directory -Force $build | Out-Null

# the real SDL2 is either still SDL2.dll (not deployed yet) or SDL2_real.dll
$realSdl = if (Test-Path "$game\SDL2_real.dll") { "$game\SDL2_real.dll" } else { "$game\SDL2.dll" }
$defArgs = @("$root\tools\gen_def.py", $realSdl, "$build\sdl2_forward.cpp")
if ($Phase2) { $defArgs += "phase2" }
& $py -I @defArgs
if ($LASTEXITCODE) { throw "gen_def failed" }

$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$cmd = "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && `"$cmake`" -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=$Config >nul && `"$cmake`" --build `"$build`""
cmd /c $cmd
if ($LASTEXITCODE) { throw "build failed" }
Write-Host "built $build\SDL2.dll"
