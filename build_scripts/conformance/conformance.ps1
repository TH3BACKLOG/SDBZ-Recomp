# conformance.ps1 -- Stage B opcode conformance: OUR recompiled code vs PCSX2, one command.
#
#   .\build_scripts\conformance\conformance.ps1 -Gen -Build      # first time: generate, configure+build, run both, report
#   .\build_scripts\conformance\conformance.ps1                  # rerun ours + report (PCSX2 output reused while the ELF is unchanged)
#   .\build_scripts\conformance\conformance.ps1 -SeedSqrtBug     # control: our sqrt.s reads fs (the 09-26 bug) -> sqrt.s must FAIL
#   .\build_scripts\conformance\conformance.ps1 -GameFixes       # the game's sqrt.s/vsqrt/vrsqrt override rewrite -> report_fixed.md
#
# Steps: gen_conformance.py (test ELF + ps2_recomp -> out\gen) -> build ps2x_conformance
#        -> ours.bin -> run_pcsx2.py (pcsx2.bin) -> conformance_diff.py -> out\report.md
# Close PCSX2 first. No game ISO, no recomp run.
param(
    [switch]$Gen,          # regenerate the ELF + generated C++ (needs -Build afterwards)
    [switch]$Build,        # cmake configure if the target is new, then build ps2x_conformance
    [switch]$Pcsx2,        # force a fresh PCSX2 run
    [switch]$SeedSqrtBug,  # seeded control run (one rebuild; the next plain run rebuilds clean)
    [switch]$GameFixes,    # gen_sqrt_abs_overrides.py rewrite applied (one rebuild; the next plain run rebuilds clean)
    [string]$Config = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = Split-Path -Parent (Split-Path -Parent $here)
$out = Join-Path $here 'out'
$exe = Join-Path $root "build\ps2xTest\$Config\ps2x_conformance.exe"
$proj = Join-Path $root 'build\ps2xTest\ps2x_conformance.vcxproj'
$variantFile = Join-Path $out 'exe_variant.txt'   # which generated code the exe was built from: clean / seeded / fixed

function Invoke-Py { python @args; if ($LASTEXITCODE -ne 0) { throw "python $($args[0]) failed ($LASTEXITCODE)" } }

function Build-Conformance([string]$variant = 'clean') {
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
    if (-not (Test-Path $proj)) {
        # New target: re-run configure on the EXISTING build tree (keeps every cached option; no rebuild of the game).
        Write-Host 'configure: adding ps2x_conformance to build\' -ForegroundColor DarkCyan
        # Takes several minutes (the runner glob is ~17k files). Deprecation warnings from _deps are normal.
        cmd /c "call `"$vs\Common7\Tools\VsDevCmd.bat`" -arch=amd64 >nul 2>&1 && cmake -Wno-dev -Wno-deprecated -S `"$root`" -B `"$root\build`" 2>&1" | Select-Object -Last 3
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $proj)) { throw "cmake configure failed or did not create $proj" }
    }
    $t0 = Get-Date
    cmd /c "call `"$vs\Common7\Tools\VsDevCmd.bat`" -arch=amd64 >nul 2>&1 && `"$vs\MSBuild\Current\Bin\amd64\MSBuild.exe`" `"$proj`" /p:Configuration=$Config /p:Platform=x64 /m:4 /v:m /nologo" |
        Where-Object { $_ -match ' error |-> ' }
    if ($LASTEXITCODE -ne 0) { throw "ps2x_conformance build failed ($LASTEXITCODE)" }
    Set-Content -Path $variantFile -Value $variant
    'build OK in {0:N0} s ({1})' -f ((Get-Date) - $t0).TotalSeconds, $variant
}

function Run-Ours([string]$bin) {
    & $exe (Join-Path $out 'conformance.elf') $bin
    if ($LASTEXITCODE -ne 0) { throw "ps2x_conformance failed ($LASTEXITCODE)" }
}

function Ensure-Pcsx2 {
    $elfSha = (Get-FileHash (Join-Path $out 'conformance.elf') -Algorithm SHA1).Hash.ToLower()
    $meta = Join-Path $out 'pcsx2.json'
    $fresh = (Test-Path (Join-Path $out 'pcsx2.bin')) -and (Test-Path $meta) -and
             ((Get-Content $meta -Raw | ConvertFrom-Json).elf_sha1 -eq $elfSha) -and
             ((Get-Content $meta -Raw | ConvertFrom-Json).complete)
    if ($Pcsx2 -or -not $fresh) {
        python (Join-Path $here 'run_pcsx2.py') --dir $out
        if ($LASTEXITCODE -eq 3) { Write-Warning 'PCSX2 got stuck: the report covers only the records it ran' }
        elseif ($LASTEXITCODE -ne 0) { throw "run_pcsx2.py failed ($LASTEXITCODE)" }
    } else { 'pcsx2.bin is current for this ELF (use -Pcsx2 to rerun)' }
}

if ($SeedSqrtBug) {
    Invoke-Py (Join-Path $here 'gen_conformance.py') --seed-bug sqrt
    Build-Conformance 'seeded'
    Run-Ours (Join-Path $out 'ours_seeded.bin')
    Ensure-Pcsx2
    Invoke-Py (Join-Path $here 'conformance_diff.py') --dir $out --ours (Join-Path $out 'ours_seeded.bin') --report (Join-Path $out 'report_seeded.md')
    Select-String -Path (Join-Path $out 'report_seeded.md') -Pattern '^\| sqrt\.s ' | ForEach-Object { 'seeded control: ' + $_.Line }
    return   # the next plain run sees exe_variant.txt != clean and regenerates + rebuilds
}

if ($GameFixes) {
    Invoke-Py (Join-Path $here 'gen_conformance.py') --game-fixes
    Build-Conformance 'fixed'
    Run-Ours (Join-Path $out 'ours_fixed.bin')
    Ensure-Pcsx2
    Invoke-Py (Join-Path $here 'conformance_diff.py') --dir $out --ours (Join-Path $out 'ours_fixed.bin') --report (Join-Path $out 'report_fixed.md')
    Select-String -Path (Join-Path $out 'report_fixed.md') -Pattern '^\| (sqrt\.s|vsqrt|vrsqrt) ' | ForEach-Object { 'game fixes: ' + $_.Line }
    return   # the next plain run sees exe_variant.txt != clean and regenerates + rebuilds
}

$dirty = (Test-Path $variantFile) -and ((Get-Content $variantFile -Raw).Trim() -ne 'clean')
if ($dirty) { Write-Host 'exe was built from seeded/fixed code: regenerating + rebuilding clean' -ForegroundColor DarkCyan }
if ($Gen -or $dirty -or -not (Test-Path (Join-Path $out 'gen\conformance_fns.inc'))) { Invoke-Py (Join-Path $here 'gen_conformance.py') }
if ($Build -or $dirty -or -not (Test-Path $exe)) { Build-Conformance }
Run-Ours (Join-Path $out 'ours.bin')
Ensure-Pcsx2
Invoke-Py (Join-Path $here 'conformance_diff.py') --dir $out
