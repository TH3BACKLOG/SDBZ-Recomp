# repeat_run.ps1 - Phase A exit test: does a run REPEAT?
#
# Why this exists
# ---------------
# Four root causes were declared and withdrawn in ~10 days (07-19c, 07-20f,
# 07-23d, 07-25j). The common factor was not bad analysis -- it was that every
# conclusion rested on a single run, while the same binary derails three
# different ways (pc=0x30, the 0x178be8 $ra clobber, pc=0x100008). A probe
# record from a run that does not repeat cannot distinguish "this is the cause"
# from "this is what happened that time".
#
# This script runs the game N times, reduces each run to a DERAIL SIGNATURE, and
# reports whether the signatures agree. Phase A passes only at 5/5.
#
# Usage
#   & "F:\SDBZ Recomp\build_scripts\repeat_run.ps1"
#   & "F:\SDBZ Recomp\build_scripts\repeat_run.ps1" -Runs 5 -Seconds 25
#   & "F:\SDBZ Recomp\build_scripts\repeat_run.ps1" -Determinism 0   # baseline
#
# Compare the -Determinism 1 result against -Determinism 0: if the baseline is
# already 5/5, then wall-clock vblank pacing was NOT the variance source and
# Phase A needs a different fix. That comparison is the point -- run both.

param(
    [int]$Runs = 5,
    [int]$Seconds = 25,          # per-run wall time before the process is killed
    [int]$Determinism = 1,       # 0 = old wall-clock vblank pacing (baseline)
    [string]$Quantum = "",       # override PS2X_DET_VBLANK_QUANTUM, "" = launcher default
    [string]$OutDir = "F:\SDBZ Recomp\build_scripts\repeat_runs",
    [switch]$KeepLogs,           # keep per-run logs even when all runs agree
    # Which signature the PASS/FAIL verdict is scored on. Measured 07-26: the
    # DERAIL is already 5/5 reproducible at -Determinism 0 (identical firstBad,
    # always frametrace slot #23, always ~line 1120), while the terminal pc and
    # dispatch trace vary 4/5 -- that variance is the ~89,000-line spin AFTER the
    # derail, not the derail. Scoring 'full' therefore fails runs whose actual
    # fault is identical, which is the wrong question to ask of a diagnosis
    # harness. 'derail' scores the fault; 'full' scores fault + aftermath and is
    # the right choice only when investigating the spin itself.
    [ValidateSet('derail','full')]
    [string]$Scope = 'derail'
)

$ErrorActionPreference = "Stop"

$launcher = "F:\SDBZ Recomp\launch_recomp.ps1"
$exeName  = "ps2EntryRunner"
if (-not (Test-Path $launcher)) { Write-Error "Not found: $launcher"; exit 1 }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

# --- Signature extraction ----------------------------------------------------
# Log lines can wrap, so a single probe record may span two physical lines.
#
# Encoding is NOT fixed and must never be hardcoded again. Tee-Object writes
# UTF-16 under Windows PowerShell 5.1 but UTF-8 under pwsh 7, so run_log.txt
# written by a hand-run launcher and a log written by this script (which shells
# out to pwsh) differ. Hardcoding -Encoding unicode made every field parse as
# "none" and turned a real 5/5 determinism PASS into a meaningless one --
# exactly the fragile-extraction failure this harness exists to eliminate.
# Sniff the BOM instead.
function Get-LogEncoding([string]$path) {
    $bytes = New-Object byte[] 2
    # FileShare::ReadWrite, not OpenRead: the killed runner's file handle can
    # still be open for a moment, and an exclusive open would throw -- which
    # under $ErrorActionPreference='Stop' kills the whole batch mid-run.
    $fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open,
                                 [System.IO.FileAccess]::Read,
                                 [System.IO.FileShare]::ReadWrite)
    try { $n = $fs.Read($bytes, 0, 2) } finally { $fs.Dispose() }
    if ($n -lt 2) { return 'utf8' }
    if ($bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) { return 'unicode' }
    if ($bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF) { return 'bigendianunicode' }
    # UTF-16LE with no BOM: first char is ASCII, so the high byte reads as 0.
    if ($bytes[1] -eq 0) { return 'unicode' }
    return 'utf8'
}

# Lines worth keeping. Everything else is discarded as it streams past.
$script:KeepPattern =
    '^\[watchdog\]|^\[determinism\]|^\[frametrace|No exact recompiled function'

