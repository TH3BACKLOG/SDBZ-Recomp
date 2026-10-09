# play.ps1 - launch the game for MANUAL play (original mode) at best speed.
#   pwsh -NoProfile -File "F:\SDBZ Recomp\build_scripts\play.ps1"
# Thin wrapper over launch_recomp.ps1: DIAG off (the default there is ON and costs
# ~10%), no debugger shm, boot script (play_boot.txt) to the main menu, no auto-stop, determinism ON (never det=0),
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
    [int]$RunSeconds = 0,
    # Automatic controls (default ON): build_scripts\sweeps\play_boot.txt presses the
    # right buttons to get past the warning / memory-card / title screens to the main
    # menu, then ends; scripted presses are OR-ed with your keys. -NoAuto turns it off.
    # Keys: arrows/WASD, X/Space = Cross, C = Circle, Z = Square, V = Triangle,
    # Q/E = L1/R1, Shift = L2/R2, Enter = Start, Tab = Select (keyboard only when no
    # gamepad is plugged in).
    [switch]$NoAuto
)

$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

# Env persists across runs in one shell: clear anything a test run left behind.
foreach ($v in 'PS2X_PAD_AUTOPRESS', 'PS2X_PAD_AUTOPRESS_BTNS', 'PS2X_PAD_AUTOPRESS_HOLD', 'PS2X_PAD_AUTOPRESS_SECS',
               'PS2X_PAD_SCRIPT', 'PS2X_VUCAP', 'PS2X_GSCAP', 'PS2X_HWWATCH', 'PS2X_TEXMISS_LOG') {
    Remove-Item "Env:$v" -ErrorAction SilentlyContinue
}
$env:PS2X_DIAG = '0'
if (-not $NoAuto) {
    $env:PS2X_PAD_SCRIPT = Join-Path $root 'build_scripts\sweeps\play_boot.txt'
}
# ps2EntryRunner defaults an UNSET value to 1, so -Bilinear must say 0 explicitly.
$env:PS2X_GS_NEAREST = if ($Bilinear) { '0' } else { '1' }
$env:PS2X_DET_VBLANK_QUANTUM = "$Quantum"

if ($Texmiss) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $env:PS2X_TEXMISS_LOG = Join-Path $root "Logs\play_$stamp`_texmiss.jsonl"
    Write-Host "[play] texmiss log -> $($env:PS2X_TEXMISS_LOG)" -ForegroundColor Cyan
}

Write-Host "[play] DIAG=0 quantum=$Quantum nearest=$(-not $Bilinear) auto-boot=$(if ($NoAuto) { 'off' } else { 'on' })  (close the window to stop)" -ForegroundColor Cyan

$args2 = @{ Exe = $Exe; NoDebugger = $true; RunSeconds = $RunSeconds }
if ($HostProfile) { $args2['HostProfile'] = $true }
& (Join-Path $root 'launch_recomp.ps1') @args2
