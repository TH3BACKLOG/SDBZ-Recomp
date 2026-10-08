# verify_fix.ps1 -- one command, unattended: is this fix good? Chains the existing tools, stops at the first FAIL.
#
#   .\build_scripts\verify_fix.ps1 -Name vsqrt -Expect '[sqrtabs] 85 sqrt.s/vsqrt/vrsqrt bodies overridden'
#   .\build_scripts\verify_fix.ps1 -Name x -Gen -Sweep build_scripts\sweeps\krillin.txt   # + regen overrides, + a sweep
#
# Steps (each PASS/FAIL + time -> Logs\verify\<Name>\summary.md):
#   gen          conformance\gen_sqrt_abs_overrides.py            (-Gen only)
#   conformance  conformance\conformance.ps1 -GameFixes            FAIL classes must drop vs report.md, no new FAIL
#                                                                  (-NoConformance for fixes the opcode test cannot see)
#   build        build.ps1 RelWithDebInfo                          exit 0
#   boot         launch_recomp.ps1, -BootSeconds (same flags as gfx_tour.ps1)
#                every -Expect line present, '[run] exiting loop', no exception/FATAL/missing-target
#   sweep        gfx_tour.ps1 -Script <Sweep>                      BROKEN=0 in its report.md (-Sweep only)
param(
    [Parameter(Mandatory)][string]$Name,
    [string[]]$Expect = @(),
    [switch]$Gen,
    [switch]$NoConformance,
    [int]$BootSeconds = 120,
    [string]$Sweep,
    [int]$SweepSeconds = 2400
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root "Logs\verify\$Name"
New-Item -ItemType Directory -Force $dir | Out-Null
$summary = Join-Path $dir 'summary.md'
$rows = [System.Collections.Generic.List[string]]::new()
$t00 = Get-Date

function Write-Summary([string]$verdict) {
    $mins = '{0:N0}' -f ((Get-Date) - $t00).TotalMinutes
    $lines = @("# verify_fix: $Name", '', "- started $($t00.ToString('MM-dd HH:mm')), total $mins min, **$verdict**", '',
        '| step | result | time | detail |', '|---|---|---|---|') + $rows
    Set-Content -Path $summary -Value $lines -Encoding utf8
    ''; $lines | ForEach-Object { $_ }; ''; "summary -> $summary"
}

# Runs one step; the block returns a detail string (PASS) or throws (FAIL). On FAIL: summary + exit 1.
function Step([string]$step, [scriptblock]$body) {
    Write-Host "== $step" -ForegroundColor Cyan
    $t0 = Get-Date
    try {
        $detail = & $body
        $secs = '{0:N0}' -f ((Get-Date) - $t0).TotalSeconds
        $rows.Add("| $step | PASS | $secs s | $detail |")
    } catch {
        $secs = '{0:N0}' -f ((Get-Date) - $t0).TotalSeconds
        $rows.Add("| $step | **FAIL** | $secs s | $($_.Exception.Message -replace '\|', '/') |")
        Write-Summary 'FAIL'
        exit 1
    }
}

function Read-ReportRows([string]$path) {
    $m = @{}
    foreach ($l in Get-Content $path) {
        if ($l -match '^\| ([^|]+?) \| (\d+) \| (\w+) \| (\w+) \|') { $m[$Matches[1]] = $Matches[3] }
    }
    $m
}

if ($Gen) {
    Step 'gen' {
        $o = python (Join-Path $root 'build_scripts\conformance\gen_sqrt_abs_overrides.py') 2>&1
        if ($LASTEXITCODE -ne 0) { throw "gen_sqrt_abs_overrides.py: $(($o | Select-Object -Last 2) -join ' ')" }
        ($o | Select-Object -First 1)
    }
}

if (-not $NoConformance) {
    Step 'conformance' {
        $conf = Join-Path $root 'build_scripts\conformance'
        $o = & (Join-Path $conf 'conformance.ps1') -GameFixes 2>&1
        $o | Select-Object -Last 6 | ForEach-Object { "   $_" } | Write-Host
        $base = Read-ReportRows (Join-Path $conf 'out\report.md')
        $fixed = Read-ReportRows (Join-Path $conf 'out\report_fixed.md')
        if ($fixed.Count -eq 0) { throw 'report_fixed.md has no class rows' }
        $nb = @($base.Keys | Where-Object { $base[$_] -eq 'FAIL' }).Count
        $nf = @($fixed.Keys | Where-Object { $fixed[$_] -eq 'FAIL' }).Count
        $new = @($fixed.Keys | Where-Object { $fixed[$_] -eq 'FAIL' -and $base[$_] -ne 'FAIL' })
        $moved = @($base.Keys | Where-Object { $base[$_] -eq 'FAIL' -and $fixed[$_] -ne 'FAIL' } | Sort-Object | ForEach-Object { "$_ FAIL->$($fixed[$_])" })
        if ($new.Count) { throw "new FAIL: $($new -join ', ')" }
        if ($nf -ge $nb) { throw "FAIL classes not reduced ($nb -> $nf)" }
        "FAIL classes $nb -> $nf; $($moved -join ', ')"
    }
}

Step 'build' {
    $o = & (Join-Path $root 'build.ps1') RelWithDebInfo 2>&1
    $code = $LASTEXITCODE
    $o | Select-Object -Last 3 | ForEach-Object { "   $_" } | Write-Host
    if ($code -ne 0) {
        $err = Join-Path $root 'Logs\build\build_errors.txt'
        $first = if (Test-Path $err) { (Get-Content $err | Select-Object -First 2) -join ' ' } else { '' }
        throw "build.ps1 exit $code $first"
    }
    'RelWithDebInfo OK'
}

Step 'boot' {
    $log = Join-Path $dir 'run_log.txt'
    & (Join-Path $root 'launch_recomp.ps1') -Determinism 1 -RunSeconds $BootSeconds -NoDebugger -NoArchive -Log $log `
        -Exe (Join-Path $root 'build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe') | Select-Object -Last 3 | Out-Null
    if (-not (Test-Path $log)) { throw "no run log at $log" }
    $text = [System.IO.File]::ReadAllText($log)   # BOM-aware: the log is UTF-16
    $missing = @($Expect | Where-Object { -not $text.Contains($_) })
    if ($missing.Count) { throw "expected line(s) not in the log: $($missing -join ' ; ')" }
    if (-not $text.Contains('[run] exiting loop')) { throw "no '[run] exiting loop' (crash or hang?)" }
    $bad = [regex]::Matches($text, '(?i)unhandled exception|FATAL|missing-target')
    if ($bad.Count) { throw "$($bad.Count) bad line(s), first: $($bad[0].Value)" }
    "$BootSeconds s boot clean; $($Expect.Count) expected line(s) found"
}

if ($Sweep) {
    Step 'sweep' {
        $free = [math]::Floor((Get-PSDrive F).Free / 1GB)
        if ($free -lt 15) { throw "F: has $free GB free (< 15): sweep skipped" }
        $sweepName = "verify_$Name"
        & (Join-Path $root 'build_scripts\gfx_tour.ps1') -Name $sweepName -Script $Sweep -Seconds $SweepSeconds -Every 90 2>&1 |
            Select-Object -Last 4 | ForEach-Object { "   $_" } | Write-Host
        $rep = Join-Path $root "gsdump\$sweepName\report.md"
        if (-not (Test-Path $rep)) { throw "no $rep" }
        $line = Select-String -Path $rep -Pattern '\*\*BROKEN: (\d+)\*\*' | Select-Object -First 1
        if (-not $line) { throw "no BROKEN line in $rep" }
        $n = [int]$line.Matches[0].Groups[1].Value
        if ($n -ne 0) { throw "BROKEN=$n (see $rep)" }
        "BROKEN=0 ($rep)"
    }
}

Write-Summary 'PASS'