function Get-LogLines([string]$path) {
    if (-not (Test-Path $path)) { return @() }
    $enc = Get-LogEncoding $path

    # STREAM, never Get-Content the whole file. A -Determinism 0 run emits ~47 MB
    # / 90k lines (88,918 of them the same muted 0x20561900 miss), and loading
    # that into an array plus O(n^2) string concatenation for continuation-joining
    # wedges the script hard enough to look like a hung run -- a baseline batch was
    # cancelled for exactly that reason. Filtering during the read keeps the
    # retained set in the hundreds regardless of how much the runner spews.
    $encoding = switch ($enc) {
        'unicode'          { [System.Text.Encoding]::Unicode }
        'bigendianunicode' { [System.Text.Encoding]::BigEndianUnicode }
        default            { [System.Text.Encoding]::UTF8 }
    }

    $joined = New-Object System.Collections.Generic.List[string]
    $sb = New-Object System.Text.StringBuilder
    $keeping = $false
    $script:MissCount = 0

    # Flush the record being accumulated (a record plus any wrapped tails).
    $flush = {
        if ($keeping -and $sb.Length -gt 0) { $joined.Add($sb.ToString()) }
        $sb.Clear() | Out-Null
    }

    $sr = New-Object System.IO.StreamReader($path, $encoding)
    try {
        while (($line = $sr.ReadLine()) -ne $null) {
            if ($line -match '^\[') {
                # A new record starts here; emit the previous one.
                & $flush
                $keeping = ($line -match $script:KeepPattern)
                if ($keeping) { $sb.Append($line) | Out-Null }
            }
            elseif ($line -match 'No exact recompiled function') {
                # Not bracket-prefixed ("Error: No exact ..."), so handle it here.
                # Only the FIRST is used by the signature, but a baseline run
                # repeats it ~89,000 times -- keep a couple and count the rest,
                # or retaining them defeats the point of streaming.
                $script:MissCount++
                if ($script:MissCount -le 2) {
                    & $flush
                    $keeping = $true
                    $sb.Append($line) | Out-Null
                } else { $keeping = $false }
            }
            elseif ($keeping) {
                # Console wrapping: this is the tail of the record above it.
                # MUST come last -- 'Error: No exact ...' has no '[' prefix and
                # would otherwise be glued onto the preceding record, corrupting
                # it into nonsense like pc=0x1.
                $sb.Append($line) | Out-Null
            }
        }
        & $flush
    } finally { $sr.Dispose() }

    return $joined
}

function Get-DerailSignature([string]$path) {
    $lines = Get-LogLines $path
    $sig = [ordered]@{
        determinism = "off"
        lastPc      = "none"
        lastCall    = "none"
        lastRa      = "none"
        traceHash   = "none"
        progress    = "none"
        badCount    = 0
        firstBad    = "none"
        missingFn   = "none"
    }
    if ($lines.Count -eq 0) { return $sig }

    if ($lines | Where-Object { $_ -match '\[determinism\]' }) { $sig.determinism = "on" }

    # Terminal watchdog state = where execution ended up.
    $wd = @($lines | Where-Object { $_ -match '^\[watchdog\]' })
    if ($wd.Count -gt 0) {
        $last = $wd[-1]
        if ($last -match 'pc=(0x[0-9a-fA-F]+)')       { $sig.lastPc = $Matches[1] }
        if ($last -match ' ra=(0x[0-9a-fA-F]+)')      { $sig.lastRa = $Matches[1] }
        if ($last -match 'lastCall=(0x[0-9a-fA-F]+)') { $sig.lastCall = $Matches[1] }
        if ($last -match 'progress=(\d+)')            { $sig.progress = $Matches[1] }

        # The 64-entry dispatch trace is the strongest discriminator we have: two
        # runs can share a terminal pc yet have reached it by different paths.
        # Hashed because the raw string is ~800 chars and unreadable in a table.
        if ($last -match 'trace=(.+)$') {
            $md5 = [System.Security.Cryptography.MD5]::Create()
            $bytes = [System.Text.Encoding]::UTF8.GetBytes($Matches[1].Trim())
            $sig.traceHash = ([BitConverter]::ToString($md5.ComputeHash($bytes)) -replace '-','').Substring(0, 8)
            $md5.Dispose()
        }
    }

    # Frame-trace BAD records: count, plus the first one verbatim. The count is
    # part of the signature; the first record is what a fix has to explain.
    $bad = @($lines | Where-Object { $_ -match '\[frametrace:\w+\]\s+#\d+\s+BAD' })
    $sig.badCount = $bad.Count
    if ($bad.Count -gt 0) {
        if ($bad[0] -match '(slot=\S+.*?liveRa=\S+)') { $sig.firstBad = $Matches[1] }
        else { $sig.firstBad = $bad[0].Trim() }
    }

    $miss = @($lines | Where-Object { $_ -match 'No exact recompiled function for guest PC (0x[0-9a-fA-F]+)' })
    if ($miss.Count -gt 0 -and $miss[0] -match 'guest PC (0x[0-9a-fA-F]+)') {
        $sig.missingFn = $Matches[1]
    }
    return $sig
}

