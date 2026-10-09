<#
.SYNOPSIS
  A/B the GS bench across env settings (min + median of N replays, vram hash).

.EXAMPLE
  pwsh -NoProfile -File build_scripts\perf\bench_ab.ps1 -Var PS2X_GS_PX32 -Values 0,1 -Nearest -Threads 1
#>
param(
    [string]$Var = 'PS2X_GS_PX32',
    [string[]]$Values = @('0','1'),
    [int]$Threads = 1,
    [int]$Repeat = 30,
    [int]$Rounds = 2,
    [switch]$Nearest,
    [string]$Dump = 'gsdump\fight_a16.gsr',
    [string]$Config = 'RelWithDebInfo'
)
$Values = @($Values | ForEach-Object { $_ -split "," })
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = Join-Path $root "build\ps2xTest\$Config\ps2x_gs_bench.exe"
$env:PS2X_GS_RASTER_THREADS = "$Threads"
$env:PS2X_GS_NEAREST = if ($Nearest) { '1' } else { '0' }
try {
    for ($r = 0; $r -lt $Rounds; $r++) {
        foreach ($v in $Values) {
            Set-Item -Path "Env:$Var" -Value $v
            $o = & $exe (Join-Path $root $Dump) $Repeat 2>&1 | Out-String
            $m = [regex]::Match($o, 'gs median ([\d.]+) ms \(min ([\d.]+)').Groups
            $h = [regex]::Match($o, 'vram hash ([0-9a-f]+)').Groups[1].Value
            '{0}={1} threads={2} nearest={3} median={4} min={5} hash={6}' -f $Var, $v, $Threads, [int]$Nearest.IsPresent, $m[1].Value, $m[2].Value, $h
        }
    }
} finally {
    foreach ($n in $Var,'PS2X_GS_RASTER_THREADS','PS2X_GS_NEAREST') { Set-Item -Path "Env:$n" -Value $null -ErrorAction SilentlyContinue }
}
