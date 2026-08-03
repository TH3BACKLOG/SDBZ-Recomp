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
    # Turn on the in-process CPU sampling profiler. Prints a [hostprof] report
    # naming the functions the host is actually burning CPU in. Not $Profile --
    # that is a PowerShell automatic variable.
    [switch]$HostProfile,
    # Arm the DR0 hardware watchpoint on the rpc_call saved-$ra slot. Off by
    # default since 2026-07-28 -- it costs ~20% of the run's CPU. Turn it on
    # only when hunting a memory writer, never during a perf measurement.
    [switch]$HwWatch,
    # Arm the standard guest-memory watch set as one unit (PS2X_WATCH). These five
    # addresses are the boot state machine's vital signs and were previously pasted
    # by hand from a markdown block -- forgetting one produced runs whose silence
    # looked like a result. See the -Watch block below for what each address is.
    [switch]$Watch,
    [switch]$Full,   # show EVERY console line (default: only important lines below)
    # Auto-stop the runner after N seconds so every diagnostic run is the same
    # length and comparable. 0 = run until closed by hand. 60 is the current
    # standard: steady state is established by t=8s and the early stall lands at
    # t=2-6s, so 60 captures both with margin.
    [int]$RunSeconds = 0,
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
    # EE-side mirror of libsifcmd's _sif_sreg[] table (base 0x561880, installed by
    # sceSifInitCmd at EE 0x177B00). WITHOUT THIS THE 08-01 ARKD/SREG COMPLETION
    # FIX IS COMPLETELY INERT: SET_SREG lands in the host-side g_sifSregs copy, the
    # guest never observes the completion word, and the run silently regresses to
    # pre-fix behaviour while looking healthy. RPC.cpp:132 defaults it to OFF, so it
    # has to be set here. It self-discloses in the log as
    #   "<-- NOT MIRRORED (set PS2_SIF_EE_SREG_BASE)"
    # which the post-run validity gate at the bottom of this script now checks for.
    PS2_SIF_EE_SREG_BASE = if ($env:PS2_SIF_EE_SREG_BASE) { $env:PS2_SIF_EE_SREG_BASE } else { '0x561880' }
    # Master gate for every GS diagnostic (ps2_diag::enabled(), ps2_diag.h:42):
    # [gs:image], [gs:frame], [gs:frame-change], [gs:ad], [present]. Stage 5.8's
    # exit test is stated in terms of these lines, so a run without this produces
    # no evidence either way -- not a negative result.
    PS2X_DIAG       = if ($env:PS2X_DIAG) { $env:PS2X_DIAG } else { 1 }
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
$Important = '\[hostprof\]|\[ARKD:|\[iop:|\[launch_recomp\]|\[trapval\]|\[gpr\]|\[stack\]|\[frametrace|\[determinism\]|\[present\]|\[gs-activity\]|SIF_DIAG|watchdog|error|fail|assert|warn|exception|unhandled'
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
# PS2X_DEBUGSHM gates the runner's shm writer, which costs a 2 KB cross-process
# memcpy on EVERY guest function call. Only pay it when something is reading.
# Set on Env directly, not into $Tracers -- that hashtable was already flushed
# to the environment above, so a late mutation would never reach the child.
if (-not $NoDebugger) {
    $env:PS2X_DEBUGSHM = '1'
    if (Test-Path $Dbg) { Start-Process -FilePath $Dbg }
    else { Write-Warning "RecompDebugger not found: $Dbg (continuing runner-only)" }
} else {
    # Must clear, not just skip: env vars persist across runs in the same shell.
    Remove-Item Env:PS2X_DEBUGSHM -ErrorAction SilentlyContinue
    Write-Host "[launch_recomp] -NoDebugger: debug shm writer disabled (PS2X_DEBUGSHM unset)" -ForegroundColor DarkGray
}

