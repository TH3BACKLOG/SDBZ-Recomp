# arkd_grep.ps1 - grep the UTF-16 run_log.txt for the IOP/ARKD signatures we
# read every disco run, without re-deriving the Get-Content encoding + filter
# each time. Companion to launch_recomp.ps1 (which tees run_log.txt as UTF-16).
#
#   .\arkd_grep.ps1                  # all [ARKD:/[iop: lines (the default view)
#   .\arkd_grep.ps1 -Imports         # DISTINCT [iop:import] signatures (lib/fid/args)
#   .\arkd_grep.ps1 -Trace           # [iop:trace] blocks (PC windows on halt)
#   .\arkd_grep.ps1 -State           # [ARKD:state] job-state dumps
#   .\arkd_grep.ps1 -Run             # [ARKD:run] service-call result table
#   .\arkd_grep.ps1 -Pattern 'sid=0x503'   # custom regex
#   .\arkd_grep.ps1 -Distinct        # dedupe whatever the chosen filter returns
#   .\arkd_grep.ps1 -Log other.txt   # a different log file
param(
    [string]$Log = "F:\SDBZ Recomp\run_log.txt",
    [switch]$Imports,
    [switch]$Trace,
    [switch]$State,
    [switch]$Run,
    [string]$Pattern,
    [switch]$Distinct
)

if (-not (Test-Path $Log)) { Write-Error "Log not found: $Log"; exit 1 }

# run_log.txt is UTF-16 (PowerShell Tee-Object default). Read it as such so the
# text isn't full of interleaved NULs when grepped.
$lines = Get-Content -LiteralPath $Log -Encoding Unicode

# Pick the filter regex from the first switch that is set.
$rx =
    if     ($Pattern) { $Pattern }
    elseif ($Imports) { '\[iop:import\]' }
    elseif ($Trace)   { '\[iop:trace\]' }
    elseif ($State)   { '\[ARKD:state\]' }
    elseif ($Run)     { '\[ARKD:run\]' }
    else              { '\[ARKD:|\[iop:' }

$hits = $lines | Select-String -Pattern $rx -Raw

# For -Imports, collapse to distinct signatures: drop the volatile a0-a3/ra hex
# so repeated calls to the same import with different args fold to one line.
if ($Imports -and -not $Pattern) {
    $hits = $hits | ForEach-Object { ($_ -replace 'a[0-3]=[0-9a-fA-F]+', '' -replace 'ra=[0-9a-fA-F]+', '').TrimEnd() }
}

if ($Distinct) { $hits = $hits | Sort-Object -Unique }

$hits
Write-Host "[arkd_grep] $($hits.Count) line(s) matching '$rx'" -ForegroundColor DarkGray
