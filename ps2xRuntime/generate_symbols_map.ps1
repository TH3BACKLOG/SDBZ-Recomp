# generate_symbols_map.ps1
#
# Generates symbols.map (<hexaddr> <name> per line) for RecompDebugger's
# "Load Symbol Map" feature, by walking the runner-generated filename
# convention Name_0xADDR.cpp under ps2xRuntime/src/runner/.
#
# This is decoupled from the recompiler's internal function-table format on
# purpose -- it only depends on the stable output filename convention, so it
# keeps working even if the dispatch-table format changes again later. Run
# this from the repo root (or anywhere; it locates src/runner relative to
# this script's own location) and copy the resulting symbols.map next to
# RecompDebugger.exe, then use its Settings/Symbols tab to load it.
#
# Restored/added 2026-07-14 alongside the RecompDebugger revival.

$ErrorActionPreference = 'Stop'

$runnerDir = Join-Path $PSScriptRoot 'src\runner'
if (-not (Test-Path $runnerDir)) {
    Write-Error "runner directory not found at $runnerDir"
    exit 1
}

$outFile = Join-Path $PSScriptRoot 'symbols.map'
$pattern = '^(.+)_0x([0-9a-fA-F]+)\.cpp$'

$writer = [System.IO.StreamWriter]::new($outFile, $false)
try {
    $count = 0
    Get-ChildItem -LiteralPath $runnerDir -Filter '*.cpp' -File | ForEach-Object {
        if ($_.Name -match $pattern) {
            $name = $Matches[1]
            $addr = $Matches[2].ToLowerInvariant()
            $writer.WriteLine("$addr $name")
            $count++
        }
    }
    Write-Host "Wrote $count symbols to $outFile"
} finally {
    $writer.Dispose()
}
