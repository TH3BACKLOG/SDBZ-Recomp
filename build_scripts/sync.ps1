#Requires -Version 5.1

# ---------------------------------------------------------------------------
# Machine detection
# ---------------------------------------------------------------------------
$IsDesktop = Test-Path "C:\SDBZ Recomp"
$IsLaptop  = Test-Path "F:\SDBZ Recomp"

if ($IsDesktop) {
    $Root   = "C:\SDBZ Recomp"
    $Prefix = "dt"
} elseif ($IsLaptop) {
    $Root   = "F:\SDBZ Recomp"
    $Prefix = "lt"
} else {
    Write-Error "Could not detect machine (neither C:\SDBZ Recomp nor F:\SDBZ Recomp found)."
    exit 1
}

$SyncDir  = if ($IsDesktop) { "C:\SDBZ_Sync" } else { "F:\SDBZ_Sync" }
$LogFile  = "$Root\_ClaudeMemory\sync_log.md"
$Bandizip = "C:\Program Files\Bandizip\Bandizip.exe"

$KnownLocations = [ordered]@{
    "Desktop (home network)" = "\\T3BL-DT\c$\SDBZ_Sync"
    "Laptop (home network)"  = "\\T3BL-LT\lt-2tb-sd-fdrive\SDBZ_Sync"
    "Laptop via RDP (Z:)"    = "Z:\SDBZ_Sync"
}

$MachineName = if ($IsDesktop) { "Desktop (dt_)" } else { "Laptop (lt_)" }

if (-not (Test-Path $SyncDir)) { New-Item -ItemType Directory -Path $SyncDir | Out-Null }

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function Get-Timestamp {
    $now = Get-Date
    return "{0:MM-dd-yyyy}_{1:HHmm}" -f $now, $now
}

function Get-SessionName { return "${Prefix}_sdbz_sync_$(Get-Timestamp)" }

function Write-Log {
    param([string]$Line)
    if (-not (Test-Path $LogFile)) {
        "# SDBZ Sync Log`n" | Set-Content $LogFile -Encoding utf8
    }
    Add-Content $LogFile $Line -Encoding utf8
}

function Get-AllSyncs {
    $folders = @(Get-ChildItem $SyncDir -Directory -ErrorAction SilentlyContinue | Sort-Object Name)
    $zips    = @(Get-ChildItem $SyncDir -ErrorAction SilentlyContinue |
                 Where-Object { $_.Extension -eq '.zip' -or $_.Extension -eq '.7z' } |
                 Sort-Object Name)
    return $folders, $zips
}

function Show-Header {
    Clear-Host
    Write-Host "================================================" -ForegroundColor DarkCyan
    Write-Host "   SDBZ Recomp Sync   |   Machine: $MachineName" -ForegroundColor Cyan
    Write-Host "================================================" -ForegroundColor DarkCyan
    Write-Host ""
}

# Read a line from console. Returns $null if Esc was pressed (= go back).
function Read-MenuInput {
    param([string]$Prompt = "Select")
    Write-Host "${Prompt}: " -NoNewline
    $text = ""
    while ($true) {
        $key = [Console]::ReadKey($true)
        switch ($key.Key) {
            "Escape" {
                Write-Host ""
                return $null
            }
            "Enter" {
                Write-Host ""
                return $text
            }
            "Backspace" {
                if ($text.Length -gt 0) {
                    $text = $text.Substring(0, $text.Length - 1)
                    Write-Host "`b `b" -NoNewline
                }
            }
            default {
                $c = $key.KeyChar
                if (-not [char]::IsControl($c)) {
                    $text += $c
                    Write-Host $c -NoNewline
                }
            }
        }
    }
}

function Pause-Menu {
    Write-Host ""
    Write-Host "Press Enter or Esc to continue..." -ForegroundColor DarkGray
    while ($true) {
        $key = [Console]::ReadKey($true)
        if ($key.Key -eq [ConsoleKey]::Enter -or $key.Key -eq [ConsoleKey]::Escape) { break }
    }
}

