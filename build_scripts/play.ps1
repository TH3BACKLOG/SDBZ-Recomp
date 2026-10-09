# play.ps1 - launch the game for MANUAL play (original mode) at best speed.
#   pwsh -NoProfile -File "F:\SDBZ Recomp\build_scripts\play.ps1"
# Thin wrapper over launch_recomp.ps1: DIAG off (the default there is ON and costs
# ~10%), no debugger shm, no autopress, no auto-stop, determinism ON (never det=0),
# vblank quantum 3000 (the value fights are verified at). Log still goes to
# run_log.txt, so [scene]/[texmiss]-style evidence from your play is kept.
param(
    [string]$Exe = "F:\SDBZ Recomp\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe",
    [int]$Quantum = 3000,
    # Opt-in GS draw/upload log (costs CPU). Analyse after with:
    #   python build_scripts\texmiss.py <file> --runlog run_log.txt
    [switch]$Texmiss,
    # CPU sampler report at the end of a timed run; pair with -RunSeconds.
    [switch]$HostProfile,
    # Keep the game's bilinear texture filtering. Default is point sampling
    # (PS2X_GS_NEAREST=1): faster raster, slightly blockier textures. Not pixel-exact vs PCSX2.
    [switch]$Bilinear,
    [int]$RunSeconds = 0
)

$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

# Env persists across runs in one shell: clear anything a test run left behind.
foreach ($v in 'PS2X_PAD_AUTOPRESS', 'PS2X_PAD_AUTOPRESS_BTNS', 'PS2X_PAD_AUTOPRESS_HOLD',
               'PS2X_PAD_SCRIPT', 'PS2X_VUCAP', 'PS2X_GSCAP', 'PS2X_HWWATCH', 'PS2X_TEXMISS_LOG') {
    Remove-Item "Env:$v" -ErrorAction SilentlyContinue
}
$env:PS2X_DIAG = '0'
if ($Bilinear) { Remove-Item Env:PS2X_GS_NEAREST -ErrorAction SilentlyContinue } else { $env:PS2X_GS_NEAREST = '1' }
$env:PS2X_DET_VBLANK_QUANTUM = "$Quantum"

if ($Texmiss) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $env:PS2X_TEXMISS_LOG = Join-Path $root "Logs\play_$stamp`_texmiss.jsonl"
    Write-Host "[play] texmiss log -> $($env:PS2X_TEXMISS_LOG)" -ForegroundColor Cyan
}

Write-Host "[play] DIAG=0 quantum=$Quantum nearest=$(-not $Bilinear)  (real gamepad required; close the window to stop)" -ForegroundColor Cyan

$args2 = @{ Exe = $Exe; NoDebugger = $true; RunSeconds = $RunSeconds }
if ($HostProfile) { $args2['HostProfile'] = $true }
& (Join-Path $root 'launch_recomp.ps1') @args2
