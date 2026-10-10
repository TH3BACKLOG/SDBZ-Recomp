<#
.SYNOPSIS
  VU1 speed benchmark + correctness gate using PCSX2 .vucap replays.

.DESCRIPTION
  Runs ps2x_vucap_replay.exe over each capture -Repeat times, then prints the
  median time spent inside VU1 execute/resume and whether the gate held
  (runs replayed > 0 and 0 mismatches). Appends one row per capture to
  logs\vucap\bench_history.csv so before/after numbers are kept.

  Needs no game run. Clears PS2X_VUCAP afterwards so a later game run cannot
  record over a capture.

.EXAMPLE
  & "F:\SDBZ Recomp\build_scripts\vu1_bench.ps1" -Build -Label "baseline"
#>
param(
    [string[]]$Captures = @('fight5full.vucap', 'pcsx2_scores_step6.vucap'),
    [int]$Repeat = 5,
    [string]$Label = '',
    [switch]$Build,
    # Also run each capture once with PS2X_VU1_RECOMP=2 (recompiled program and
    # interpreter side by side; full state, memory, packets and cycle counts
    # compared). Any mismatch fails the gate.
    [switch]$Verify,
    [string]$Config = 'RelWithDebInfo',
    # Do not append to logs\vucap\bench_history.csv (sweep captures are a correctness check, not a speed sample).
    [switch]$NoHistory
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$capDir = Join-Path $root 'logs\vucap'
$exe = Join-Path $root "build\ps2xTest\$Config\ps2x_vucap_replay.exe"
$history = Join-Path $capDir 'bench_history.csv'

if ($Build) {
    $vs = & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
    $proj = Join-Path $root 'build\ps2xTest\ps2x_vucap_replay.vcxproj'
    cmd /c "call `"$vs\Common7\Tools\VsDevCmd.bat`" -arch=amd64 >nul && `"$vs\MSBuild\Current\Bin\amd64\MSBuild.exe`" `"$proj`" /p:Configuration=$Config /m:2 /v:m /nologo"
    if ($LASTEXITCODE -ne 0) { throw "replay tool build failed ($LASTEXITCODE)" }
}
if (-not (Test-Path $exe)) { throw "missing $exe (use -Build)" }

$commit = (git -C $root rev-parse --short HEAD).Trim()
$dirty = if (git -C $root status --porcelain -- ps2xRuntime) { '+dirty' } else { '' }
$allPass = $true

try {
    foreach ($cap in $Captures) {
        $capPath = if ([IO.Path]::IsPathRooted($cap)) { $cap } else { Join-Path $capDir $cap }
        if (-not (Test-Path $capPath)) { Write-Warning "skip, missing: $capPath"; $allPass = $false; continue }
        $report = Join-Path $env:TEMP ("vu1bench_" + [IO.Path]::GetFileNameWithoutExtension($capPath) + '.txt')

        $times = @(); $runs = 0; $mismatch = $true; $nsPerCycle = 0.0; $cycles = 0
        for ($i = 1; $i -le $Repeat; $i++) {
            $env:PS2X_VUCAP = $capPath
            $env:PS2X_VUCAP_OUT = $report
            & $exe *> $null
            Remove-Item Env:PS2X_VUCAP, Env:PS2X_VUCAP_OUT -ErrorAction SilentlyContinue

            $text = Get-Content $report -Raw
            if ($text -match 'runs replayed (\d+)') { $runs = [int]$Matches[1] }
            $mismatch = -not ($text -match '0 mismatches')
            if ($text -match 'bench: vu1 ([\d.]+) ms, (\d+) vu cycles, ([\d.]+) ns/cycle') {
                $times += [double]$Matches[1]; $cycles = [uint64]$Matches[2]; $nsPerCycle = [double]$Matches[3]
            }
            else { throw "no bench line in $report (old replay exe? use -Build)" }
        }

        $sorted = $times | Sort-Object
        $median = $sorted[[int][math]::Floor($sorted.Count / 2)]
        $pass = ($runs -gt 0) -and (-not $mismatch)
        if (-not $pass) { $allPass = $false }
        $ns = if ($cycles) { $median * 1e6 / $cycles } else { 0 }

        '{0,-28} vu1 median {1,8:N1} ms  (min {2:N1}, max {3:N1})  {4:N2} ns/cycle  runs {5}  gate {6}' -f `
            (Split-Path $capPath -Leaf), $median, $sorted[0], $sorted[-1], $ns, $runs, $(if ($pass) { 'PASS' } else { 'FAIL' })
        if ($mismatch) { Get-Content $report | Select-Object -Last 15 }

        if ($Verify) {
            $env:PS2X_VUCAP = $capPath
            $env:PS2X_VUCAP_OUT = $report
            $env:PS2X_VU1_RECOMP = '2'
            $vout = & $exe 2>&1 | ForEach-Object { "$_" }
            Remove-Item Env:PS2X_VUCAP, Env:PS2X_VUCAP_OUT, Env:PS2X_VU1_RECOMP -ErrorAction SilentlyContinue
            $mis = @($vout | Where-Object { $_ -match '\[vu1recomp\] MISMATCH' })
            $ran = @($vout | Where-Object { $_ -match '\[vu1recomp\] verify: [0-9]+ runs' })
            $vpass = ($mis.Count -eq 0) -and ($ran.Count -gt 0)
            $detail = if ($ran.Count -eq 0) { 'never ran: no compiled program hit' } else { $ran[-1] -replace '.*verify: ', '' }
            '{0,-28} recomp verify {1}  ({2})' -f (Split-Path $capPath -Leaf), $(if ($vpass) { 'PASS' } else { 'FAIL' }), $detail
            $mis | Select-Object -First 5
            if (-not $vpass) { $allPass = $false; $pass = $false }
        }

        if ($NoHistory) { continue }
        if (-not (Test-Path $history)) { 'date,commit,label,capture,repeat,median_ms,min_ms,max_ms,ns_per_cycle,runs,gate' | Set-Content $history }
        '{0},{1}{2},{3},{4},{5},{6:F1},{7:F1},{8:F1},{9:F3},{10},{11}' -f (Get-Date -Format s), $commit, $dirty, $Label,
            (Split-Path $capPath -Leaf), $Repeat, $median, $sorted[0], $sorted[-1], $ns, $runs, $(if ($pass) { 'PASS' } else { 'FAIL' }) |
            Add-Content $history
    }
}
finally {
    Remove-Item Env:PS2X_VUCAP, Env:PS2X_VUCAP_OUT -ErrorAction SilentlyContinue
}

if (-not $allPass) { Write-Host 'GATE FAILED' -ForegroundColor Red; exit 1 }
Write-Host 'gate PASS' -ForegroundColor Green
