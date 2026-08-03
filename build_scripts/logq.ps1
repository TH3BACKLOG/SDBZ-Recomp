# logq.ps1 -- query run_log.txt without the UTF-16 false-negative class.
#
# WHY THIS EXISTS
# ---------------
# launch_recomp.ps1 tees run_log.txt through Tee-Object, which writes UTF-16.
# Plain grep / rg / Select-String-without--Encoding therefore return ZERO HITS
# on a log that is full of matches, silently and with exit code 0. That has
# been read as evidence in this project more than once -- "no [rpcValid] lines
# in the log" and "streaming stopped" both came from a query that never
# actually searched the file. A zero from this script means zero; a zero from
# rg means nothing at all.
#
# It also auto-detects, rather than assuming: if a future launcher switches to
# UTF-8, -Encoding Unicode would break the same way in the other direction.
#
#   .\build_scripts\logq.ps1 -Census                 # tag histogram -- START HERE
#   .\build_scripts\logq.ps1 -Tag watch              # all [watch] lines
#   .\build_scripts\logq.ps1 -Tag coverage -Last 20  # last 20 [coverage] lines
#   .\build_scripts\logq.ps1 -Pattern 'tme=1'        # arbitrary regex
#   .\build_scripts\logq.ps1 -Pattern 'tbp' -Distinct
#   .\build_scripts\logq.ps1 -Census -Log other.txt
param(
    [string]$Log = "F:\SDBZ Recomp\run_log.txt",
    [string]$Pattern,
    [string]$Tag,
    [switch]$Census,
    [int]$Last = 0,
    [switch]$Distinct,
    [switch]$Count
)

if (-not (Test-Path -LiteralPath $Log)) { Write-Error "Log not found: $Log"; exit 1 }

# Detect the encoding from the BOM instead of hardcoding it. A UTF-16LE BOM is
# FF FE; UTF-8 with BOM is EF BB BF; anything else is read as UTF-8. Getting
# this wrong is the entire bug this script exists to prevent, so it is measured,
# not assumed.
$bom = Get-Content -LiteralPath $Log -AsByteStream -TotalCount 3 -ErrorAction Stop
$enc = if ($bom.Count -ge 2 -and $bom[0] -eq 0xFF -and $bom[1] -eq 0xFE) { 'Unicode' }
       elseif ($bom.Count -ge 2 -and $bom[0] -eq 0xFE -and $bom[1] -eq 0xFF) { 'BigEndianUnicode' }
       else { 'UTF8' }

$lines = Get-Content -LiteralPath $Log -Encoding $enc
$sizeMb = [math]::Round((Get-Item -LiteralPath $Log).Length / 1MB, 1)
Write-Host "[logq] $Log  ${sizeMb}MB  encoding=$enc  $($lines.Count) lines" -ForegroundColor DarkGray

if ($lines.Count -eq 0) {
    Write-Host "[logq] file decoded to ZERO lines -- encoding detection failed, do not treat this as 'no output'." -ForegroundColor Red
    exit 1
}

# -Census: what tags does this log actually contain, and how many of each.
# This is the query to run FIRST. Most wrong conclusions in this project came
# from grepping for a tag that the build never emitted, which is
# indistinguishable from "the condition never happened" unless you look at the
# whole tag set.
if ($Census) {
    $hist = @{}
    foreach ($l in $lines) {
        # Require a non-digit: bare [0]/[12] are array indices inside payload
        # lines, not tags, and 16 of them drown the real histogram.
        foreach ($m in [regex]::Matches($l, '\[([A-Za-z0-9_:.\-]*[A-Za-z][A-Za-z0-9_:.\-]*)\]')) {
            $t = $m.Groups[1].Value
            $hist[$t] = 1 + $(if ($hist.ContainsKey($t)) { $hist[$t] } else { 0 })
        }
    }
    if ($hist.Count -eq 0) { Write-Host "[logq] no [tag] lines found." -ForegroundColor Yellow; exit 0 }
    Write-Host "[logq] $($hist.Count) distinct tags:" -ForegroundColor Cyan
    $hist.GetEnumerator() | Sort-Object -Property Value -Descending |
        ForEach-Object { "{0,10}  {1}" -f $_.Value, $_.Key }
    exit 0
}

$rx = if ($Pattern) { $Pattern }
      elseif ($Tag)  { '\[' + [regex]::Escape($Tag) }
      else { Write-Error "Give -Pattern, -Tag, or -Census."; exit 1 }

$hits = @($lines | Select-String -Pattern $rx -Raw)

if ($Distinct) { $hits = @($hits | Sort-Object -Unique) }
if ($Last -gt 0 -and $hits.Count -gt $Last) { $hits = @($hits | Select-Object -Last $Last) }

if (-not $Count) { $hits }
Write-Host "[logq] $($hits.Count) line(s) matching '$rx'" -ForegroundColor DarkGray
