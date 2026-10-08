# build_pcsx2.ps1 -- incremental build of the SDBZ PCSX2 (F:\PCSX2-src, branch sdbz-tools).
#
#   .\build_scripts\build_pcsx2.ps1            # pcsx2-qt  -> F:\PCSX2-src\bin\pcsx2-qtx64-avx2.exe
#   .\build_scripts\build_pcsx2.ps1 -GSRunner  # pcsx2-gsrunner -> F:\PCSX2-src\bin\pcsx2-gsrunnerx64-avx2.exe
#
# Builds the vcxproj directly with SolutionDir set (the .slnx /t:<project> form fails with MSB4057).
# Close PCSX2 first: a running pcsx2-qtx64-avx2.exe locks the file and the link fails.
# Log: Logs\build\pcsx2_build_log.txt
param([switch]$GSRunner, [int]$Jobs = 6)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$proj = if ($GSRunner) { 'F:\PCSX2-src\pcsx2-gsrunner\pcsx2-gsrunner.vcxproj' } else { 'F:\PCSX2-src\pcsx2-qt\pcsx2-qt.vcxproj' }
if (-not $GSRunner -and (Get-Process -Name 'pcsx2-qtx64-avx2' -ErrorAction SilentlyContinue)) {
    Write-Error 'PCSX2 is running: close it first (the exe is locked while it runs)'; exit 1
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vs) { Write-Error 'no Visual Studio with MSBuild found (vswhere)'; exit 1 }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
$logDir = Join-Path $repo 'Logs\build'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir 'pcsx2_build_log.txt'
$t0 = Get-Date
& $msbuild $proj '/p:SolutionDir=F:\PCSX2-src\' '/p:Configuration=Release AVX2' '/p:Platform=x64' "/m:$Jobs" '/v:m' '/nologo' `
    "/flp:LogFile=$log;Verbosity=normal" | Select-String -Pattern 'error|warning C|->' | ForEach-Object { $_.Line }
$rc = $LASTEXITCODE
'{0} in {1:N0} s (exit {2}); log: {3}' -f $(if ($rc -eq 0) { 'BUILD OK' } else { 'BUILD FAILED' }), ((Get-Date) - $t0).TotalSeconds, $rc, $log
exit $rc