# ---------------------------------------------------------------------------
# File collection
# out\build is NOT excluded — extension filter blocks .obj/.lib etc.
# The built .exe will be included.
# ---------------------------------------------------------------------------
$ExcludeDirs = @('.git', 'SDBZ_Sync', '.vs', 'graphify-out')
$ExcludeExts = @('.obj', '.pdb', '.lib', '.exp', '.ilk', '.tlog', '.log',
                 '.lastbuildstate', '.cache', '.zip', '.7z')

function Copy-TodaysFiles {
    param([string]$DestPath)

    $today  = (Get-Date).Date
    $copied = 0

    $allFiles = Get-ChildItem $Root -Recurse -File -ErrorAction SilentlyContinue | Where-Object {
        $f = $_
        if ($f.LastWriteTime.Date -lt $today) { return $false }
        foreach ($ex in $ExcludeExts) { if ($f.Extension -eq $ex) { return $false } }
        $rel = $f.FullName.Substring($Root.Length + 1)
        foreach ($ex in $ExcludeDirs) { if ($rel -like "$ex\*" -or $rel -like "$ex/*") { return $false } }
        return $true
    }

    foreach ($f in $allFiles) {
        $rel     = $f.FullName.Substring($Root.Length + 1)
        $dest    = "$DestPath\$rel"
        $destDir = Split-Path $dest -Parent
        if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
        Copy-Item $f.FullName $dest -Force
        $copied++
    }
    return $copied
}

function Has-Archive {
    param([string]$Name)
    return (Test-Path "$SyncDir\$Name.zip") -or (Test-Path "$SyncDir\$Name.7z")
}

# ---------------------------------------------------------------------------
# 1. Create
# ---------------------------------------------------------------------------
function Do-Create {
    Show-Header
    Write-Host "[ CREATE SESSION ]" -ForegroundColor Cyan
    Write-Host ""

    $name = Get-SessionName
    $path = "$SyncDir\$name"

    if (Test-Path $path) {
        Write-Host "Session folder already exists with this timestamp:" -ForegroundColor Yellow
        Write-Host "  $name"
        Write-Host "Use Update (option 2) to add today's files to it."
        Pause-Menu
        return
    }

    New-Item -ItemType Directory -Path $path | Out-Null
    Write-Host "Created: $name" -ForegroundColor Green
    Write-Host "Scanning for files modified today..." -ForegroundColor DarkGray
    Write-Host ""

    $count = Copy-TodaysFiles -DestPath $path
    Write-Host "Copied $count files." -ForegroundColor Green
    Write-Host "Path: $path"

    $ts = Get-Date -f 'MM-dd-yyyy HH:mm'
    Write-Log "| $name | Created ($count files) | $ts | - | - | - |"
    Pause-Menu
}

# ---------------------------------------------------------------------------
# 2. Update
# ---------------------------------------------------------------------------
function Do-Update {
    Show-Header
    Write-Host "[ UPDATE SESSION ]" -ForegroundColor Cyan
    Write-Host ""

    $folders, $dummy = Get-AllSyncs
    $unzipped = @($folders | Where-Object { -not (Has-Archive $_.Name) })

    if ($unzipped.Count -eq 0) {
        Write-Host "No open (unzipped) session folders found." -ForegroundColor Yellow
        Write-Host "Create a new session first (option 1)."
        Pause-Menu
        return
    }

    for ($i = 0; $i -lt $unzipped.Count; $i++) {
        $fc = (Get-ChildItem $unzipped[$i].FullName -Recurse -File -ErrorAction SilentlyContinue).Count
        Write-Host "  $($i+1). $($unzipped[$i].Name)  [$fc files]"
    }
    Write-Host ""
    Write-Host "  [Esc] Back" -ForegroundColor DarkGray
    Write-Host ""

    $sel = Read-MenuInput "Select session to update"
    if ($null -eq $sel) { return }

    $idx = [int]$sel - 1
    if ($idx -lt 0 -or $idx -ge $unzipped.Count) {
        Write-Host "Invalid selection." -ForegroundColor Red
        Pause-Menu
        return
    }

    $target = $unzipped[$idx]
    $path   = $target.FullName

    Write-Host ""
    Write-Host "Merging today's files into $($target.Name) ..." -ForegroundColor DarkGray
    $count = Copy-TodaysFiles -DestPath $path
    $total = (Get-ChildItem $path -Recurse -File -ErrorAction SilentlyContinue).Count

    # Rename folder to reflect update time
    $newName = "updated_$(Get-SessionName)"
    $newPath = "$SyncDir\$newName"
    if ($newName -ne $target.Name -and -not (Test-Path $newPath)) {
        Rename-Item $path $newName
        Write-Host "Renamed: $($target.Name) -> $newName" -ForegroundColor DarkGray
        $path = $newPath
    }

    Write-Host "Merged $count file(s). Session now contains $total file(s)." -ForegroundColor Green
    Write-Host "Path: $path"
    Pause-Menu
}

