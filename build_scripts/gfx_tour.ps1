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
#   .\build_scripts\gfx_tour.ps1 -Name pc_disc -Emu pcsx2 -Script build_scripts\sweeps\discover.txt -Seconds 1800
#        # PCSX2 plays the same script (pcsx2/DebugTools/PadScript.cpp, branch sdbz-tools) and dumps GS + VU1;
#        # the dumps become .gsr/.vram and the same oracle + audit grade OUR GS on them; vu_t*.vucap replay vs PCSX2
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
    [switch]$NoLedger,           # grade every capture even if gsdump\known_good says its uploads + scene were verified
    [switch]$TexLog,             # PS2X_TEXHASH_LOG: hash EVERY texture upload of the run (default with -Script); exact disc coverage
    [switch]$NoTexLog,           # with -Script: do not log uploads (the log hashes every upload, ~100 transfers per fight tick)
    [int]$RasterThreads = 4,     # PS2X_GS_RASTER_THREADS for scripted/autopress runs: 4 = what the game uses (default, ~18 vbl/s in fights),
                                 # 0 = single-thread (8 vbl/s). 10-06 A/B (rt0 vs rt4, fighters2): 217/217 vs 486/486 oracle MATCH, 0 BROKEN.
    [ValidateSet('recomp', 'pcsx2')]
    [string]$Emu = 'recomp',     # pcsx2: the PCSX2 build plays -Script and takes the captures (no recomp run at all)
    [string]$Iso = 'F:\SDBZ Recomp\PCSX2\Games\Super Dragon Ball Z (USA).iso',
    [int]$Ticks = 0,             # -Emu pcsx2: stop at this guest tick (default -Seconds x 60); a script also stops 120 ticks after its end
    [switch]$NoTurbo             # -Emu pcsx2: keep the 60 fps limiter (default: unthrottled)
)
$startedAt = Get-Date
$root = 'F:\SDBZ Recomp'
$dir = Join-Path $root "gsdump\$Name"
if ($Script) {
    $Script = (Resolve-Path $Script).Path
    $Oracle = $true; $Audit = $true; $Dedupe = $true
    if ($VuCapEvery -eq 0 -and -not $VuCapTicks) { $VuCapEvery = 600 }
}
# A 1-frame PCSX2 dump ends on the frame its .png shows.
if ($Emu -eq 'pcsx2' -and -not $PSBoundParameters.ContainsKey('Frames')) { $Frames = 1 }
$pcsx2Exe = 'F:\PCSX2-src\bin\pcsx2-qtx64-avx2.exe'
if (-not $ReplayOnly) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    Get-ChildItem $dir -File -Recurse | Remove-Item -Confirm:$false
    $set = @{
        PS2X_DIAG = '0'; PS2X_DET_VBLANK_QUANTUM = '3000'
        PS2X_GSCAP = "$From,$Frames,$dir\cap"; PS2X_GSCAP_EVERY = "$Every"
    }
    if ($Script) {
        $set.PS2X_GS_RASTER_THREADS = "$RasterThreads"
        $set.PS2X_PAD_SCRIPT = $Script
        $set.PS2X_SNAP_DIR = "$dir\snap"   # script op `snap <name>` -> RAM dumps for build_scripts\ramdiff.py
    } elseif (-not $Manual) {
        $set.PS2X_GS_RASTER_THREADS = "$RasterThreads"
        $set.PS2X_PAD_AUTOPRESS = '120'; $set.PS2X_PAD_AUTOPRESS_BTNS = $Buttons; $set.PS2X_PAD_AUTOPRESS_HOLD = '20'
    }
    if (($Script -or $TexLog) -and -not $NoTexLog -and $Emu -eq 'recomp') {
        # Capture windows see ~5% of uploads; this log sees all of them (FNV-1a 64 per transfer). audit_disc_textures
        # folds it into coverage_hashes.json ("fnv"), then this script deletes the (large) jsonl after a graded audit.
        $set.PS2X_TEXHASH_LOG = "$dir\texhash.jsonl"
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
    if ($Emu -eq 'pcsx2') {
        # PCSX2 VuCapture counts the vsync it starts on as frame 1 (VuCapture.cpp OnVSync: Begin, then
        # --framesLeft), so frames=1 closes the file in the same call with 0 runs (pc_discover2 10-08: 25/25 empty).
        if ($VuCapTicks) { $set.PS2X_VUCAP_FRAMES = "$($VuCapFrames + 1)" }
        $set.PS2X_PCSX2_LOG = "$dir\run_log.txt"   # [scene] lines land where -Audit looks for them
        $set.PS2X_PCSX2_RUN_TICKS = "$(if ($Ticks) { $Ticks } else { $Seconds * 60 })"
        if ($Script) { $set.PS2X_PCSX2_EXIT_ON_END = '1' }
        if (-not $NoTurbo) { $set.PS2X_PCSX2_TURBO = '1' }
    }
    foreach ($k in $set.Keys) { Set-Item "Env:$k" $set[$k] }
    try {
      if ($Emu -eq 'pcsx2') {
        if (Get-Process -Name 'pcsx2-qtx64-avx2' -ErrorAction SilentlyContinue) {
            throw 'PCSX2 is already running: close it first (the sweep needs its own instance)'
        }
        # -batch: PCSX2 exits when PadScript.cpp shuts the VM down (tick limit or script end).
        $wall = [math]::Max(3600, $Seconds * 4)
        $t0 = Get-Date
        $proc = Start-Process -FilePath $pcsx2Exe -ArgumentList @('-batch', '-fastboot', '-nofullscreen', '--', "`"$Iso`"") `
            -WorkingDirectory $dir -PassThru
        if (-not $proc.WaitForExit($wall * 1000)) { Write-Warning "PCSX2 still running after $wall s: killed"; $proc.Kill() }
        $secs = ((Get-Date) - $t0).TotalSeconds
        $stopLine = Select-String -Path "$dir\run_log.txt" -Pattern '^\[pcsx2sweep\] stop tick=(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($stopLine) {
            $endTick = [int]$stopLine.Matches[0].Groups[1].Value
            '{0}  ({1:N0} ticks in {2:N0} s = {3:N1} ticks/s)' -f $stopLine.Line, $endTick, $secs, ($endTick / [math]::Max(1, $secs))
        } else { Write-Warning "no [pcsx2sweep] stop line in $dir\run_log.txt (PCSX2 closed early or the hook did not run)" }
      } else {
        # The log lands in the capture dir so -Audit can label captures with [scene] lines.
        & "$root\launch_recomp.ps1" -Determinism 1 -RunSeconds $Seconds -NoDebugger -NoArchive -Log "$dir\run_log.txt" `
            -Exe "$root\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe" | Select-Object -Last 3
      }
    } finally {
        foreach ($k in $set.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    }
}
if ($Emu -eq 'pcsx2') {
    # PCSX2 .gs[.zst|.xz] -> the .gsr (GIF stream) + .vram (start VRAM) pair a recomp capture has, same cap_t<tick> name.
    $n = 0
    foreach ($gz in Get-ChildItem $dir -File -Filter 'cap_t*.gs*' | Where-Object Name -match '\.gs(\.zst|\.xz)?$') {
        $stem = $gz.Name -replace '\.gs(\.zst|\.xz)?$', ''
        $gsr = Join-Path $dir "$stem.gsr"; $vram = Join-Path $dir "$stem.vram"
        if ((Test-Path $gsr) -and (Get-Item $gsr).LastWriteTime -ge $gz.LastWriteTime) { continue }
        $out = python "$root\build_scripts\gsdump_parse.py" $gz.FullName --emit-replay $gsr --init-state --emit-vram $vram 2>&1
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $gsr)) { Write-Warning "gsdump_parse failed on $($gz.Name): $(($out | Select-Object -Last 2) -join ' ')"; continue }
        $n++
    }
    "converted $n PCSX2 GS dump(s) to .gsr/.vram"
}
$targs = @("$root\build_scripts\gfx_scene_diff.py", $dir)
# The sheet skips captures the known-good ledger covers (only when the oracle will grade them anyway).
if ($Oracle -and -not $NoLedger) { $targs += @('--ledger', "$root\gsdump\known_good") }
python @targs
if ($Emu -eq 'pcsx2') {
    # PCSX2 dumps carry PCSX2's own screenshot: our replay vs that picture, per capture (GSRunner's read-back
    # probe does not fire on real PCSX2 streams, 10-08). Seeded control: PS2X_GSBENCH_SKIP=2000 on
    # gsdump\_pc_selftest turns MATCH into FLAG 'OURS DARKER (missing?)'.
    python "$root\build_scripts\pcsx2_grade.py" $dir
}
if ($Oracle -and $Emu -eq 'recomp') {
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
    # The distinct upload hashes are now in coverage_hashes.json; the raw log can be 100s of MB.
    $tlog = Join-Path $dir 'texhash.jsonl'; $cj = Join-Path $dir 'coverage_hashes.json'
    if ((Test-Path $tlog) -and -not $KeepCaps -and (Test-Path $cj) -and (Get-Item $cj).LastWriteTime -ge $startedAt) {
        Write-Host ("texhash.jsonl {0:N0} MB folded into coverage_hashes.json, deleted" -f ((Get-Item $tlog).Length / 1MB))
        Remove-Item $tlog -Confirm:$false
    }
}
$vuRep = Join-Path $dir 'vu1_replay.txt'
$vuCaps = @(Get-ChildItem $dir -File -Filter 'vu_t*.vucap')
if ($Emu -eq 'pcsx2' -and $vuCaps.Count) {
    # PCSX2's VIF1 input through OUR VU1, compared with PCSX2's own XGKICK output (vu1_bench gate).
    # A window with 0 VU1 runs (2D screens: warning, logos, movie) is EMPTY, not a failure -- vu1_bench
    # gates it FAIL because "runs replayed 0" proves nothing; only runs>0 + mismatch is a real FAIL.
    $vuOut = & "$root\build_scripts\vu1_bench.ps1" -Captures $vuCaps.FullName -Repeat 1 -NoHistory *>&1 | ForEach-Object { "$_" }
    $vuOut | Set-Content -Path $vuRep
    $vuLines = @($vuOut | Where-Object { $_ -match '^vu_t\d+\.vucap\s' })
    $vuEmpty = @($vuLines | Where-Object { $_ -match '\sruns 0\s' }).Count
    $vuFail = @($vuLines | Where-Object { $_ -match 'gate FAIL' -and $_ -notmatch '\sruns 0\s' })
    $vuPass = @($vuLines | Where-Object { $_ -match 'gate PASS' }).Count
    "vu1 replay: PASS {0}  FAIL {1}  EMPTY {2} (no VU1 work in the window) -> {3}" -f $vuPass, $vuFail.Count, $vuEmpty, $vuRep
    $vuFail | ForEach-Object { "  $_" }
    if ($vuPass + $vuFail.Count -eq 0) {
        # Menus and select screens do run VU1 (pilot menu.vucap: 18k+ runs), so all-empty = broken capture.
        Write-Warning "every VU1 window is EMPTY: the capture recorded nothing, this is not a pass"
    }
    $global:LASTEXITCODE = [int]($vuFail.Count -gt 0)  # vu1_bench exits 1 on EMPTY windows too
}
# Disk: a 4800 s sweep is ~5 GB of .gsr/.vram/.vucap, and a full F: froze orig3 mid-run (10-05).
# Once a capture is graded clean, only its report line + sheet frame are worth keeping.
# Prune only after a full grade (oracle + audit) whose report was written by THIS invocation;
# every capture named in report.md / oracle\report.md (BROKEN, DIFF) is kept for follow-up.
$rep = Join-Path $dir 'report.md'
$pcRep = Join-Path $dir 'pcsx2_report.md'
if ($Emu -eq 'pcsx2' -and -not $KeepCaps -and (Test-Path $pcRep) -and (Get-Item $pcRep).LastWriteTime -ge $startedAt) {
    # Keep every capture the reports name (FLAG/ERROR here, BROKEN in report.md, VU1 gate FAIL); drop the rest.
    $keep = @{}
    foreach ($line in Get-Content $pcRep) { if ($line -match '^\|\s*(cap_t\d+)\s*\|.*\|\s*(FLAG|ERROR)\s*\|') { $keep[$Matches[1]] = $true } }
    if (Test-Path $rep) { foreach ($m in [regex]::Matches((Get-Content $rep -Raw), '(cap|vu)_t\d+')) { $keep[$m.Value] = $true } }
    if (Test-Path $vuRep) { foreach ($line in Get-Content $vuRep) { if ($line -match '^(vu_t\d+)\.vucap\s.*runs [1-9]\d*\s+gate FAIL') { $keep[$Matches[1]] = $true } } }
    $gone = Get-ChildItem $dir -File | Where-Object {
        $_.Name -match '^((cap|vu)_t\d+)\.(gsr|vram|vucap|gs|gs\.zst|gs\.xz|png)$' -and -not $keep.ContainsKey($Matches[1]) }
    $bytes = ($gone | Measure-Object Length -Sum).Sum
    $gone | Remove-Item -Confirm:$false
    Write-Host ("pruned {0} clean PCSX2 capture file(s), {1:N1} GB; kept {2} named in the reports (-KeepCaps to keep all)" -f `
        @($gone).Count, ($bytes / 1GB), $keep.Count)
}
if (($Prune -or $Script) -and -not $KeepCaps -and $Oracle -and $Audit -and $Emu -eq 'recomp' -and
    (Test-Path $rep) -and (Get-Item $rep).LastWriteTime -ge $startedAt -and
    -not (Select-String -Path $rep -Pattern 'oracle: not graded' -Quiet)) {
    $named = @{}
    # report.md names only BROKEN captures. oracle\report.md has a per-capture table listing
    # EVERY capture (MATCH too) plus informational lint lines -- keep only its non-clean rows
    # (orig2 10-05: matching every name there kept all 336 and pruned nothing).
    foreach ($m in [regex]::Matches((Get-Content $rep -Raw), '(cap|vu)_t\d+')) { $named[$m.Value] = $true }
    if (Test-Path $vuRep) {
        foreach ($line in Get-Content $vuRep) { if ($line -match '^(vu_t\d+)\.vucap\s.*runs [1-9]\d*\s+gate FAIL') { $named[$Matches[1]] = $true } }
    }
    $orep = Join-Path $dir 'oracle\report.md'
    if (Test-Path $orep) {
        foreach ($line in Get-Content $orep) {
            if ($line -match '^\|\s*((cap|vu)_t\d+)\s*\|\s*(\S+)' -and $Matches[3] -notin @('MATCH', 'SAME', 'KNOWN')) {
                $named[$Matches[1]] = $true
            }
        }
    }
    $gone = Get-ChildItem $dir -File | Where-Object {
        $_.Name -match '^((cap|vu)_t\d+)\.(gsr|vram|vucap|gs|gs\.zst|gs\.xz|png)$' -and -not $named.ContainsKey($Matches[1]) }
    $bytes = ($gone | Measure-Object Length -Sum).Sum
    $gone | Remove-Item -Confirm:$false
    Write-Host ("pruned {0} clean capture file(s), {1:N1} GB; kept {2} named in the reports (-KeepCaps to keep all)" -f `
        @($gone).Count, ($bytes / 1GB), $named.Count)
}
