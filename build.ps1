# build.ps1 - MSBuild wrapper with progress (works on Desktop and Laptop)
# Usage: .\build.ps1 [Debug|RelWithDebInfo] [parallelism]
# Example: .\build.ps1 Debug 2
# Example: .\build.ps1 Debug -Test    # build the ps2x_tests unit-test runner

param(
    [string]$Config = "Debug",
    [int]$Jobs = 2,
    [switch]$Recomp,    # Build the ps2xRecomp tool instead of the runtime
    [switch]$Studio,    # Build ps2xStudio instead of the runtime
    [switch]$Debugger,  # Build ps2xDebugger instead of the runtime
    [switch]$Test       # Build the ps2x_tests unit-test runner instead of the runtime
)

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Write-Error "vswhere.exe not found - VS installer missing?"; exit 1 }
$vsRoot  = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsRoot) { Write-Error "No VS installation found via vswhere"; exit 1 }
$vsdev   = "$vsRoot\Common7\Tools\VsDevCmd.bat"
$msbuild = "$vsRoot\MSBuild\Current\Bin\amd64\MSBuild.exe"
$root    = $PSScriptRoot

if ($Recomp) {
    $target = "$root\build\ps2xRecomp\ps2_recomp.vcxproj"
    $total  = 8
} elseif ($Studio) {
    $target = "$root\build\ps2xStudio\ps2xStudio.vcxproj"
    $total  = 6
} elseif ($Debugger) {
    $target = "$root\build\ps2xRuntime\RecompDebugger.vcxproj"
    $total  = 12
} elseif ($Test) {
    $target = "$root\build\ps2xTest\ps2x_tests.vcxproj"
    $total  = 6
} else {
    $target = "$root\build\ps2xRuntime\ps2EntryRunner.vcxproj"
    $total  = 12
}

$done     = 0
$start    = Get-Date
$errorLog = "$root\build_errors.txt"
$buildLog = "$root\build_log.txt"

# Show errors from the previous build before clearing
if (Test-Path $errorLog) {
    $prevErrors = Get-Content $errorLog | Where-Object { $_.Trim() -ne "" }
    if ($prevErrors.Count -gt 0) {
        Write-Host "--- Errors from last build ---" -ForegroundColor Red
        $prevErrors | ForEach-Object { Write-Host $_ -ForegroundColor Red }
        Write-Host "------------------------------" -ForegroundColor Red
        Write-Host ""
    }
}

Set-Content $errorLog ""  # clear/create
Set-Content $buildLog ""  # clear/create

if ($Recomp) {
    Write-Host "Building ps2xRecomp tool ($Config)" -ForegroundColor Yellow
    Write-Host ""
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
        $exePath = "$root\build\ps2xRecomp\$Config\ps2_recomp.exe"
        if (Test-Path $exePath) {
            Write-Host "`nRecomp tool built: $exePath" -ForegroundColor Green
            Write-Host "Next: run it to regenerate output/ files:" -ForegroundColor Cyan
            Write-Host "  & `"$exePath`" `"$root\config.toml`"" -ForegroundColor White
        }
    } else {
        Write-Host "`nFailed (exit $exitCode) after +$elapsed" -ForegroundColor Red
    }
    exit $exitCode
}

# Sync recompiler output into build tree before compiling
$outputDir  = "$root\output"
$runnerDir  = "$root\ps2xRuntime\src\runner"
$includeDir = "$root\ps2xRuntime\include"

$missingCheckScript = "$root\build_scripts\check_missing_functions.py"
if (Test-Path $missingCheckScript) {
    Write-Host "Checking for orphaned call targets (BUG-008 pattern)..." -ForegroundColor DarkCyan
    & python $missingCheckScript
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Orphaned symbols found - see above. Extract with build_scripts/extract_folded_function.py, or Ctrl+C to abort." -ForegroundColor Red
    }
}

