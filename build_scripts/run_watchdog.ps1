# run_watchdog.ps1 - Launch ps2EntryRunner with PC watchdog enabled.
# Usage:  .\run_watchdog.ps1            (uses default ELF path below)
#         .\run_watchdog.ps1 -Elf "F:\path\to\OTHER_ELF"

param(
    [string]$Elf = "F:\SDBZ Recomp\ELF\SLUS_214.42",
    [string]$Exe = "F:\SDBZ Recomp\build\ps2xRuntime\Debug\ps2EntryRunner.exe"
)

if (-not (Test-Path $Exe)) {
    Write-Error "Runner not found: $Exe  (build the ps2EntryRunner target first)"
    exit 1
}
if (-not (Test-Path $Elf)) {
    Write-Error "ELF not found: $Elf"
    exit 1
}

$env:PS2_PC_WATCHDOG = 1
Write-Host "[run_watchdog] PS2_PC_WATCHDOG=1" -ForegroundColor Cyan
Write-Host "[run_watchdog] exe: $Exe"          -ForegroundColor Cyan
Write-Host "[run_watchdog] elf: $Elf"          -ForegroundColor Cyan

& $Exe $Elf
