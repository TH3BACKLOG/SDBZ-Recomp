# analyze_slotwatch.ps1
# Parses run_log.txt for the rpc_call $ra-clobber probe (SLOTWATCH / RAFORK /
# IMBAL / TABLE) and prints a verdict instead of raw lines.
#
# The key question: which wrapped frame flips the watched slot from
# expected(clean) -> now=0x1(dirty)? That frame brackets the writer.
#
#   before == expected  &&  now == 0x1   -> stomp happened INSIDE this subtree
#   before == 0x1        &&  now == 0x1   -> entered already-dirty (below writer)
#
# The DEEPEST frame in the first group names the writer's bracket. Given the
# known chain 177fe8 -> 177eb0 -> ... -> 178068, deepest = latest-entered.
#
# NOTE ON LINE SPLITTING: the probe emits a record via chained std::cerr <<,
# and the concurrent "guest PC 0x1" watchdog writes the same stream, so a
# newline can land mid-record and push before=/expected=/now= onto the NEXT
# physical line. We pre-join any continuation line (one that does NOT start a
# new probe record) onto its preceding record before parsing. This was the
# real cause of the old false "STALE LOG" verdict.
#
# Usage:  .\build_scripts\analyze_slotwatch.ps1
#         .\build_scripts\analyze_slotwatch.ps1 -Path some_other_log.txt

param(
    [string]$Path = "run_log.txt"
)

if (-not (Test-Path $Path)) {
    Write-Host "No log at $Path" -ForegroundColor Red
    exit 1
}

# A record begins with one of these tags. Anything else on a following line is a
# continuation split off by interleaved watchdog output.
$recordTag = '\[frametrace:(SLOTWATCH2?|SLOTENTRY|RAFORK|IMBAL|TABLE)\]'

# Log is UTF-16 (tee'd by launch_recomp.ps1). Pull the probe lines PLUS the line
# immediately after each (Context 0,1) so we can re-attach split continuations.
$raw = Select-String -Path $Path -Encoding unicode -Pattern $recordTag -Context 0,1

# Rebuild whole records: each match's own line, with its following line appended
# only when that following line is NOT itself a new record (i.e. it's a split
# continuation). The watchdog spam line ("guest PC 0x1") is not a record tag, so
# a genuine one-line record simply gets its (unrelated) next line ignored.
$records = foreach ($m in $raw) {
    $line = $m.Line
    $next = $m.Context.PostContext | Select-Object -First 1
    if ($next -and ($next -notmatch $recordTag) -and
        ($line -notmatch 'now=0x' -or $line -notmatch 'before=0x')) {
        # Record looks incomplete (missing the tail fields) -> stitch the
        # continuation on. Guard so we never swallow the next real record.
        "$line $next"
    } else {
        $line
    }
}

if (-not $records) {
    Write-Host "No probe lines found. Did the build pick up the edits? Is the log fresh?" -ForegroundColor Yellow
    exit 0
}

function Get-Hex($line, $key) {
    if ($line -match "$key=0x([0-9a-fA-F]+)") { return "0x$($matches[1])" }
    return $null
}

# The dump ordinal is printed as "#N " (no '='), e.g. "[frametrace:SLOTWATCH] #3 ".
function Get-Ordinal($line) {
    if ($line -match '\]\s*#([0-9]+)') { return [int]$matches[1] }
    return $null
}

# Chain depth: position of a callee in the known nested call chain
# (outer -> inner). Higher index = deeper. Unknown callees -> -1.
$chainOrder = @('0x178428','0x17ed60','0x17edb0','0x177fe8','0x177eb0','0x1781b0','0x175060','0x178068')
function Get-Depth($callee) {
    $i = [array]::IndexOf($chainOrder, $callee)
    return $i
}

$slotwatch = @()
$slotentry = @()
$rafork    = @()

foreach ($l in $records) {
    if ($l -match '\[frametrace:SLOTWATCH\]') {
        $slotwatch += [pscustomobject]@{
            n        = Get-Ordinal $l          # real dump ordinal the probe printed
            callee   = Get-Hex $l 'callee'
            entrySp  = Get-Hex $l 'entrySp'
            slot     = Get-Hex $l 'slot'
            expected = Get-Hex $l 'expected'
            before   = Get-Hex $l 'before'
            now      = Get-Hex $l 'now'
            raw      = $l
        }
    }
    elseif ($l -match '\[frametrace:SLOTENTRY\]') {
        $slotentry += [pscustomobject]@{
            n        = Get-Ordinal $l          # entry order — this tag fires AT entry, so ascending n = chronological
            callee   = Get-Hex $l 'callee'
            entryPc  = Get-Hex $l 'entryPc'
            entrySp  = Get-Hex $l 'entrySp'
            expected = Get-Hex $l 'expected'
            before   = Get-Hex $l 'before'
            raw      = $l
        }
    }
    elseif ($l -match '\[frametrace:RAFORK\]') {
        $rafork += $l
    }
}