Write-Host "Syncing output\ -> src\runner\ and include\" -ForegroundColor DarkCyan
$copied = 0
foreach ($src in (Get-ChildItem "$outputDir\*.cpp")) {
    $dst = Join-Path $runnerDir $src.Name
    if (-not (Test-Path $dst) -or $src.LastWriteTime -gt (Get-Item $dst).LastWriteTime) {
        Copy-Item $src $dst -Force; $copied++
    }
}
foreach ($src in (Get-ChildItem "$outputDir\*.h")) {
    $dst = Join-Path $includeDir $src.Name
    if (-not (Test-Path $dst) -or $src.LastWriteTime -gt (Get-Item $dst).LastWriteTime) {
        Copy-Item $src $dst -Force; $copied++
    }
}
Write-Host "  -> $copied file(s) updated" -ForegroundColor DarkCyan

# Auto-generate fn_ forward declarations + sub_->fn_ aliases
Write-Host "Generating fn_forward_decls.h..." -ForegroundColor DarkCyan
$sig = "(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);"
$lines = @("// Auto-generated by build.ps1 - do not edit", "#pragma once", "", "#include <cstdint>", "struct R5900Context;", "class PS2Runtime;", "")

$fnFiles = Get-ChildItem $runnerDir -Filter "fn_*.cpp"
foreach ($f in $fnFiles) {
    $n = [System.IO.Path]::GetFileNameWithoutExtension($f.Name)
    $lines += "void $n$sig"
}

# Also declare BUG-008 fold-extracted override functions defined only in game_overrides.cpp
# (never materialized as their own runner/fn_*.cpp file, so the loop above misses them)
$fnFileNames = [System.Collections.Generic.HashSet[string]]::new()
foreach ($f in $fnFiles) { [void]$fnFileNames.Add([System.IO.Path]::GetFileNameWithoutExtension($f.Name)) }
$overridesPath = "$PSScriptRoot\ps2xRuntime\src\lib\game_overrides.cpp"
if (Test-Path $overridesPath) {
    $overridesContent = Get-Content $overridesPath -Raw
    $overrideMatches = [regex]::Matches($overridesContent, 'void\s+(fn_[0-9A-Fa-f]+_0x[0-9a-fA-F]+)\s*\(uint8_t\s*\*\s*rdram')
    $lines += ""
    $lines += "// game_overrides.cpp fold-extracted function declarations"
    foreach ($m in $overrideMatches) {
        $n = $m.Groups[1].Value
        if (-not $fnFileNames.Contains($n)) {
            $lines += "void $n$sig"
            [void]$fnFileNames.Add($n)
        }
    }
}

# Also declare Ghidra-named and other non-fn_ runner files so noop_wrapper callers can see them
$lines += ""
$lines += "// Ghidra-named and other runner function declarations"
$otherFiles = Get-ChildItem $runnerDir -Filter "*.cpp" | Where-Object { $_.Name -notlike "fn_*.cpp" }
foreach ($f in $otherFiles) {
    $n = [System.IO.Path]::GetFileNameWithoutExtension($f.Name)
    $lines += "void $n$sig"
}

# Build address -> best function name map for cross-naming aliases
$addrBest = @{}
$addrPri  = @{}
foreach ($f in (Get-ChildItem $runnerDir -Filter "*.cpp")) {
    $n = [System.IO.Path]::GetFileNameWithoutExtension($f.Name)
    if ($n -match '_(0x([0-9a-f]+))$') {
        $addrHex = $matches[2]
        $pri = if ($n -like 'fn_*') { 3 } elseif ($n -notlike 'noop*' -and $n -notlike 'nullsub*') { 2 } else { 1 }
        if (-not $addrPri.ContainsKey($addrHex) -or $pri -gt $addrPri[$addrHex]) {
            $addrBest[$addrHex] = $n
            $addrPri[$addrHex]  = $pri
        }
    }
}

