# launch_recomp.ps1 - Launch ps2EntryRunner (recomp backend) with the PC watchdog
# printing in THIS terminal, then bring up RecompDebugger.exe to attach.
#   .\launch_recomp.ps1                 (defaults below)
#   .\launch_recomp.ps1 -NoDebugger     (runner + watchdog only)
param(
    [string]$Elf = "F:\SDBZ Recomp\ELF\SLUS_214.42",
    [string]$Exe = "F:\SDBZ Recomp\build\ps2xRuntime\Debug\ps2EntryRunner.exe",
    [string]$Dbg = "F:\SDBZ Recomp\build\ps2xRuntime\Debug\RecompDebugger.exe",
    [string]$Log = "F:\SDBZ Recomp\run_log.txt",
    [switch]$NoDebugger
)

foreach ($p in @($Exe, $Elf)) { if (-not (Test-Path $p)) { Write-Error "Not found: $p"; exit 1 } }

# Debugger in its own window first, so it's ready to attach the moment the shm appears.
if (-not $NoDebugger) {
    if (Test-Path $Dbg) { Start-Process -FilePath $Dbg }
    else { Write-Warning "RecompDebugger not found: $Dbg (continuing runner-only)" }
}

$env:PS2_PC_WATCHDOG = 1
Write-Host "[launch_recomp] PS2_PC_WATCHDOG=1  (watchdog prints below)" -ForegroundColor Cyan
Write-Host "[launch_recomp] exe: $Exe" -ForegroundColor Cyan
Write-Host "[launch_recomp] elf: $Elf" -ForegroundColor Cyan
Write-Host "[launch_recomp] log: $Log" -ForegroundColor Cyan

# Runner runs in THIS terminal so watchdog stderr streams here, and every line
# is also captured to $Log (stdout+stderr) via Tee-Object for later grepping.
& $Exe $Elf 2>&1 | Tee-Object -FilePath $Log