Write-Host "=== RAFORK (rpc_call entry vs exit) ===" -ForegroundColor Cyan
if ($rafork) { $rafork | ForEach-Object { Write-Host "  $_" } }
else         { Write-Host "  (none - rpc_call did not derail this run)" }

Write-Host ""
Write-Host "=== SLOTWATCH frames ($($slotwatch.Count)) ===" -ForegroundColor Cyan

# STALE only if the whole log carries no before= anywhere (never per-line).
$hasBefore = $slotwatch | Where-Object { $_.before }
if (-not $hasBefore) {
    Write-Host "  STALE LOG: no 'before=' field present in ANY record." -ForegroundColor Yellow
    Write-Host "  This log predates the before-read edit. Rebuild + rerun, then re-run me." -ForegroundColor Yellow
    exit 0
}

# Classify each frame. Print order does NOT track chain depth (SLOTWATCH fires
# inner-first at exit), so pick the writer bracket by KNOWN chain depth, not by
# order: the deepest (highest chain index) clean-in/dirty-out frame is the
# tightest bracket.
$writerBracket = $null
$writerDepth   = -2
foreach ($f in $slotwatch) {
    $tag = ""
    $color = "Gray"
    if ($f.now -eq "0x1" -and $f.before -eq $f.expected) {
        $tag = "  <-- CLEAN-IN, DIRTY-OUT (writer in this subtree)"
        $color = "Green"
        $d = Get-Depth $f.callee
        if ($d -ge $writerDepth) { $writerDepth = $d; $writerBracket = $f }
    }
    elseif ($f.now -eq "0x1" -and $f.before -eq "0x1") {
        $tag = "  (entered dirty - below writer, exonerated)"
        $color = "DarkGray"
    }
    Write-Host ("  #{0} callee={1} entrySp={2} before={3} now={4}{5}" -f `
        $f.n, $f.callee, $f.entrySp, $f.before, $f.now, $tag) -ForegroundColor $color
}

Write-Host ""
Write-Host "=== SLOTENTRY (entry-time, chronological — first dirty-at-entry narrows the stomp to its predecessor) ===" -ForegroundColor Cyan
if ($slotentry) {
    # NOTE: 'n' is a per-template-instantiation counter (the static counter lives
    # inside sdbzFrameTraceWrapper<I>, so each wrapped address I gets its OWN
    # sequence, not a shared global one) -- sorting by n interleaves unrelated
    # functions' local counts and is NOT chronological. $slotentry is already in
    # true log/entry order because it was built by iterating $records in file
    # order, so print it as-is.
    $firstDirty = $null
    foreach ($f in $slotentry) {
        $tag = ""
        $color = "Gray"
        if ($f.before -ne $f.expected -and -not $firstDirty) {
            $tag = "  <-- FIRST DIRTY-AT-ENTRY (predecessor in the call chain is the stomp site)"
            $color = "Red"
            $firstDirty = $f
        }
        elseif ($f.before -ne $f.expected) {
            $color = "DarkGray"
        }
        Write-Host ("  #{0} callee={1} entryPc={2} entrySp={3} before={4} expected={5}{6}" -f `
            $f.n, $f.callee, $f.entryPc, $f.entrySp, $f.before, $f.expected, $tag) -ForegroundColor $color
    }
    if ($firstDirty) {
        Write-Host ""
        Write-Host ("  Stomp site is BEFORE {0}'s entry — look at whatever calls it directly." -f $firstDirty.callee) -ForegroundColor Red
    } else {
        Write-Host "  All entries clean — stomp happens after entry, inside one of these bodies (not before any of them)." -ForegroundColor Yellow
    }
} else {
    Write-Host "  (none logged — probe may not have fired, or log predates the SLOTENTRY edit)" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "=== VERDICT ===" -ForegroundColor Cyan
if ($writerBracket) {
    Write-Host ("  Writer bracket (deepest clean-in/dirty-out): {0}" -f $writerBracket.callee) -ForegroundColor Green
    Write-Host "  The clobber occurs inside this frame's body or its UNWRAPPED leaves."
    Write-Host "  Next: wrap that frame's direct callees, or disassemble its body for"
    Write-Host "  a store to the saved-ra slot. (Or confirm via PCSX2 write-watchpoint"
    Write-Host "  on the slot address - deterministic, no rebuild.)"
} else {
    Write-Host "  No clean-in/dirty-out frame found." -ForegroundColor Yellow
    Write-Host "  Either the slot was dirtied before the FIRST wrapped frame (writer is"
    Write-Host "  above 0x178428 in the chain), or arming missed. Widen the wrap set."
}