# ---------------------------------------------------------------------------
# 3. Zip
# ---------------------------------------------------------------------------
function Do-Zip {
    Show-Header
    Write-Host "[ ZIP SESSION ]" -ForegroundColor Cyan
    Write-Host ""

    $folders, $dummy = Get-AllSyncs
    $unzipped = @($folders | Where-Object { -not (Has-Archive $_.Name) })

    if ($unzipped.Count -eq 0) {
        Write-Host "No open session folders to zip." -ForegroundColor Yellow
        Pause-Menu
        return
    }

    for ($i = 0; $i -lt $unzipped.Count; $i++) {
        $fc = (Get-ChildItem $unzipped[$i].FullName -Recurse -File -ErrorAction SilentlyContinue).Count
        Write-Host "  $($i+1). $($unzipped[$i].Name)  [$fc files]"
    }
    Write-Host ""
    Write-Host "  [Esc] Back" -ForegroundColor DarkGray
    Write-Host ""

    $sel = Read-MenuInput "Select session to zip"
    if ($null -eq $sel) { return }

    $idx = [int]$sel - 1
    if ($idx -lt 0 -or $idx -ge $unzipped.Count) {
        Write-Host "Invalid selection." -ForegroundColor Red
        Pause-Menu
        return
    }

    $name       = $unzipped[$idx].Name
    $folderPath = $unzipped[$idx].FullName
    $archiveOut = "$SyncDir\$name.7z"

    if (-not (Test-Path $Bandizip)) {
        Write-Host "Bandizip not found at: $Bandizip" -ForegroundColor Red
        Pause-Menu
        return
    }

    Write-Host ""
    Write-Host "Creating $name.7z ..." -ForegroundColor Cyan
    & $Bandizip a -y "$archiveOut" "$folderPath" | Out-Null

    if (Test-Path $archiveOut) {
        $sizeMB = [math]::Round((Get-Item $archiveOut).Length / 1MB, 2)
        Write-Host "Done: $name.7z ($sizeMB MB)" -ForegroundColor Green
        $ts = Get-Date -f 'MM-dd-yyyy HH:mm'
        Write-Log "| $name | Zipped (.7z, $sizeMB MB) | - | $ts | - | - |"
    } else {
        Write-Host "Bandizip ran but archive was not created." -ForegroundColor Red
    }
    Pause-Menu
}

