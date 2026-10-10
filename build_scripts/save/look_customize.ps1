<#
.SYNOPSIS
  Run the game on the current memory card, drive it into Customize (customize_look.txt) and save
  desktop screenshots every few seconds to Logs\save_look\<stamp>\ so the card / colour / skill-tree
  screens can be looked at.  pwsh -NoProfile -File build_scripts\save\look_customize.ps1
#>
param(
    [string]$Exe = "F:\SDBZ Recomp\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe",
    [int]$RunSeconds = 330,
    [int]$EverySeconds = 8,
    [string]$Script = 'build_scripts\sweeps\customize_look.txt'
)
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class W32 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
}
'@
$out = Join-Path $root ("Logs\save_look\" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $out | Out-Null
$env:PS2X_DIAG = '0'; $env:PS2X_VU1_THREAD = '1'; $env:PS2X_GS_NEAREST = '1'; $env:PS2X_DET_VBLANK_QUANTUM = '3000'
$env:PS2X_PAD_SCRIPT = Join-Path $root $Script
$job = Start-Job -ScriptBlock {
    param($l, $e, $s) & $l -Exe $e -NoDebugger -RunSeconds $s | Out-Null
} -ArgumentList (Join-Path $root 'launch_recomp.ps1'), $Exe, $RunSeconds
$t0 = Get-Date; $n = 0
while ($job.State -eq 'Running' -and ((Get-Date) - $t0).TotalSeconds -lt ($RunSeconds + 20)) {
    Start-Sleep -Seconds $EverySeconds
    try {
        # just the game window when it exists (title 'PS2-Recomp | ...'), else the whole desktop
        $p = Get-Process ps2EntryRunner -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        $r = New-Object W32+RECT
        if ($p -and [W32]::GetWindowRect($p.MainWindowHandle, [ref]$r) -and ($r.R - $r.L) -gt 100) {
            $b = New-Object System.Drawing.Rectangle $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T)
        } else { $b = [System.Windows.Forms.SystemInformation]::VirtualScreen }
        $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
        $bmp.Save((Join-Path $out ('s{0:000}.png' -f $n)), [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose(); $n++
    } catch { }
}
Remove-Item Env:PS2X_PAD_SCRIPT -ErrorAction SilentlyContinue
Receive-Job $job -ErrorAction SilentlyContinue | Out-Null
Write-Host "screenshots: $n -> $out"
