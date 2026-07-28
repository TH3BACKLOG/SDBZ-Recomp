#!/usr/bin/env pwsh
# boot_test.ps1 (pr137-test sandbox) — timed run + capture + automatic spin detection
param(
    [switch]$Release,
    [int]$Seconds       = 999999,
    [int]$SpinThreshold = 6,
    # Lines matching this pattern are excluded from spin detection (vsync sema fires every frame)
    [string]$IgnoreLines = '^\[(WaitSema|SignalSema|iSignalSema|gs:)'
)

$config = if ($Release) { "Release" } else { "Debug" }
$root   = $PSScriptRoot
$exe    = "$root\build\ps2xRuntime\$config\ps2EntryRunner.exe"
$elf    = "$root\ELF\SLUS_214.42"
$logDir = "$root\run_logs"

if (-not (Test-Path $logDir)) { New-Item -ItemType Directory $logDir | Out-Null }
$ts      = Get-Date -Format "yyyyMMdd_HHmmss"
$logFile = "$logDir\run_$ts.txt"
$errFile = "$logDir\run_$ts.stderr.txt"

if (-not (Test-Path $exe)) { Write-Host "EXE not found: $exe" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $elf)) { Write-Host "ELF not found: $elf" -ForegroundColor Red; exit 1 }

Write-Host "Starting exe (max $Seconds s, kills early on spin)..."
Write-Host "Log -> $logFile"
$launchTime = Get-Date
$proc = Start-Process `
    -FilePath      $exe `
    -ArgumentList  "`"$elf`"" `
    -RedirectStandardOutput $logFile `
    -RedirectStandardError  $errFile `
    -PassThru -NoNewWindow

# --- Early-kill: poll every 250 ms; kill as soon as spin detected ---
function Find-Spin($lines, $threshold, $ignoreRx) {
    if (-not $lines -or $lines.Count -eq 0) { return $null }
    $runLen  = 1
    $runLine = $lines[0]
    for ($i = 1; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -eq $lines[$i-1]) {
            if ($ignoreRx -and $ignoreRx.IsMatch($lines[$i])) { $runLen = 1; continue }
            $runLen++
            if ($runLen -gt $threshold) { return @{ Line = $runLine; StartIdx = $i - $runLen + 1 } }
        } else {
            $runLen  = 1
            $runLine = $lines[$i]
        }
    }
    return $null
}
$ignRx = if ($IgnoreLines) { [regex]$IgnoreLines } else { $null }

$sw          = [System.Diagnostics.Stopwatch]::StartNew()
$killedEarly = $false
$exitedEarly = $false
while ($sw.Elapsed.TotalSeconds -lt $Seconds -and -not $proc.HasExited) {
    Start-Sleep -Milliseconds 250
    $peek = Get-Content $logFile -ErrorAction SilentlyContinue
    if ($peek -and (Find-Spin $peek $SpinThreshold $ignRx)) { $killedEarly = $true; break }
}
if ($proc.HasExited -and -not $killedEarly) {
    $exitedEarly = $true
    Write-Host "Process exited on its own after $([math]::Round($sw.Elapsed.TotalSeconds,1))s (exit code $($proc.ExitCode))" -ForegroundColor Yellow
}
if (-not $proc.HasExited) {
    try { $proc.Kill() } catch { }
    $proc.WaitForExit(3000) | Out-Null
    # Forceful fallback for GLFW/GUI processes that survive Kill()
    if (-not $proc.HasExited) { & taskkill /F /PID $proc.Id /T 2>$null }
    if ($killedEarly) { Write-Host "Killed early at $([math]::Round($sw.Elapsed.TotalSeconds,1))s (spin detected)" }
    else              { Write-Host "Killed after $Seconds s" }
}
Start-Sleep 1  # let OS flush file handles

$lines = Get-Content $logFile -ErrorAction SilentlyContinue