# ---------------------------------------------------------------------------
# 4. Transfer
# ---------------------------------------------------------------------------
function Do-Transfer {
    Show-Header
    Write-Host "[ TRANSFER SESSION ]" -ForegroundColor Cyan
    Write-Host ""

    $dummy, $zips = Get-AllSyncs
    if ($zips.Count -eq 0) {
        Write-Host "No archives found. Zip a session first (option 3)." -ForegroundColor Yellow
        Pause-Menu
        return
    }

    Write-Host "Select archive to transfer:"
    for ($i = 0; $i -lt $zips.Count; $i++) {
        $sizeMB = [math]::Round($zips[$i].Length / 1MB, 2)
        Write-Host "  $($i+1). $($zips[$i].Name)  [$sizeMB MB]"
    }
    Write-Host ""
    Write-Host "  [Esc] Back" -ForegroundColor DarkGray
    Write-Host ""

    $sel = Read-MenuInput "Select archive"
    if ($null -eq $sel) { return }

    $idx = [int]$sel - 1
    if ($idx -lt 0 -or $idx -ge $zips.Count) {
        Write-Host "Invalid selection." -ForegroundColor Red
        Pause-Menu
        return
    }

    $zipName  = $zips[$idx].Name
    $localZip = "$SyncDir\$zipName"

    Write-Host ""
    Write-Host "Select destination:" -ForegroundColor Cyan
    $locList = @($KnownLocations.GetEnumerator())
    for ($i = 0; $i -lt $locList.Count; $i++) {
        $reachable = Test-Path $locList[$i].Value
        $status    = if ($reachable) { "[reachable]" } else { "[unreachable]" }
        $color     = if ($reachable) { "White" } else { "DarkGray" }
        Write-Host "  $($i+1). $($locList[$i].Key)  $status" -ForegroundColor $color
        Write-Host "       $($locList[$i].Value)" -ForegroundColor DarkGray
    }
    $newLocNum = $locList.Count + 1
    Write-Host "  $newLocNum. New location..."
    Write-Host ""
    Write-Host "  [Esc] Back" -ForegroundColor DarkGray
    Write-Host ""

    $sel2 = Read-MenuInput "Select destination"
    if ($null -eq $sel2) { return }

    $idx2 = [int]$sel2 - 1

    if ($idx2 -eq $locList.Count) {
        $destPath = Read-MenuInput "Enter full UNC or local path"
        if ($null -eq $destPath) { return }
    } elseif ($idx2 -ge 0 -and $idx2 -lt $locList.Count) {
        $destPath = $locList[$idx2].Value
        if (-not (Test-Path $destPath)) {
            Write-Host "That location is not reachable. Aborting." -ForegroundColor Red
            Pause-Menu
            return
        }
    } else {
        Write-Host "Invalid selection." -ForegroundColor Red
        Pause-Menu
        return
    }

    if (-not (Test-Path $destPath)) {
        try { New-Item -ItemType Directory -Path $destPath -Force | Out-Null }
        catch { Write-Host "Cannot reach or create: $destPath" -ForegroundColor Red; Pause-Menu; return }
    }

    Write-Host ""
    Write-Host "Copying $zipName -> $destPath ..." -ForegroundColor Cyan
    Copy-Item $localZip "$destPath\$zipName" -Force

    if (Test-Path "$destPath\$zipName") {
        Write-Host "Transfer complete!" -ForegroundColor Green
        $ts = Get-Date -f 'MM-dd-yyyy HH:mm'
        Write-Log "| $zipName | Transferred to $destPath | - | - | $ts | - |"
    } else {
        Write-Host "Copy ran but file not found at destination." -ForegroundColor Red
    }
    Pause-Menu
}

