# launch_recomp.ps1 - Launch ps2EntryRunner (recomp backend) with the PC watchdog
# printing in THIS terminal, then bring up RecompDebugger.exe to attach.
#   .\launch_recomp.ps1                 (defaults below)
#   .\launch_recomp.ps1 -NoDebugger     (runner + watchdog only)
param(
    [string]$Elf = "F:\SDBZ Recomp\ELF\SLUS_214.42",
    [string]$Exe = "F:\SDBZ Recomp\build\ps2xRuntime\Debug\ps2EntryRunner.exe",
    [string]$Dbg = "F:\SDBZ Recomp\build\ps2xRuntime\Debug\RecompDebugger.exe",
    [string]$Log = "F:\SDBZ Recomp\run_log.txt",
    # Phase B structured probe sink. Written on its own file descriptor by
    # ps2x_probe_kv -- it never goes through the console pipe, so Tee-Object
    # encoding and console line-wrapping cannot corrupt it. Query with
    # build_scripts\analyze_run.py. Per-run path lets a batch keep them apart.
    [string]$Probe = "F:\SDBZ Recomp\run_probe.jsonl",
    [string]$CdRoot = "F:\SDBZ Recomp\Super Dragon Ball Z ISO\Arcade Version\SDBZ ISO 2",
    [switch]$NoDebugger,
    [switch]$Full,   # show EVERY console line (default: only important lines below)
    # Determinism override, '' = use the $Tracers default (1). Pass -Determinism 0
    # for DIAGNOSTIC runs: det=1 paces vblank off guest progress and the boot
    # derail does not occur at all, so a det=1 run produces an empty --derail
    # report and looks like "nothing is wrong". The fault reproduces 5/5 at
    # det=0 (Phase A). Exposed as a parameter because reaching for the env var
    # by hand has already cost one confusing empty run.
    [ValidateSet('', '0', '1')]
    [string]$Determinism = ''
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
    # trace. Optional ":ADDRLO:ADDRHI" suffix restricts it to a destination range.
    #
    # DEFAULT IS OFF as of 07-26. It used to default to 0x20561900, which was
    # measured across 5 baseline runs to be the wrong value entirely: $ra is
    # clobbered to 0x1, not 0x20561900 (see the RASLOT #23 record -- saved=0x1
    # liveRa=0x1). Do NOT simply set this to 0x1; storing the literal 1 is
    # overwhelmingly common in normal guest code and the trap would fire
    # constantly. Set it to a specific value you have evidence for.
    PS2X_TRAPVAL = if ($env:PS2X_TRAPVAL) { $env:PS2X_TRAPVAL } else { '0' }
    # Frame tracer: 64-entry ring of {funcStart, entryPc, entrySp, exitPc, exitSp,
    # exitRa} over the wrapped slots in game_overrides.cpp, dumped by
    # reportMissingFunction at fault time. Flags mid-body entry (entryPc !=
    # funcStart) and frame imbalance (exitSp != entrySp). Wrappers install
    # regardless; this arms the recording.
    PS2X_FRAMETRACE = if ($env:PS2X_FRAMETRACE) { $env:PS2X_FRAMETRACE } else { 1 }
    # Determinism (Phase A). The VBLANK IRQ worker normally paces itself off
    # steady_clock, so interrupts land at a wall-clock-dependent point in the
    # guest instruction stream and the derail signature changes run to run
    # (pc=0x30 / the 0x178be8 clobber / pc=0x100008 from one binary). With this
    # set, vblank is paced off GUEST PROGRESS instead, so a run repeats.
    # Look for "[determinism] vblank paced by guest progress" in the log to
    # confirm it took effect. Set to 0 for the old wall-clock behaviour.
    # Precedence: -Determinism parameter > pre-set env var > default 1.
    PS2X_DETERMINISM = if ($Determinism -ne '') { $Determinism }
                       elseif ($env:PS2X_DETERMINISM) { $env:PS2X_DETERMINISM }
                       else { 1 }
    # Guest-progress ticks per vblank under PS2X_DETERMINISM. One tick = 128
    # guest back-edges. There is no principled default; tune this if the game
    # runs visibly too fast or too slow relative to the old wall-clock pacing.
    PS2X_DET_VBLANK_QUANTUM = if ($env:PS2X_DET_VBLANK_QUANTUM) { $env:PS2X_DET_VBLANK_QUANTUM } else { 20000 }
}
foreach ($k in $Tracers.Keys) { Set-Item -Path "Env:$k" -Value $Tracers[$k] }

# Set outside $Tracers because it is a path, not a 0/1 knob. The runtime
# truncates this file at open, so each run owns its sink -- appending is how
# records from a previous build get mistaken for the current one.
$env:PS2X_PROBE_FILE = $Probe

# Point the pseudo-disc root at the full flat disc extraction so ARKD's
# sceCdSearchFile / cdvd reads resolve INFO.DAT / GAME.DAT (and later assets).
# Honors a pre-set $env:PS2_CD_ROOT; otherwise uses the -CdRoot default above.
if (-not $env:PS2_CD_ROOT) { $env:PS2_CD_ROOT = $CdRoot }
Write-Host "[launch_recomp] cdRoot: $env:PS2_CD_ROOT" -ForegroundColor Cyan

# --- Console filter -----------------------------------------------------------
# Full stream always goes to $Log. Console shows only lines matching this unless
# -Full is passed. Add patterns here as new tracers/errors matter.
$Important = '\[ARKD:|\[iop:|\[launch_recomp\]|\[trapval\]|\[gpr\]|\[stack\]|\[frametrace|\[determinism\]|SIF_DIAG|watchdog|error|fail|assert|warn|exception|unhandled'
# Known/understood spam suppressed from CONSOLE only (still in $Log). Add patterns
# here once a message is diagnosed so it stops flooding the screen.
# Post-derail dispatch-miss spin: once $ra is clobbered the dispatcher jumps to
# the bad target forever and emits this ~89,000 times in 25s (47 MB of log). The
# FIRST one matters and is preserved in $Log; the rest are the same event.
# Was keyed to 0x20561900 and therefore matched nothing once the real clobber
# value turned out to be 0x1 -- hence the flood. Address-agnostic now so a future
# change of target does not silently re-open it.
$Mute = 'No exact recompiled function for guest PC'

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
Write-Host "[launch_recomp] probe sink: $Probe" -ForegroundColor Cyan
# Say this out loud. A det=1 run simply does not derail, so analyze_run.py
# reports nothing and the run reads as healthy when it is only quiet.
if ($Tracers.PS2X_DETERMINISM -eq 1) {
    Write-Host "[launch_recomp] PS2X_DETERMINISM=1 -- the boot derail does NOT reproduce in this mode. Use -Determinism 0 to diagnose it." -ForegroundColor Yellow
}
if (-not $Full) { Write-Host "[launch_recomp] console filtered -> important lines only (-Full for raw)" -ForegroundColor DarkGray }

# Full stdout+stderr is ALWAYS captured to $Log via Tee-Object. The console is
# then filtered to the important lines unless -Full is passed, so nothing scrolls
# past too fast and the full record is still on disk for grepping.
if ($Full) {
    & $Exe $Elf 2>&1 | Tee-Object -FilePath $Log
} else {
    & $Exe $Elf 2>&1 | Tee-Object -FilePath $Log | Where-Object { $_ -match $Important -and $_ -notmatch $Mute }
}
