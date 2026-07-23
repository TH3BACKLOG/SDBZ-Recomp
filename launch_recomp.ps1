# launch_recomp.ps1 - Launch ps2EntryRunner (recomp backend) with the PC watchdog
# printing in THIS terminal, then bring up RecompDebugger.exe to attach.
#   .\launch_recomp.ps1                 (defaults below)
#   .\launch_recomp.ps1 -NoDebugger     (runner + watchdog only)
param(
    [string]$Elf = "F:\SDBZ Recomp\ELF\SLUS_214.42",
    [string]$Exe = "F:\SDBZ Recomp\build\ps2xRuntime\Debug\ps2EntryRunner.exe",
    [string]$Dbg = "F:\SDBZ Recomp\build\ps2xRuntime\Debug\RecompDebugger.exe",
    [string]$Log = "F:\SDBZ Recomp\run_log.txt",
    [string]$CdRoot = "F:\SDBZ Recomp\Super Dragon Ball Z ISO\Arcade Version\SDBZ ISO 2",
    [switch]$NoDebugger,
    [switch]$Full   # show EVERY console line (default: only important lines below)
)

# --- Tracers -----------------------------------------------------------------
# Baked-in env tracers. Add a new one = add a line here; it's set automatically.
$Tracers = @{
    PS2_PC_WATCHDOG = 1
    PS2_ARKD_TRACE  = 1
    # Phase 1 SIF-RPC diagnostic: dumps queue packet + RPC_SERVER_DATA tables and a
    # derail detector before/after the EE dispatcher runs. Instrumentation only.
    PS2_SIF_DIAG    = 1
    # S2.1 IRX loader: load+relocate+map always logs ([iop:irx]) when ARKD_DVD is
    # requested. Set to 1 to ALSO run the module's _start diagnostically (bounded)
    # to see the real import sequence (S2.2a). Honors a pre-set env value so
    #   $env:PS2_ARKD_IRX_RUN=1; .\launch_recomp.ps1
    # works; otherwise defaults to 0.
    PS2_ARKD_IRX_RUN = if ($env:PS2_ARKD_IRX_RUN) { $env:PS2_ARKD_IRX_RUN } else { 1 }
    # Stage-2 ARKD bridge: route real ARKD SIF-RPC CALLs (sid 0x500-0x503) into the
    # embedded R3000 running ARKD_DVD.IRX (ps2_iop_runArkdService) instead of the
    # Stage-1 observe-only path. Requires PS2_ARKD_IRX_RUN=1 (populates g_arkdServices).
    PS2_ARKD_SERVICE = if ($env:PS2_ARKD_SERVICE) { $env:PS2_ARKD_SERVICE } else { 1 }
    # Value-triggered guest-store trap: logs [trapval] for every guest store
    # whose stored value equals this, with the writing function's pc + dispatch
    # trace. Default hunts the 0x20561900 that clobbers $ra at the boot derail.
    # Optional ":ADDRLO:ADDRHI" suffix restricts it to a destination range.
    # Set to 0 to disable.
    PS2X_TRAPVAL = if ($env:PS2X_TRAPVAL) { $env:PS2X_TRAPVAL } else { '0x20561900' }
    # Frame tracer: 64-entry ring of {funcStart, entryPc, entrySp, exitPc, exitSp,
    # exitRa} over the wrapped slots in game_overrides.cpp, dumped by
    # reportMissingFunction at fault time. Flags mid-body entry (entryPc !=
    # funcStart) and frame imbalance (exitSp != entrySp). Wrappers install
    # regardless; this arms the recording.
    PS2X_FRAMETRACE = if ($env:PS2X_FRAMETRACE) { $env:PS2X_FRAMETRACE } else { 1 }
}
foreach ($k in $Tracers.Keys) { Set-Item -Path "Env:$k" -Value $Tracers[$k] }

# Point the pseudo-disc root at the full flat disc extraction so ARKD's
# sceCdSearchFile / cdvd reads resolve INFO.DAT / GAME.DAT (and later assets).
# Honors a pre-set $env:PS2_CD_ROOT; otherwise uses the -CdRoot default above.
if (-not $env:PS2_CD_ROOT) { $env:PS2_CD_ROOT = $CdRoot }
Write-Host "[launch_recomp] cdRoot: $env:PS2_CD_ROOT" -ForegroundColor Cyan

# --- Console filter -----------------------------------------------------------
# Full stream always goes to $Log. Console shows only lines matching this unless
# -Full is passed. Add patterns here as new tracers/errors matter.
$Important = '\[ARKD:|\[iop:|\[launch_recomp\]|\[trapval\]|\[gpr\]|\[stack\]|\[frametrace|SIF_DIAG|watchdog|error|fail|assert|warn|exception|unhandled'
# Known/understood spam suppressed from CONSOLE only (still in $Log). Add patterns
# here once a message is diagnosed so it stops flooding the screen.
$Mute = 'No exact recompiled function for guest PC 0x20561900'

foreach ($p in @($Exe, $Elf)) { if (-not (Test-Path $p)) { Write-Error "Not found: $p"; exit 1 } }

# Debugger in its own window first, so it's ready to attach the moment the shm appears.
if (-not $NoDebugger) {
    if (Test-Path $Dbg) { Start-Process -FilePath $Dbg }
    else { Write-Warning "RecompDebugger not found: $Dbg (continuing runner-only)" }
}

Write-Host "[launch_recomp] tracers: $($Tracers.Keys -join ', ')" -ForegroundColor Cyan
Write-Host "[launch_recomp] exe: $Exe" -ForegroundColor Cyan
Write-Host "[launch_recomp] elf: $Elf" -ForegroundColor Cyan
Write-Host "[launch_recomp] log: $Log  (full stream)" -ForegroundColor Cyan
if (-not $Full) { Write-Host "[launch_recomp] console filtered -> important lines only (-Full for raw)" -ForegroundColor DarkGray }

# Full stdout+stderr is ALWAYS captured to $Log via Tee-Object. The console is
# then filtered to the important lines unless -Full is passed, so nothing scrolls
# past too fast and the full record is still on disk for grepping.
if ($Full) {
    & $Exe $Elf 2>&1 | Tee-Object -FilePath $Log
} else {
    & $Exe $Elf 2>&1 | Tee-Object -FilePath $Log | Where-Object { $_ -match $Important -and $_ -notmatch $Mute }
}