# Host CPU sampling profiler (src/lib/Kernel/HostSampler.cpp). Same env-directly
# rule as PS2X_DEBUGSHM above. It reports itself on its own timer rather than at
# shutdown, so the auto-report must land BEFORE the auto-stop kills the process:
# aim for RunSeconds minus a 6s margin.
if ($HostProfile) {
    $env:PS2X_PROFILE = '1'
    $env:PS2X_PROFILE_SECS = if ($RunSeconds -gt 12) { "$($RunSeconds - 6)" } else { '30' }
    Write-Host "[launch_recomp] -HostProfile: CPU sampler on, auto-report at $($env:PS2X_PROFILE_SECS)s" -ForegroundColor Cyan
} else {
    # Must clear, not just skip: env vars persist across runs in the same shell.
    Remove-Item Env:PS2X_PROFILE -ErrorAction SilentlyContinue
    Remove-Item Env:PS2X_PROFILE_SECS -ErrorAction SilentlyContinue
}

# Hardware data breakpoint on the rpc_call saved-$ra slot (game_overrides.cpp).
# OPT-IN as of 2026-07-28: its armer thread suspends every thread 4x/sec and
# [hostprof] measured it at 19.06s of a 96s run. Never leave it on for a run
# whose timing matters -- it silently taxes the number being measured.
if ($HwWatch) {
    $env:PS2X_HWWATCH = '1'
    Write-Host "[launch_recomp] -HwWatch: DR0 watchpoint armed -- adds ~20% CPU, do not trust perf numbers from this run" -ForegroundColor Yellow
} else {
    Remove-Item Env:PS2X_HWWATCH -ErrorAction SilentlyContinue
}