# NOTE: `progress` and `stuckSecs` are deliberately NOT part of the signature.
# Each run is killed on WALL time, so the guest retires a slightly different
# number of back-edges every time even when execution is perfectly reproducible.
# Including them would report FAIL on a run that is in fact identical.
function Format-Signature($sig) {
    return ("pc={0} ra={1} lastCall={2} trace={3} bad={4} missingFn={5} firstBad=[{6}]" -f `
            $sig.lastPc, $sig.lastRa, $sig.lastCall, $sig.traceHash, `
            $sig.badCount, $sig.missingFn, $sig.firstBad)
}

# The fault alone, with every post-fault field stripped. lastPc/lastRa/lastCall/
# traceHash all describe where the run happened to be when the 25s kill landed,
# which is somewhere inside a dispatch-miss spin that retires tens of thousands
# of iterations per second -- they cannot agree run to run and it means nothing
# when they don't.
function Format-DerailSignature($sig) {
    return ("bad={0} missingFn={1} firstBad=[{2}]" -f `
            $sig.badCount, $sig.missingFn, $sig.firstBad)
}

# --- Run loop ----------------------------------------------------------------
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$results = @()

Write-Host "[repeat_run] runs=$Runs seconds=$Seconds determinism=$Determinism" -ForegroundColor Cyan

for ($i = 1; $i -le $Runs; $i++) {
    $log = Join-Path $OutDir "run-$stamp-$i.txt"

    # Kill any stragglers from a previous iteration before starting a fresh one,
    # or two runners will fight over the same shared-memory debug region.
    Get-Process -Name $exeName -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300

    Write-Host "[repeat_run] run $i/$Runs -> $log" -ForegroundColor DarkGray

    # Env is set here rather than by editing the launcher: launch_recomp.ps1's
    # $Tracers table honours a pre-set value for every knob.
    $envAssign = "`$env:PS2X_DETERMINISM='$Determinism';"
    if ($Quantum -ne "") { $envAssign += "`$env:PS2X_DET_VBLANK_QUANTUM='$Quantum';" }

    $cmd = "$envAssign & '$launcher' -NoDebugger -Log '$log' | Out-Null"
    $proc = Start-Process -FilePath "pwsh" `
                          -ArgumentList "-NoProfile", "-Command", $cmd `
                          -PassThru -WindowStyle Hidden

    # Show liveness while waiting. The runner window is hidden and the script is
    # otherwise silent for $Seconds, which is indistinguishable from a wedge --
    # a baseline batch was cancelled for exactly that reason. Print elapsed time
    # plus the log's byte count: a growing log proves the run is alive, a log
    # stuck at 0 bytes means it never started (bad launcher path, exe missing).
    $deadline = (Get-Date).AddSeconds($Seconds)
    $started  = Get-Date
    $nextTick = 0
    while ((Get-Date) -lt $deadline -and -not $proc.HasExited) {
        Start-Sleep -Milliseconds 500
        $elapsed = [int]((Get-Date) - $started).TotalSeconds
        if ($elapsed -ge $nextTick) {
            $size = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
            Write-Host ("    {0,2}s/{1}s  log={2} bytes" -f $elapsed, $Seconds, $size) -ForegroundColor DarkGray
            $nextTick = $elapsed + 5
        }
    }
    if ($proc.HasExited) {
        Write-Host ("    runner exited on its own after {0}s" -f [int]((Get-Date) - $started).TotalSeconds) -ForegroundColor DarkGray
    }

    Get-Process -Name $exeName -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500

    $sig = Get-DerailSignature $log
    $results += [pscustomobject]@{
        Run       = $i
        Log       = $log
        Det       = $sig.determinism
        Signature = Format-Signature $sig
        Derail    = Format-DerailSignature $sig
        Progress  = $sig.progress
    }
    Write-Host ("  det={0} progress={1} {2}" -f $sig.determinism, $sig.progress, (Format-Signature $sig))
}

# --- Verdict -----------------------------------------------------------------
Write-Host ""
Write-Host "=== Phase A verdict ===" -ForegroundColor Cyan
Write-Host "scope=$Scope (verdict is scored on this column)" -ForegroundColor DarkGray
if ($Scope -eq 'derail') {
    $results | Format-Table Run, Det, Progress, Derail -AutoSize | Out-String -Width 400 | Write-Host
    Write-Host "  post-derail terminal state (NOT scored):" -ForegroundColor DarkGray
    $results | Format-Table Run, Signature -AutoSize | Out-String -Width 400 | Write-Host
} else {
    $results | Format-Table Run, Det, Progress, Signature -AutoSize | Out-String -Width 400 | Write-Host
}

if ($results.Count -eq 0) {
    Write-Host "INCONCLUSIVE: no runs executed (-Runs $Runs)." -ForegroundColor Yellow
    exit 2
}

$distinct = if ($Scope -eq 'derail') { @($results.Derail | Sort-Object -Unique) }
            else                    { @($results.Signature | Sort-Object -Unique) }
$detOff   = @($results | Where-Object { $_.Det -ne "on" })
$empty    = @($results | Where-Object { $_.Signature -match 'pc=none' })

# An empty signature means we failed to READ the log, not that the runs agreed.
# Five identical "none" rows once scored as PASS (a bad -Encoding), which is the
# most dangerous possible bug in a harness whose whole job is to be trusted.
if ($empty.Count -gt 0) {
    Write-Host "INCONCLUSIVE: $($empty.Count)/$Runs run(s) yielded no [watchdog] line." -ForegroundColor Yellow
    Write-Host "  The log was not readable or the run never started. Logs kept in $OutDir" -ForegroundColor Yellow
    exit 2
}

if ($Determinism -ne 0 -and $detOff.Count -gt 0) {
    # A determinism run whose log never printed the banner did not actually run
    # deterministically -- most likely a stale build. Say so rather than scoring it.
    Write-Host "INCONCLUSIVE: $($detOff.Count)/$Runs run(s) never printed [determinism]." -ForegroundColor Yellow
    Write-Host "  The build predates the PS2X_DETERMINISM change. Rebuild and re-run." -ForegroundColor Yellow
    exit 2
}

# A derail-scope batch where nothing derailed agrees trivially. Under
# -Determinism 1 that is exactly what happens (bad=0, zero dispatch misses), and
# scoring it PASS would report "the fault reproduces 5/5" about a run containing
# no fault at all -- the same class of false PASS as the empty-signature bug.
$noFault = @($results | Where-Object { $_.Derail -match 'bad=0' })
if ($Scope -eq 'derail' -and $noFault.Count -eq $results.Count) {
    Write-Host "INCONCLUSIVE: no run derailed (bad=0 in $($noFault.Count)/$Runs)." -ForegroundColor Yellow
    Write-Host "  Nothing to compare. This configuration avoids the fault rather than" -ForegroundColor Yellow
    Write-Host "  reproducing it, so it cannot be the basis for diagnosing it." -ForegroundColor Yellow
    exit 2
}

if ($distinct.Count -eq 1) {
    Write-Host "PASS: $Runs/$Runs runs share one derail signature." -ForegroundColor Green
    Write-Host "  $($distinct[0])"
    if (-not $KeepLogs) {
        Remove-Item (Join-Path $OutDir "run-$stamp-*.txt") -Force -ErrorAction SilentlyContinue
        Write-Host "  (per-run logs removed; -KeepLogs to retain)" -ForegroundColor DarkGray
    }
    exit 0
} else {
    Write-Host "FAIL: $($distinct.Count) distinct signatures across $Runs runs." -ForegroundColor Red
    foreach ($d in $distinct) { Write-Host "  $d" }
    Write-Host "  Per-run logs kept in $OutDir" -ForegroundColor DarkGray
    exit 1
}