# ---------------------------------------------------------------------------
# 5. Manage
# ---------------------------------------------------------------------------
function Do-Manage {
    while ($true) {
        Show-Header
        Write-Host "[ MANAGE SYNCS ]" -ForegroundColor Cyan
        Write-Host ""
        Write-Host "  1. List all syncs"
        Write-Host "  2. Inspect archive contents"
        Write-Host "  3. Delete selected syncs (local + remote)"
        Write-Host "  4. Delete ALL syncs (local + remote)"
        Write-Host "  5. Back to main menu"
        Write-Host ""
        Write-Host "  [Esc] Back to main menu" -ForegroundColor DarkGray
        Write-Host ""

        $choice = Read-MenuInput "Select"
        if ($null -eq $choice) { return }

        switch ($choice) {
            "1" {
                Show-Header
                $folders, $zips = Get-AllSyncs
                Write-Host "--- Open Folders (not yet archived) ---" -ForegroundColor Yellow
                if ($folders.Count -eq 0) { Write-Host "  (none)" }
                foreach ($f in $folders) {
                    $fc = (Get-ChildItem $f.FullName -Recurse -File -ErrorAction SilentlyContinue).Count
                    Write-Host "  $($f.Name)  [$fc files]"
                }
                Write-Host ""
                Write-Host "--- Archives ---" -ForegroundColor Yellow
                if ($zips.Count -eq 0) { Write-Host "  (none)" }
                foreach ($z in $zips) {
                    $sizeMB = [math]::Round($z.Length / 1MB, 2)
                    Write-Host "  $($z.Name)  [$sizeMB MB]  $($z.LastWriteTime.ToString('MM-dd-yyyy HH:mm'))"
                }
                Pause-Menu
            }
            "2" {
                Show-Header
                $dummy, $zips = Get-AllSyncs
                if ($zips.Count -eq 0) {
                    Write-Host "No archives found." -ForegroundColor Yellow
                    Pause-Menu; continue
                }
                Write-Host "Select archive to inspect:" -ForegroundColor Cyan
                for ($j = 0; $j -lt $zips.Count; $j++) {
                    Write-Host "  $($j+1). $($zips[$j].Name)"
                }
                Write-Host ""
                Write-Host "  [Esc] Back" -ForegroundColor DarkGray
                Write-Host ""
                $sel = Read-MenuInput "Select"
                if ($null -eq $sel) { continue }
                $idx = [int]$sel - 1
                if ($idx -lt 0 -or $idx -ge $zips.Count) {
                    Write-Host "Invalid." -ForegroundColor Red; Pause-Menu; continue
                }
                Write-Host ""
                Write-Host "Contents of $($zips[$idx].Name):" -ForegroundColor Cyan
                Write-Host ""
                & $Bandizip l "$($zips[$idx].FullName)"
                Pause-Menu
            }
            "3" {
                # Multi-select delete
                Show-Header
                $folders, $zips = Get-AllSyncs
                $all = @()
                foreach ($f in $folders) { $all += [PSCustomObject]@{ Name = $f.Name; IsDir = $true } }
                foreach ($z in $zips)    { $all += [PSCustomObject]@{ Name = $z.Name; IsDir = $false } }

                if ($all.Count -eq 0) {
                    Write-Host "Nothing to delete." -ForegroundColor Yellow
                    Pause-Menu; continue
                }

                Write-Host "Select items to delete (local + all reachable remotes)." -ForegroundColor Cyan
                Write-Host "Enter numbers separated by commas (e.g. 1,3) or 'all'." -ForegroundColor DarkGray
                Write-Host ""
                for ($j = 0; $j -lt $all.Count; $j++) {
                    $tag = if ($all[$j].IsDir) { "[folder] " } else { "[archive]" }
                    Write-Host "  $($j+1). $tag  $($all[$j].Name)"
                }
                Write-Host ""
                Write-Host "  [Esc] Back" -ForegroundColor DarkGray
                Write-Host ""

                $sel = Read-MenuInput "Select"
                if ($null -eq $sel) { continue }

                $indices = @()
                if ($sel.Trim().ToLower() -eq "all") {
                    $indices = 0..($all.Count - 1)
                } else {
                    foreach ($part in ($sel -split ',')) {
                        $n = 0
                        if ([int]::TryParse($part.Trim(), [ref]$n) -and $n -ge 1 -and $n -le $all.Count) {
                            $indices += ($n - 1)
                        }
                    }
                }

                if ($indices.Count -eq 0) {
                    Write-Host "No valid items selected." -ForegroundColor Red
                    Pause-Menu; continue
                }

                Write-Host ""
                Write-Host "Will delete:" -ForegroundColor Yellow
                foreach ($i in $indices) { Write-Host "  - $($all[$i].Name)" }
                Write-Host ""

                $confirm = Read-MenuInput "Confirm (y/n)"
                if ($null -eq $confirm -or $confirm.ToLower() -ne "y") {
                    Write-Host "Cancelled." -ForegroundColor DarkGray
                    Pause-Menu; continue
                }

                foreach ($i in $indices) {
                    $item      = $all[$i]
                    $localPath = "$SyncDir\$($item.Name)"
                    if (Test-Path $localPath) {
                        if ($item.IsDir) { Remove-Item $localPath -Recurse -Force }
                        else             { Remove-Item $localPath -Force }
                        Write-Host "Deleted local: $($item.Name)" -ForegroundColor Green
                    }
                    if (-not $item.IsDir) {
                        foreach ($loc in $KnownLocations.Values) {
                            $remote = "$loc\$($item.Name)"
                            if (Test-Path $remote) {
                                Remove-Item $remote -Force
                                Write-Host "Deleted remote ($loc): $($item.Name)" -ForegroundColor Green
                            }
                        }
                    }
                    $ts = Get-Date -f 'MM-dd-yyyy HH:mm'
                    Write-Log "| $($item.Name) | Deleted (local+remote) | - | - | - | $ts |"
                }
                Pause-Menu
            }
            "4" {
                # Delete ALL
                Show-Header
                $folders, $zips = Get-AllSyncs
                $total = $folders.Count + $zips.Count
                if ($total -eq 0) {
                    Write-Host "Nothing to delete." -ForegroundColor Yellow
                    Pause-Menu; continue
                }

                Write-Host "This will delete ALL $total items (local + all reachable remotes):" -ForegroundColor Red
                Write-Host ""
                foreach ($f in $folders) { Write-Host "  [folder]  $($f.Name)" }
                foreach ($z in $zips)    { Write-Host "  [archive] $($z.Name)" }
                Write-Host ""

                $confirm = Read-MenuInput "Type 'yes' to confirm"
                if ($null -eq $confirm -or $confirm.ToLower() -ne "yes") {
                    Write-Host "Cancelled." -ForegroundColor DarkGray
                    Pause-Menu; continue
                }

                foreach ($f in $folders) {
                    Remove-Item $f.FullName -Recurse -Force
                    Write-Host "Deleted: $($f.Name)" -ForegroundColor Green
                    Write-Log "| $($f.Name) | Deleted-all (local) | - | - | - | $(Get-Date -f 'MM-dd-yyyy HH:mm') |"
                }
                foreach ($z in $zips) {
                    Remove-Item $z.FullName -Force
                    Write-Host "Deleted: $($z.Name)" -ForegroundColor Green
                    foreach ($loc in $KnownLocations.Values) {
                        $remote = "$loc\$($z.Name)"
                        if (Test-Path $remote) {
                            Remove-Item $remote -Force
                            Write-Host "Deleted remote ($loc): $($z.Name)" -ForegroundColor Green
                        }
                    }
                    Write-Log "| $($z.Name) | Deleted-all (local+remote) | - | - | - | $(Get-Date -f 'MM-dd-yyyy HH:mm') |"
                }
                Pause-Menu
            }
            "5" { return }
            default { Write-Host "Invalid." -ForegroundColor Red; Start-Sleep 1 }
        }
    }
}

# ---------------------------------------------------------------------------
# Main menu
# ---------------------------------------------------------------------------
while ($true) {
    Show-Header
    Write-Host "  1. Create new session"
    Write-Host "  2. Update existing session"
    Write-Host "  3. Zip session"
    Write-Host "  4. Transfer zip to other machine"
    Write-Host "  5. Manage syncs (list / inspect / delete)"
    Write-Host "  6. Exit"
    Write-Host ""
    Write-Host "  [Esc] Exit" -ForegroundColor DarkGray
    Write-Host ""

    $choice = Read-MenuInput "Select"
    if ($null -eq $choice) { exit 0 }

    switch ($choice) {
        "1" { Do-Create }
        "2" { Do-Update }
        "3" { Do-Zip }
        "4" { Do-Transfer }
        "5" { Do-Manage }
        "6" { exit 0 }
        default { Write-Host "Invalid." -ForegroundColor Red; Start-Sleep 1 }
    }
}