# Standard guest-memory watch set (PS2X_WATCH=ADDR[:SIZE][:LABEL][,...], consumed
# by ps2_watch::armWatchesFromEnv, ps2_runtime.cpp:2745). Same env-directly rule
# as above. Confirm it took with "[watch] armed 5" in the log -- 0 armed means
# every appinit/req12/snd00 conclusion from that run is void.
#   0x5E5924 CAppInit state word   (sub_327810 CAppInit::Update; 0x3e8 = retired,
#                                   the terminal value measured on real PCSX2)
#   0x5618B0 _sif_sreg[12]         (SET_SREG index 0xc -- the ARKD completion word)
#   0x5D6BA4 snd00                 (audio gate that held state 11)
#   0x5A9380 pkt_snd / 0x5A9358 pkt_ccd  (SIF packet slots)
if ($Watch) {
    $env:PS2X_WATCH = '0x5E5924:2:appinit_state,0x5618B0:4:req12,0x5D6BA4:4:snd00,0x5A9380:4:pkt_snd,0x5A9358:4:pkt_ccd'
    Write-Host "[launch_recomp] -Watch: 5 guest-memory watches queued (expect '[watch] armed 5' in the log)" -ForegroundColor Cyan
} else {
    Remove-Item Env:PS2X_WATCH -ErrorAction SilentlyContinue
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

# Native-command output piped through PowerShell is hard-wrapped at the console
# buffer width -- roughly 110 columns here -- and Tee-Object writes the already
# clipped text, so the LOG loses the tail of every long line too, not just the
# screen. That silently ate the last fields of every [watchdog] line and made a
# measurement look like it had not been built. Widen the buffer before launching.
# Non-fatal: some hosts (VS Code's terminal) have no settable RawUI.
try {
    $rawUi = $Host.UI.RawUI
    if ($rawUi.BufferSize.Width -lt 512) {
        $newBuf = $rawUi.BufferSize
        $newBuf.Width = 512
        $rawUi.BufferSize = $newBuf
    }
} catch {
    Write-Host "[launch_recomp] could not widen console buffer -- long log lines may be clipped: $_" -ForegroundColor Yellow
}

# --- CPU-time sampler ---------------------------------------------------------
# Answers the one question busy% cannot: is the executor BURNING CPU or BLOCKED?
# busy% times wall-clock inside ps2fiber_resume, so a fiber parked in a host call
# that never yields is indistinguishable from one spinning at full tilt. Process
# CPU time is not ambiguous:
#   cpu/wall ~= 100% of one core -> CPU-bound, cost is in emitted/runtime code
#   cpu/wall  <  ~15%            -> BLOCKED, chase whichever host call holds the
#                                   guest token
# Runs in a thread job because the Tee pipeline below blocks this thread until
# the process exits, and TotalProcessorTime is unreadable once it has.
# Also keeps a rolling per-thread snapshot: TotalProcessorTime per OS thread
# names the offender directly rather than just proving one exists.
$sampler = $null
$samplerCmd = Get-Command Start-ThreadJob -ErrorAction SilentlyContinue
if (-not $samplerCmd) { $samplerCmd = Get-Command Start-Job -ErrorAction SilentlyContinue }
if ($samplerCmd) {
    $sampler = & $samplerCmd -ArgumentList $Exe, $RunSeconds -ScriptBlock {
        param($ExePath, $LimitSeconds)

        # Match on full image path, not name: a stale ps2EntryRunner from an
        # earlier run would otherwise be sampled instead of this one.
        $proc = $null
        $deadline = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $deadline -and -not $proc) {
            $proc = Get-Process -ErrorAction SilentlyContinue |
                    Where-Object { $_.Path -eq $ExePath } |
                    Sort-Object StartTime -Descending |
                    Select-Object -First 1
            if (-not $proc) { Start-Sleep -Milliseconds 200 }
        }
        if (-not $proc) { return [pscustomobject]@{ Found = $false } }

        $started  = $proc.StartTime
        $lastCpu  = [TimeSpan]::Zero
        $lastThreads = @()
        $killed = $false

        while (-not $proc.HasExited) {
            try {
                $proc.Refresh()
                $lastCpu = $proc.TotalProcessorTime
                # Snapshot top threads by CPU. Wrapped: individual ProcessThread
                # entries throw as threads die between enumeration and read.
                $lastThreads = @(
                    $proc.Threads |
                        ForEach-Object {
                            try {
                                [pscustomobject]@{
                                    Id  = $_.Id
                                    Cpu = $_.TotalProcessorTime.TotalSeconds
                                    Wait = "$($_.ThreadState)/$($_.WaitReason)"
                                }
                            } catch { }
                        } |
                        Sort-Object Cpu -Descending |
                        Select-Object -First 6
                )
            } catch { break }

            if ($LimitSeconds -gt 0 -and
                ((Get-Date) - $started).TotalSeconds -ge $LimitSeconds) {
                $killed = $true
                try { $proc.CloseMainWindow() | Out-Null } catch { }
                # 6s, not 2: the clean shutdown path can now print a [hostprof]
                # report, and symbol resolution against the runner's PDB is slow.
                Start-Sleep -Seconds 6
                if (-not $proc.HasExited) { try { $proc.Kill() } catch { } }
                break
            }
            Start-Sleep -Milliseconds 500
        }

        $wall = ((Get-Date) - $started).TotalSeconds
        [pscustomobject]@{
            Found      = $true
            WallSec    = [math]::Round($wall, 2)
            CpuSec     = [math]::Round($lastCpu.TotalSeconds, 2)
            CorePct    = if ($wall -gt 0) { [math]::Round(($lastCpu.TotalSeconds / $wall) * 100, 1) } else { 0 }
            AutoKilled = $killed
            TopThreads = $lastThreads
        }
    }
}
if ($RunSeconds -gt 0) {
    Write-Host "[launch_recomp] auto-stop after ${RunSeconds}s" -ForegroundColor Cyan
}

# Full stdout+stderr is ALWAYS captured to $Log via Tee-Object. The console is
# then filtered to the important lines unless -Full is passed, so nothing scrolls
# past too fast and the full record is still on disk for grepping.
if ($Full) {
    & $Exe $Elf 2>&1 | Tee-Object -FilePath $Log
} else {
    & $Exe $Elf 2>&1 | Tee-Object -FilePath $Log | Where-Object { $_ -match $Important -and $_ -notmatch $Mute }
}

