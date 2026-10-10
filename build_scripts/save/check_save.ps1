<#
.SYNOPSIS
  Boot the game on the CURRENT memory card (no pokes) and report what the save unlocked.
.DESCRIPTION
  Runs build_scripts\sweeps\save_check.txt (Original char-select grid walk, then Customize), then
  prints the distinct p1 ids the cursor reached ([scene] lines) and the save_check log markers.
  18 distinct non-zero ids = every fighter selectable.
  pwsh -NoProfile -File build_scripts\save\check_save.ps1            (about 6-7 minutes)
#>
param(
    [string]$Exe = "F:\SDBZ Recomp\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe",
    [int]$RunSeconds = 420
)
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
foreach ($v in 'PS2X_PAD_AUTOPRESS','PS2X_PAD_AUTOPRESS_BTNS','PS2X_PAD_AUTOPRESS_HOLD','PS2X_PAD_AUTOPRESS_SECS','PS2X_VUCAP','PS2X_GSCAP','PS2X_HWWATCH','PS2X_TEXMISS_LOG') {
    Remove-Item "Env:$v" -ErrorAction SilentlyContinue
}
$env:PS2X_DIAG = '0'
$env:PS2X_VU1_THREAD = '1'
$env:PS2X_GS_NEAREST = '1'
$env:PS2X_DET_VBLANK_QUANTUM = '3000'
$env:PS2X_PAD_SCRIPT = Join-Path $root 'build_scripts\sweeps\save_check.txt'
& (Join-Path $root 'launch_recomp.ps1') -Exe $Exe -NoDebugger -RunSeconds $RunSeconds | Out-Null
Remove-Item Env:PS2X_PAD_SCRIPT -ErrorAction SilentlyContinue
python -I -c @"
import sys,re
sys.path.insert(0,r'$root\build_scripts')
import analyze_run
t=analyze_run.read_text_any(r'$root\run_log.txt')
ids=sorted({m.group(1) for m in re.finditer(r'\[scene\][^\n]*? p1=(0x[0-9a-fA-F]+)',t)})
print('p1 ids reached:',len(ids),ids)
for l in t.splitlines():
    if 'save_check' in l: print(l.strip()[:160])
apps=sorted({m.group(1) for m in re.finditer(r'app=(\w+)',t)})
print('apps:',apps)
"@
