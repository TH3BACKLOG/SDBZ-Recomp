<#
.SYNOPSIS
  Line-level CPU profile of the GS bench (fight dump) with the in-process sampler.

.DESCRIPTION
  Runs ps2x_gs_bench.exe with PS2X_PROFILE on and prints, per raster worker /
  GS thread, the hot functions and (for functions matching -Lines) the hot
  source lines. Use -Threads 1 for a clean worker profile (no spin noise).

.EXAMPLE
  pwsh -NoProfile -File build_scripts\perf\bench_prof.ps1 -Threads 1 -Lines gsmt -Out logs\p7_prof.txt
#>
param(
    [int]$Threads = 1,
    [int]$Repeat = 40,
    [string]$Lines = 'gsmt',
    [string]$Dump = 'gsdump\fight_a16.gsr',
    [string]$Out = 'logs\bench_prof.txt',
    [string]$Config = 'RelWithDebInfo',
    [switch]$Nearest   # PS2X_GS_NEAREST=1, what interactive play runs
)
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = Join-Path $root "build\ps2xTest\$Config\ps2x_gs_bench.exe"
$env:PS2X_PROFILE = '1'; $env:PS2X_PROFILE_SECS = '9999'; $env:PS2X_PROFILE_MS = '1'
if ($Nearest) { $env:PS2X_GS_NEAREST = '1' }
$env:PS2X_PROFILE_LINES = $Lines; $env:PS2X_GS_RASTER_THREADS = "$Threads"
try {
    $o = & $exe (Join-Path $root $Dump) $Repeat 2>&1 | Out-String
    $outPath = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
    [IO.File]::WriteAllText($outPath, $o)
    Write-Host "wrote $outPath"
} finally {
    foreach ($n in 'PS2X_PROFILE','PS2X_PROFILE_SECS','PS2X_PROFILE_MS','PS2X_PROFILE_LINES','PS2X_GS_RASTER_THREADS','PS2X_GS_NEAREST') {
        Set-Item -Path "Env:$n" -Value $null -ErrorAction SilentlyContinue
    }
}
