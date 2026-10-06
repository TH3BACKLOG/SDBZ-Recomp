# One unattended graphics loop: run the game with autopress, capture the GS
# stream every N ticks (PS2X_GSCAP + PS2X_GSCAP_EVERY), then replay every
# capture offline into gsdump\<Name>\frames\sheet.png (gfx_scene_diff.py).
#
#   .\build_scripts\gfx_tour.ps1 -Name menu -Buttons XS -Seconds 330 -From 6600
#   .\build_scripts\gfx_tour.ps1 -Name tour2 -ReplayOnly -Oracle   # grade vs PCSX2, no eyes
#   .\build_scripts\gfx_tour.ps1 -Name krillin -Manual -Seconds 900 -Every 90 -Frames 2 -VuCapEvery 300
#        # you play (no autopress); GS every 90 ticks + 1-frame VU1 captures every 300 ticks
#   .\build_scripts\gfx_tour.ps1 -Name sweep1 -Script build_scripts\sweeps\discover.txt -Seconds 1800
#        # nobody plays: PS2X_PAD_SCRIPT drives the menus, then oracle + audit -> gsdump\sweep1\report.md
#   .\build_scripts\gfx_tour.ps1 -Name krillin -ReplayOnly -Audit  # re-grade old captures, no run
#
# Then, for one capture:
#   python build_scripts\gfx_scene_diff.py gsdump\menu\cap_t7650.gsr --fbp 0x70 --rect 0,320,512,128 --steps
param(
    [string]$Name = 'tour',
    [string]$Buttons = 'X',      # autopress rotation: X, O, S (Start)
    [int]$Seconds = 260,
    [int]$From = 60,             # first capture tick
    [int]$Every = 60,            # ticks between captures
    [int]$Frames = 3,            # ticks recorded per capture
    [switch]$ReplayOnly,         # skip the run, rebuild the sheet from existing captures
    [switch]$Oracle,             # also grade every capture against PCSX2 GSRunner + lint + disc coverage
    [string]$VuCapTicks = '',    # e.g. "7770,8085": also record VU1 (PS2X_VUCAP) at these GS ticks -> <dir>\vu_t<tick>.vucap
    [int]$VuCapEvery = 0,        # or: a VU1 window every N ticks from -From (for a played run, ticks unknown up front)
    [int]$VuCapFrames = 1,       # vsyncs per VU1 window (~20 MB each with full memory)
    [switch]$Manual,             # no autopress and default raster threads: a human plays
    [string]$Script = '',        # PS2X_PAD_SCRIPT file: scripted inputs, no autopress; implies -Oracle -Audit -Dedupe
    [switch]$Audit,              # one report: oracle + BROKEN lint + host-NaN VU1 scan + scenes + coverage -> <dir>\report.md
    [switch]$Dedupe,             # oracle skips GSRunner for frames equal to the last graded one
    [int]$MinFreeGB = 10,        # VU1 capture is skipped when F: has less free space than this
    [switch]$Prune,              # after a graded audit, delete captures the reports do not name (default with -Script)
    [switch]$KeepCaps,           # never prune
    [switch]$NoLedger            # grade every capture even if gsdump\known_good says its uploads + scene were verified
)
$startedAt = Get-Date
$root = 'F:\SDBZ Recomp'
$dir = Join-Path $root "gsdump\$Name"
if ($Script) {
    $Script = (Resolve-Path $Script).Path
    $Oracle = $true; $Audit = $true; $Dedupe = $true
    if ($VuCapEvery -eq 0 -and -not $VuCapTicks) { $VuCapEvery = 600 }
}
if (-not $ReplayOnly) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    Get-ChildItem $dir -File -Recurse | Remove-Item -Confirm:$false
    $set = @{
        PS2X_DIAG = '0'; PS2X_DET_VBLANK_QUANTUM = '3000'
        PS2X_GSCAP = "$From,$Frames,$dir\cap"; PS2X_GSCAP_EVERY = "$Every"
    }
    if ($Script) {
        $set.PS2X_GS_RASTER_THREADS = '0'
        $set.PS2X_PAD_SCRIPT = $Script
    } elseif (-not $Manual) {
        $set.PS2X_GS_RASTER_THREADS = '0'
        $set.PS2X_PAD_AUTOPRESS = '120'; $set.PS2X_PAD_AUTOPRESS_BTNS = $Buttons; $set.PS2X_PAD_AUTOPRESS_HOLD = '20'
    }
    if ($VuCapEvery -gt 0 -and -not $VuCapTicks) {
        # Windows past the end of the run never fire; 400 covers any session length we use.
        $VuCapTicks = ((0..399) | ForEach-Object { $From + $_ * $VuCapEvery }) -join ','
    }
    $freeGB = [math]::Floor((Get-PSDrive F).Free / 1GB)
    if ($VuCapTicks -and $freeGB -lt $MinFreeGB) {
        Write-Warning "F: has $freeGB GB free (< $MinFreeGB): VU1 capture skipped, GS capture only"
        $VuCapTicks = ''
    }
    if ($VuCapTicks) {
        # Tick windows ("t" prefix) line up with the GSCAP ticks; cleared in finally so a
        # later game run cannot record over the capture.
        $set.PS2X_VUCAP = "$dir\vu.vucap"
        $set.PS2X_VUCAP_AT = (($VuCapTicks -split ',') | ForEach-Object { 't' + $_.Trim() }) -join ','
        $set.PS2X_VUCAP_FRAMES = "$VuCapFrames"
    }
    foreach ($k in $set.Keys) { Set-Item "Env:$k" $set[$k] }
    try {
        # The log lands in the capture dir so -Audit can label captures with [scene] lines.
        & "$root\launch_recomp.ps1" -Determinism 1 -RunSeconds $Seconds -NoDebugger -NoArchive -Log "$dir\run_log.txt" `
            -Exe "$root\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe" | Select-Object -Last 3
    } finally {
        foreach ($k in $set.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    }
}
$targs = @("$root\build_scripts\gfx_scene_diff.py", $dir)
# The sheet skips captures the known-good ledger covers (only when the oracle will grade them anyway).
if ($Oracle -and -not $NoLedger) { $targs += @('--ledger', "$root\gsdump\known_good") }
python @targs
if ($Oracle) {
    # No human judges a frame: report.md lists every capture where we differ from PCSX2,
    # grouped by the blamed draw, plus runaway triangles; coverage names unreached screens.
    $oargs = @("$root\build_scripts\gfx_scene_diff.py", $dir, '--oracle')
    if ($Dedupe) { $oargs += '--dedupe' }
    # Known-good ledger: uploads + scene already graded MATCH by an earlier run are not re-graded
    # (rows KNOWN; lint still runs on every capture). -NoLedger forces a full regrade.
    if (-not $NoLedger) { $oargs += @('--ledger', "$root\gsdump\known_good") }
    python @oargs
    if (-not $Audit) {
        python "$root\build_scripts\audit_disc_textures.py" --coverage $dir | Select-Object -Last 25
        Write-Host "report: $dir\oracle\report.md"
    }
}
if ($Audit) {
    python "$root\build_scripts\gfx_scene_diff.py" $dir --audit
    Write-Host "audit: $dir\report.md  (sheet: $dir\frames\sheet.png)"
}
# Disk: a 4800 s sweep is ~5 GB of .gsr/.vram/.vucap, and a full F: froze orig3 mid-run (10-05).
# Once a capture is graded clean, only its report line + sheet frame are worth keeping.
# Prune only after a full grade (oracle + audit) whose report was written by THIS invocation;
# every capture named in report.md / oracle\report.md (BROKEN, DIFF) is kept for follow-up.
$rep = Join-Path $dir 'report.md'
if (($Prune -or $Script) -and -not $KeepCaps -and $Oracle -and $Audit -and
    (Test-Path $rep) -and (Get-Item $rep).LastWriteTime -ge $startedAt -and
    -not (Select-String -Path $rep -Pattern 'oracle: not graded' -Quiet)) {
    $named = @{}
    # report.md names only BROKEN captures. oracle\report.md has a per-capture table listing
    # EVERY capture (MATCH too) plus informational lint lines -- keep only its non-clean rows
    # (orig2 10-05: matching every name there kept all 336 and pruned nothing).
    foreach ($m in [regex]::Matches((Get-Content $rep -Raw), '(cap|vu)_t\d+')) { $named[$m.Value] = $true }
    $orep = Join-Path $dir 'oracle\report.md'
    if (Test-Path $orep) {
        foreach ($line in Get-Content $orep) {
            if ($line -match '^\|\s*((cap|vu)_t\d+)\s*\|\s*(\S+)' -and $Matches[3] -notin @('MATCH', 'SAME', 'KNOWN')) {
                $named[$Matches[1]] = $true
            }
        }
    }
    $gone = Get-ChildItem $dir -File | Where-Object {
        $_.Name -match '^(cap|vu)_t\d+\.(gsr|vram|vucap)$' -and -not $named.ContainsKey($_.BaseName) }
    $bytes = ($gone | Measure-Object Length -Sum).Sum
    $gone | Remove-Item -Confirm:$false
    Write-Host ("pruned {0} clean capture file(s), {1:N1} GB; kept {2} named in the reports (-KeepCaps to keep all)" -f `
        @($gone).Count, ($bytes / 1GB), $named.Count)
}
