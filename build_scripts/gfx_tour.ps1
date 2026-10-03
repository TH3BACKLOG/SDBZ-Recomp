# One unattended graphics loop: run the game with autopress, capture the GS
# stream every N ticks (PS2X_GSCAP + PS2X_GSCAP_EVERY), then replay every
# capture offline into gsdump\<Name>\frames\sheet.png (gfx_scene_diff.py).
#
#   .\build_scripts\gfx_tour.ps1 -Name menu -Buttons XS -Seconds 330 -From 6600
#   .\build_scripts\gfx_tour.ps1 -Name tour2 -ReplayOnly -Oracle   # grade vs PCSX2, no eyes
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
    [switch]$Oracle              # also grade every capture against PCSX2 GSRunner + lint + disc coverage
)
$root = 'F:\SDBZ Recomp'
$dir = Join-Path $root "gsdump\$Name"
if (-not $ReplayOnly) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    Get-ChildItem $dir -File -Recurse | Remove-Item -Confirm:$false
    $set = @{
        PS2X_DIAG = '0'; PS2X_DET_VBLANK_QUANTUM = '3000'; PS2X_GS_RASTER_THREADS = '0'
        PS2X_PAD_AUTOPRESS = '120'; PS2X_PAD_AUTOPRESS_BTNS = $Buttons; PS2X_PAD_AUTOPRESS_HOLD = '20'
        PS2X_GSCAP = "$From,$Frames,$dir\cap"; PS2X_GSCAP_EVERY = "$Every"
    }
    foreach ($k in $set.Keys) { Set-Item "Env:$k" $set[$k] }
    try {
        & "$root\launch_recomp.ps1" -Determinism 1 -RunSeconds $Seconds -NoDebugger -Exe "$root\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe" | Select-Object -Last 3
    } finally {
        foreach ($k in $set.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    }
}
python "$root\build_scripts\gfx_scene_diff.py" $dir
if ($Oracle) {
    # No human judges a frame: report.md lists every capture where we differ from PCSX2,
    # grouped by the blamed draw, plus runaway triangles; coverage names unreached screens.
    python "$root\build_scripts\gfx_scene_diff.py" $dir --oracle
    python "$root\build_scripts\audit_disc_textures.py" --coverage $dir | Select-Object -Last 25
    Write-Host "report: $dir\oracle\report.md"
}
