# run_ps2x_tests.ps1 -- run every ps2x_tests suite on its own, with a timeout.
#
# Why: a bare ps2x_tests.exe hangs in PS2RuntimeKernel and silently skips the
# suites after it (BUG-031). One process per suite means a hang costs only that
# suite, and every suite gets a result.
#
# Example: .\build_scripts\run_ps2x_tests.ps1                     # Debug exe, 120 s per suite
# Example: .\build_scripts\run_ps2x_tests.ps1 -Config RelWithDebInfo -TimeoutSec 300
#
# Output: Logs\tests\ps2x_tests_<stamp>\<suite>.txt  + summary.txt (one line per suite).
# Note: the filter is a substring match, so suite "Scheduler" also re-runs every
# Scheduler* suite. Its line in the summary covers all of them.
param(
    [string]$Config = "Debug",
    [int]$TimeoutSec = 120
)
$ErrorActionPreference = 'Stop'
$repo  = Split-Path -Parent $PSScriptRoot
$exe   = Join-Path $repo "build\ps2xTest\$Config\ps2x_tests.exe"
if (-not (Test-Path -LiteralPath $exe)) { Write-Error "not found: $exe (build it: .\build.ps1 $Config -Test)"; exit 1 }

$suites = Select-String -Path (Join-Path $repo 'ps2xTest\src\*.cpp') -Pattern 'MiniTest::Case\("([^"]+)"' -AllMatches |
    ForEach-Object { $_.Matches | ForEach-Object { $_.Groups[1].Value } } | Sort-Object -Unique

$outDir = Join-Path $repo ("Logs\tests\ps2x_tests_{0}" -f (Get-Date -Format 'yyyy-MM-dd_HHmm'))
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exeTime = (Get-Item -LiteralPath $exe).LastWriteTime
$summary = New-Object System.Collections.Generic.List[string]
$summary.Add("exe: $exe ($exeTime)  timeout: $TimeoutSec s")
$summary.Add(('{0,-30} {1,-8} {2,6} {3,6} {4,6}' -f 'suite', 'result', 'tests', 'passed', 'secs'))

foreach ($s in $suites) {
    $log = Join-Path $outDir (($s -replace '[^A-Za-z0-9_]', '_') + '.txt')
    $err = "$log.stderr"
    $sw  = [Diagnostics.Stopwatch]::StartNew()
    $p   = Start-Process -FilePath $exe -ArgumentList "`"$s`"" -NoNewWindow -PassThru `
               -RedirectStandardOutput $log -RedirectStandardError $err
    $done = $p.WaitForExit($TimeoutSec * 1000)
    if (-not $done) { try { $p.Kill($true) } catch {} ; $p.WaitForExit() }
    $sw.Stop()
    if ((Test-Path -LiteralPath $err) -and (Get-Item -LiteralPath $err).Length -gt 0) {
        Add-Content -LiteralPath $log -Value "`n--- stderr ---"; Get-Content -LiteralPath $err | Add-Content -LiteralPath $log
    }
    Remove-Item -LiteralPath $err -ErrorAction SilentlyContinue
    $text   = Get-Content -LiteralPath $log -Raw
    $total  = if ($text -match 'Total Tests:\s*(\d+)') { [int]$Matches[1] } else { -1 }
    $passed = if ($text -match 'Passed:\s*(\d+)')      { [int]$Matches[1] } else { -1 }
    $result = if (-not $done) { 'TIMEOUT' }
              elseif ($total -lt 0) { "CRASH($($p.ExitCode))" }
              elseif ($passed -eq $total) { 'PASS' } else { 'FAIL' }
    $line = '{0,-30} {1,-8} {2,6} {3,6} {4,6:N0}' -f $s, $result, $total, $passed, $sw.Elapsed.TotalSeconds
    $summary.Add($line); Write-Host $line
}
$summaryPath = Join-Path $outDir 'summary.txt'
[IO.File]::WriteAllLines($summaryPath, $summary)
Write-Host "`nsummary: $summaryPath"