# --- Silent-crash detection (pr137-test specific symptom: exits early, zero output) ---
if ($exitedEarly -and (-not $lines -or $lines.Count -eq 0)) {
    Write-Host ""
    Write-Host "*** SILENT CRASH SYMPTOM: process exited early with zero console output ***" -ForegroundColor Red
    Write-Host "    Exit code: $($proc.ExitCode)" -ForegroundColor Red

    $crashEvents = Get-WinEvent -FilterHashtable @{
        LogName      = 'Application'
        ProviderName = 'Application Error'
        StartTime    = $launchTime
    } -ErrorAction SilentlyContinue | Where-Object { $_.Message -match 'ps2EntryRunner' }

    if ($crashEvents) {
        Write-Host "    Crash record found in Event Viewer:" -ForegroundColor Red
        $crashEvents | ForEach-Object { Write-Host "    $($_.Message)" }
    } else {
        Write-Host "    No Application Error record in Event Viewer (SEH-level crash may not log one)." -ForegroundColor Yellow
        Write-Host "    Use the 'TEST UB - pr137-test ps2EntryRunner (cppvsdbg)' launch config for a real call stack." -ForegroundColor Yellow
    }
}

if (-not $lines) { Write-Host "`nLog empty (buffering issue? try Tee-Object run manually)" -ForegroundColor Yellow; exit 0 }

# --- Spin detection ---
$spin = Find-Spin $lines $SpinThreshold $ignRx

Write-Host ""
if ($spin) {
    $totalHits = ($lines | Where-Object { $_ -eq $spin.Line }).Count
    Write-Host "*** SPIN DETECTED ($totalHits hits):" -ForegroundColor Red
    Write-Host "    $($spin.Line)" -ForegroundColor Red
    $beforeSpin = $lines[0..([Math]::Max(0, $spin.StartIdx - 1))] | Where-Object { $_ -ne $spin.Line }
    $lastUnique = $beforeSpin | Select-Object -Last 1
    Write-Host "    Last unique before spin: $lastUnique" -ForegroundColor Yellow

    # --- Correlate spin to owning tid + recent PC history ---
    $persistRx = [regex]'persistentThread tid=(\d+): ran (\d+) steps pc=0x([0-9A-Fa-f]+)'
    $lastTid = $null
    for ($i = $spin.StartIdx; $i -ge 0; $i--) {
        $m = $persistRx.Match($lines[$i])
        if ($m.Success) { $lastTid = $m.Groups[1].Value; break }
    }
    if ($lastTid) {
        $createRx = [regex]"\[thbase\] CreateThread entry=0x([0-9A-Fa-f]+).*?tid=$lastTid\b"
        $entry = ($lines | Where-Object { $_ -match $createRx } | Select-Object -First 1)
        $entryAddr = if ($entry -and $entry -match $createRx) { $matches[1] } else { "?" }
        Write-Host "    Owning tid=$lastTid entry=0x$entryAddr" -ForegroundColor Yellow

        $pcHistory = $lines | Where-Object { $_ -match "persistentThread tid=$lastTid\b" } |
            ForEach-Object { if ($_ -match $persistRx) { $matches[3] } } |
            Select-Object -Last 20
        if ($pcHistory) {
            Write-Host ("    PC history: " + (($pcHistory | ForEach-Object { "0x$_" }) -join " -> ")) -ForegroundColor Yellow
        }
    }
} else {
    Write-Host "No spin detected." -ForegroundColor Green
}

# --- IOP call frequency table ---
$iopLines = $lines | Where-Object { $_ -match '^\[' }
Write-Host "`n--- IOP calls: $($iopLines.Count) total ---"
$iopLines | Group-Object | Sort-Object Count -Descending | Select-Object -First 15 |
    ForEach-Object { Write-Host ("{0,7}x  {1}" -f $_.Count, $_.Name) }

# --- Stderr: unregistered function calls ---
$errLines = Get-Content $errFile -ErrorAction SilentlyContinue
$unknown  = $errLines | Where-Object { $_ -match '\[IopKernel\] dispatch: unknown code=' } |
            Group-Object | Sort-Object Count -Descending | Select-Object -First 5
if ($unknown) {
    Write-Host "`n--- Unregistered IOP calls (stderr) ---" -ForegroundColor Magenta
    $unknown | ForEach-Object { Write-Host ("{0,7}x  {1}" -f $_.Count, $_.Name) }
}

# --- Tail ---
Write-Host "`n--- Last 10 lines ---" -ForegroundColor Cyan
$lines | Select-Object -Last 10 | ForEach-Object { Write-Host $_ }

# Also update the shared run_log.txt so any future check_log.ps1-style tool works without an argument
Copy-Item $logFile "$root\run_log.txt" -Force
Copy-Item $errFile "$root\run_log.stderr.txt" -Force -ErrorAction SilentlyContinue
Write-Host "`nAlso copied to: $root\run_log.txt (+ run_log.stderr.txt)"
