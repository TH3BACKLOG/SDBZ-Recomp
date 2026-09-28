<#
.SYNOPSIS
  GS speed benchmark: replays a PCSX2 GS dump (.gsr) through our GS.

.DESCRIPTION
  Runs ps2x_gs_bench.exe on each dump (default: the fight dump) and prints
  the median host time for one full replay. Appends one row per dump to
  logs\gsbench\bench_history.csv so before/after numbers are kept.

  Make a dump from a PCSX2 .gs/.gs.zst:
    python build_scripts/gsdump_parse.py dump.gs.zst --emit-replay gsdump/x.gsr --emit-vram gsdump/x.vram

  -Bmp also writes the framebuffer after the last replay (fbp 0x70, fbw 8, 512x448)
  so a change that breaks drawing is visible.

.EXAMPLE
  & "F:\SDBZ Recomp\build_scripts\gs_bench.ps1" -Build -Label "baseline"
#>
param(
    [string[]]$Dumps = @('gsdump\fight_a16.gsr'),
    [int]$Repeat = 5,
    [string]$Label = '',
    [switch]$Build,
    [switch]$Bmp,
    [string]$Config = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\ps2xTest\$Config\ps2x_gs_bench.exe"
$histDir = Join-Path $root 'logs\gsbench'
$history = Join-Path $histDir 'bench_history.csv'

if ($Build) {
    $vs = & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
    $proj = Join-Path $root 'build\ps2xTest\ps2x_gs_bench.vcxproj'
    cmd /c "call `"$vs\Common7\Tools\VsDevCmd.bat`" -arch=amd64 >nul && `"$vs\MSBuild\Current\Bin\amd64\MSBuild.exe`" `"$proj`" /p:Configuration=$Config /m:2 /v:m /nologo"
    if ($LASTEXITCODE -ne 0) { throw "gs bench build failed ($LASTEXITCODE)" }
}
if (-not (Test-Path $exe)) { throw "missing $exe (use -Build)" }
New-Item -ItemType Directory -Force $histDir | Out-Null

$commit = (git -C $root rev-parse --short HEAD).Trim()
$dirty = if (git -C $root status --porcelain -- ps2xRuntime) { '+dirty' } else { '' }

try {
    foreach ($dump in $Dumps) {
        $path = if ([IO.Path]::IsPathRooted($dump)) { $dump } else { Join-Path $root $dump }
        if (-not (Test-Path $path)) { Write-Warning "skip, missing: $path"; continue }
        if ($Bmp) { $env:PS2X_GSBENCH_BMP = "0x70,8,512,448," + [IO.Path]::ChangeExtension($path, '.bench.bmp') }
        $out = & $exe $path $Repeat 2>&1 | Out-String
        if ($out -notmatch 'bench: gs median ([\d.]+) ms \(min ([\d.]+), max ([\d.]+)\)') { $out; throw "no bench line for $path" }
        $median = [double]$Matches[1]; $min = [double]$Matches[2]; $max = [double]$Matches[3]
        '{0,-24} gs median {1,8:N1} ms  (min {2:N1}, max {3:N1})' -f (Split-Path $path -Leaf), $median, $min, $max
        if ($Bmp) { ($out -split "`n" | Select-String 'bmp') -join '' }

        if (-not (Test-Path $history)) { 'date,commit,label,dump,repeat,median_ms,min_ms,max_ms' | Set-Content $history }
        '{0},{1}{2},{3},{4},{5},{6:F1},{7:F1},{8:F1}' -f (Get-Date -Format s), $commit, $dirty, $Label,
            (Split-Path $path -Leaf), $Repeat, $median, $min, $max | Add-Content $history
    }
}
finally {
    Remove-Item Env:PS2X_GSBENCH_BMP -ErrorAction SilentlyContinue
}
