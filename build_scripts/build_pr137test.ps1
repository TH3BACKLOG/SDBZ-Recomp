# build_pr137test.ps1 - Recompiler + build pipeline for the F:\SDBZ-Recomp-pr137-test sandbox ONLY.
# NOT a replacement for F:\SDBZ Recomp\build.ps1 - that script targets the main tree's
# PS2Recomp\ prefixed layout and cannot see this sandbox's paths (no PS2Recomp\ prefix here,
# build\ instead of out\build\ps2xRuntime).
#
# Usage:
#   .\build_pr137test.ps1 -Recomp              # build ps2_recomp.exe only
#   .\build_pr137test.ps1 -Generate             # run ps2_recomp.exe against config.toml -> output\
#   .\build_pr137test.ps1                       # sync output\ -> src\runner\/include\, then build ps2EntryRunner
#   .\build_pr137test.ps1 -Config RelWithDebInfo -Jobs 4

param(
    [string]$Config = "Debug",
    [int]$Jobs = 3,
    [switch]$Recomp,     # Build the ps2xRecomp tool instead of the runtime
    [switch]$Generate    # Run ps2_recomp.exe against config.toml to regenerate output\
)

$root    = $PSScriptRoot
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Write-Error "vswhere.exe not found - VS installer missing?"; exit 1 }
$vsRoot  = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsRoot) { Write-Error "No VS installation found via vswhere"; exit 1 }
$vsdev   = "$vsRoot\Common7\Tools\VsDevCmd.bat"
$msbuild = "$vsRoot\MSBuild\Current\Bin\amd64\MSBuild.exe"

function Invoke-MSBuild($target, $total, $label) {
    $done     = 0
    $start    = Get-Date
    $errorLog = "$root\build_errors.txt"
    $buildLog = "$root\build_log.txt"
    Set-Content $errorLog ""
    Set-Content $buildLog ""

    Write-Host "Building $label ($Config)" -ForegroundColor Yellow
    $cmdLine = "`"$vsdev`" -arch=amd64 && `"$msbuild`" `"$target`" /p:Configuration=$Config /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.26100.0 /m:$Jobs /v:minimal"
    cmd /c $cmdLine 2>&1 | ForEach-Object {
        Write-Host $_
        Add-Content $buildLog $_
        if ($_ -match ' error [A-Z]') { Add-Content $errorLog $_ }
        if ($_ -match ' -> ') {
            $done++
            $pct = [int](($done / $total) * 100)
            $filled = [int]($pct / 5)
            $bar = ('#' * $filled).PadRight(20)
            $elapsed = ((Get-Date) - $start).ToString("mm\:ss")
            Write-Host ("  [{0}] {1,3}%  ({2}/{3})  +{4}" -f $bar, $pct, $done, $total, $elapsed) -ForegroundColor Cyan
        }
    }
    $exitCode = $LASTEXITCODE
    $elapsed = ((Get-Date) - $start).ToString("mm\:ss")
    if ($exitCode -eq 0) {
        Write-Host "`nDone in +$elapsed" -ForegroundColor Green
    } else {
        Write-Host "`nFailed (exit $exitCode) after +$elapsed" -ForegroundColor Red
    }
    return $exitCode
}

if ($Recomp) {
    $target = "$root\build\ps2xRecomp\ps2_recomp.vcxproj"
    $exitCode = Invoke-MSBuild $target 8 "ps2xRecomp tool"
    if ($exitCode -eq 0) {
        $exePath = "$root\build\ps2xRecomp\$Config\ps2_recomp.exe"
        if (Test-Path $exePath) {
            Write-Host "`nRecomp tool built: $exePath" -ForegroundColor Green
            Write-Host "Next: .\build_pr137test.ps1 -Generate" -ForegroundColor Cyan
        }
    }
    exit $exitCode
}

if ($Generate) {
    $exePath = "$root\build\ps2xRecomp\Release\ps2_recomp.exe"
    if (-not (Test-Path $exePath)) { $exePath = "$root\build\ps2xRecomp\$Config\ps2_recomp.exe" }
    if (-not (Test-Path $exePath)) {
        Write-Error "ps2_recomp.exe not found - run .\build_pr137test.ps1 -Recomp first"
        exit 1
    }
    $configToml = "$root\config.toml"
    if (-not (Test-Path $configToml)) {
        Write-Error "config.toml not found at $configToml"
        exit 1
    }
    Write-Host "Running $exePath against $configToml ..." -ForegroundColor Yellow
    & $exePath $configToml
    $exitCode = $LASTEXITCODE
    if ($exitCode -eq 0) {
        Write-Host "`nGenerated output written to $root\output" -ForegroundColor Green
        Write-Host "Next: .\build_pr137test.ps1  (syncs output\ and builds ps2EntryRunner)" -ForegroundColor Cyan
    } else {
        Write-Host "`nps2_recomp.exe failed (exit $exitCode)" -ForegroundColor Red
    }
    exit $exitCode
}

# --- Normal path: sync output\ -> src\runner\/include\, then build ps2EntryRunner ---
$outputDir  = "$root\output"
$runnerDir  = "$root\ps2xRuntime\src\runner"
$includeDir = "$root\ps2xRuntime\include"

if (-not (Test-Path $outputDir)) {
    Write-Error "No output\ directory found - run .\build_pr137test.ps1 -Recomp then .\build_pr137test.ps1 -Generate first"
    exit 1
}

if (-not (Test-Path $runnerDir)) { New-Item -ItemType Directory -Force -Path $runnerDir | Out-Null }

Write-Host "Syncing output\ -> src\runner\ and include\" -ForegroundColor DarkCyan
$copied = 0
foreach ($src in (Get-ChildItem "$outputDir\*.cpp" -ErrorAction SilentlyContinue)) {
    $dst = Join-Path $runnerDir $src.Name
    if (-not (Test-Path $dst) -or $src.LastWriteTime -gt (Get-Item $dst).LastWriteTime) {
        Copy-Item $src $dst -Force; $copied++
    }
}
foreach ($src in (Get-ChildItem "$outputDir\*.h" -ErrorAction SilentlyContinue)) {
    $dst = Join-Path $includeDir $src.Name
    if (-not (Test-Path $dst) -or $src.LastWriteTime -gt (Get-Item $dst).LastWriteTime) {
        Copy-Item $src $dst -Force; $copied++
    }
}
Write-Host "  -> $copied file(s) updated" -ForegroundColor DarkCyan

$target = "$root\build\ps2xRuntime\ps2EntryRunner.vcxproj"
$exitCode = Invoke-MSBuild $target 12 "ps2EntryRunner"
if ($exitCode -eq 0) {
    $exePath = "$root\build\ps2xRuntime\$Config\ps2EntryRunner.exe"
    if (Test-Path $exePath) {
        Write-Host "`nBuilt: $exePath" -ForegroundColor Green
        Write-Host "Run: & `"$exePath`" `"$root\ELF\SLUS_214.42`"" -ForegroundColor Cyan
    }
}
exit $exitCode
