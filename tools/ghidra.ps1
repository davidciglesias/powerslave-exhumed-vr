# Run a PowerslaveVR Ghidra script headless against the analyzed project (read-only, no save).
# usage: ghidra.ps1 DumpRefs out.txt needle1 needle2 ...
param([Parameter(Mandatory)][string]$Script, [Parameter(ValueFromRemainingArguments)][string[]]$ScriptArgs)
$root = Split-Path $PSScriptRoot -Parent
$env:JAVA_HOME = "C:\Program Files\Eclipse Adoptium\jdk-25.0.5.8-hotspot"
$env:PATH = "$env:JAVA_HOME\bin;$env:PATH"
$env:GHIDRA_HEADLESS_MAXMEM = "12G"
& "$root\tools\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat" "$root\re\ghidra" anubis -process anubis_Shipping_Steam_x64.exe -noanalysis -readOnly -scriptPath "$root\re\scripts" -postScript "$Script.java" @ScriptArgs *> "$root\re\ghidra_last_run.log"
Write-Host "exit $LASTEXITCODE"