$lines += ""
$lines += "// Cross-naming aliases: sub_XXXXXX and sub_00XXXXXX -> canonical function name"
foreach ($addrHex in $addrBest.Keys) {
    $bestName  = $addrBest[$addrHex]
    $addrUpper = $addrHex.ToUpper()
    $padded    = $addrUpper.PadLeft(8, '0')
    foreach ($alias in @("sub_${addrUpper}_0x${addrHex}", "sub_${padded}_0x${addrHex}")) {
        if ($alias -ne $bestName -and -not (Test-Path "$runnerDir\${alias}.cpp")) {
            $lines += "#define $alias $bestName"
        }
    }
}

$fwdDeclPath = "$includeDir\fn_forward_decls.h"
if (Test-Path $fwdDeclPath) {
    $existingLines = Get-Content $fwdDeclPath
    $changed = ($existingLines.Count -ne $lines.Count) -or (Compare-Object $existingLines $lines)
} else {
    $changed = $true
}
if ($changed) {
    $lines | Set-Content $fwdDeclPath -Encoding UTF8
    Write-Host "  -> $($fnFiles.Count) fn_ + $($otherFiles.Count) other declarations written" -ForegroundColor DarkCyan
} else {
    Write-Host "  -> fn_forward_decls.h unchanged (no recompile triggered)" -ForegroundColor DarkGray
}

# Inject include into ps2_recompiled_functions.h (idempotent — survives re-sync)
$rfhPath = "$includeDir\ps2_recompiled_functions.h"
if (Test-Path $rfhPath) {
    $rfhContent = Get-Content $rfhPath -Raw
    if ($rfhContent -notmatch 'fn_forward_decls') {
        $rfhContent = $rfhContent -replace '(#define PS2_RECOMPILED_FUNCTIONS_H)', "`$1`n`n#include `"fn_forward_decls.h`""
        Set-Content $rfhPath $rfhContent -Encoding UTF8 -NoNewline
        Write-Host "  -> injected fn_forward_decls.h into ps2_recompiled_functions.h" -ForegroundColor DarkCyan
    }
}

Write-Host "Building $Config with /m:$Jobs  ($total targets)" -ForegroundColor Yellow
Write-Host ""

# Run VsDevCmd + MSBuild in a single cmd shell so INCLUDE/LIB propagate correctly
# rlimgui_lib is a ProjectReference of ps2EntryRunner, but MSBuild doesn't walk the
# reference graph when targeting a single vcxproj directly (no .sln) - build it explicitly first.
$rlimguiTarget = "$root\build\ps2xRuntime\rlImGui.vcxproj"
$preBuild = ""
if (-not $Recomp -and -not $Studio -and -not $Debugger -and -not $Test -and (Test-Path $rlimguiTarget)) {
    $preBuild = "`"$msbuild`" `"$rlimguiTarget`" /p:Configuration=$Config /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.26100.0 /m:$Jobs /v:minimal && "
}
$cmdLine = "`"$vsdev`" -arch=amd64 && $preBuild`"$msbuild`" `"$target`" /p:Configuration=$Config /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.26100.0 /m:$Jobs /v:minimal"
cmd /c $cmdLine 2>&1 | ForEach-Object {
    Write-Host $_
    Add-Content $buildLog $_
    if ($_ -match ' error [A-Z]') {
        Add-Content $errorLog $_
    }
    if ($_ -match ' -> ') {
        $done++
        $pct     = [int](($done / $total) * 100)
        $filled  = [int]($pct / 5)
        $bar     = ('#' * $filled).PadRight(20)
        $elapsed = ((Get-Date) - $start).ToString("mm\:ss")
        Write-Host ("  [{0}] {1,3}%  ({2}/{3})  +{4}" -f $bar, $pct, $done, $total, $elapsed) -ForegroundColor Cyan
    }
}

$exitCode = $LASTEXITCODE
$elapsed  = ((Get-Date) - $start).ToString("mm\:ss")
if ($exitCode -eq 0) {
    Write-Host "`nDone in +$elapsed" -ForegroundColor Green
} else {
    Write-Host "`nFailed (exit $exitCode) after +$elapsed" -ForegroundColor Red
}
exit $exitCode