# --- Post-run CPU verdict -----------------------------------------------------
if ($sampler) {
    $r = Receive-Job -Job $sampler -Wait -ErrorAction SilentlyContinue
    Remove-Job -Job $sampler -Force -ErrorAction SilentlyContinue
    if ($r -and $r.Found) {
        Write-Host ''
        Write-Host "[cputime] wall=$($r.WallSec)s  cpu=$($r.CpuSec)s  =$($r.CorePct)% of one core$(if ($r.AutoKilled) { '  (auto-stopped)' })" -ForegroundColor Green
        # The interpretation, printed inline so a pasted-back run is self-explaining.
        if ($r.CorePct -ge 70) {
            Write-Host "[cputime] CPU-BOUND -- time is being burned executing, not waiting. Cost is in emitted/runtime code." -ForegroundColor Green
        } elseif ($r.CorePct -le 15) {
            Write-Host "[cputime] BLOCKED -- the process is mostly idle. Find the host call holding the guest token." -ForegroundColor Yellow
        } else {
            Write-Host "[cputime] MIXED -- partly blocked, partly executing. Use the per-thread split below." -ForegroundColor Yellow
        }
        if ($r.TopThreads) {
            Write-Host "[cputime] top threads by CPU (id / sec / state):" -ForegroundColor DarkGray
            foreach ($t in $r.TopThreads) {
                Write-Host ("[cputime]   {0,-8} {1,8:N2}s  {2}" -f $t.Id, $t.Cpu, $t.Wait) -ForegroundColor DarkGray
            }
        }
    } elseif ($r) {
        Write-Host "[cputime] runner process was never found -- no CPU measurement this run." -ForegroundColor Yellow
    }
}

# --- Post-run VALIDITY GATE ---------------------------------------------------
# A run can fail in a way that produces a complete, healthy-looking log full of
# nothing. Three documented cases, each of which has already cost a session:
#   1. SREG not mirrored     -> the ARKD/SREG completion fix is inert; you are
#                               measuring the PRE-fix build and will not know.
#   2. no watches armed      -> "appinit never moved" is unfalsifiable, not false.
#   3. a probe hit its cap   -> a saturated probe and an absent one look identical,
#                               so the ABSENCE of that tag proves nothing.
# In all three the danger is the same: absence of evidence reads as evidence of
# absence. Say so loudly rather than leaving it to a grep nobody runs.
if (Test-Path $Log) {
    $gate = @()
    $logText = Get-Content -LiteralPath $Log -Raw -ErrorAction SilentlyContinue

    if ($logText -match 'NOT MIRRORED') {
        $gate += 'SREG NOT MIRRORED -- PS2_SIF_EE_SREG_BASE did not take. The 08-01 ARKD/SREG completion fix was INERT this run.'
    }
    if ($Watch -and $logText -notmatch '\[watch\] armed') {
        $gate += '-Watch was passed but no "[watch] armed" line appeared. No watch fired because none existed -- not because nothing changed.'
    }
    $caps = @([regex]::Matches($logText, '\[cap\][^\r\n]*') | ForEach-Object { $_.Value } | Select-Object -Unique)
    if ($caps.Count -gt 0) {
        $gate += "$($caps.Count) probe(s) hit their emit cap. The ABSENCE of those tags later in the log is meaningless:"
        $gate += $caps | ForEach-Object { "      $_" }
    }

    if ($gate.Count -gt 0) {
        Write-Host ''
        Write-Host '  ############################################################' -ForegroundColor Red
        Write-Host '  #  RUN VALIDITY FAILURE -- do not draw conclusions from it  #' -ForegroundColor Red
        Write-Host '  ############################################################' -ForegroundColor Red
        foreach ($g in $gate) { Write-Host "  ! $g" -ForegroundColor Red }
        Write-Host '  ############################################################' -ForegroundColor Red
    } else {
        Write-Host "[validity] run passes the validity gate (SREG mirrored$(if ($Watch) { ', watches armed' }), no capped probes)." -ForegroundColor Green
    }
}
