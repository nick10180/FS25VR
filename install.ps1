<#
.SYNOPSIS
  Installs fs25vr (stereoscopic 6DOF VR for Farming Simulator 25).

.DESCRIPTION
  Copies the bridge (dinput8.dll, openxr_loader.dll, fs25vr.ini) into <game>\x64 and the
  FS25_VR mod into your mods folder (honours the in-game mods-folder override).
  Works from a release package (x64\ + mod\ folders) or from a source checkout after build.bat.

.PARAMETER GameDir
  Farming Simulator 25 install folder. Detected from Steam, the installed-programs list, Epic or
  the Xbox app if omitted.

.PARAMETER ProfileDir
  The game's profile folder (gameSettings.xml, game.xml, mods). Defaults to
  Documents\My Games\FarmingSimulator2025, or for the Xbox app / Microsoft Store version
  %LOCALAPPDATA%\Packages\GIANTSSoftware.FarmingSimulator25PC_<id>\LocalCache\Local.

.PARAMETER EyeResolution
  Not needed normally: the bridge measures the render size for your headset in the first VR
  session and uses it from the next start on. This fixes the size to -EyeWidth x -EyeHeight instead
  (renderWidth/renderHeight in fs25vr.ini; set them to 0 to go back to the measured size).

.PARAMETER AutoResolution
  Like -EyeResolution, with the size the bridge measured (from x64\fs25vr.log).

.PARAMETER Uninstall
  Removes the bridge and the mod and restores game.xml if it was changed.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File install.ps1
  powershell -ExecutionPolicy Bypass -File install.ps1 -EyeResolution -EyeWidth 5672 -EyeHeight 4336
  powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall
#>
param(
    [string]$GameDir = "",
    [string]$ProfileDir = "",
    [int]$EyeWidth = 2448,
    [int]$EyeHeight = 2448,
    [switch]$EyeResolution,
    [switch]$AutoResolution,
    [switch]$Uninstall
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

function Find-GameDir {
    $candidates = @()
    try {
        $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction Stop).SteamPath
        $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
        $libs = @($steam)
        if (Test-Path $vdf) {
            $libs += (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' -AllMatches).Matches |
                ForEach-Object { $_.Groups[1].Value.Replace('\\', '\') }
        }
        $candidates += $libs | ForEach-Object { Join-Path $_ "steamapps\common\Farming Simulator 25" }
    } catch { }
    # installed-programs list (standalone / GIANTS store / other installers)
    foreach ($key in "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
                     "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*",
                     "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*") {
        Get-ItemProperty $key -ErrorAction SilentlyContinue |
            Where-Object { $_.DisplayName -match "Farming Simulator (25|2025)" -and $_.InstallLocation } |
            ForEach-Object { $candidates += $_.InstallLocation }
    }
    # Epic Games Store
    $epic = Join-Path $env:ProgramData "Epic\EpicGamesLauncher\Data\Manifests"
    if (Test-Path $epic) {
        Get-ChildItem $epic -Filter *.item -ErrorAction SilentlyContinue | ForEach-Object {
            try {
                $m = Get-Content $_.FullName -Raw | ConvertFrom-Json
                if ($m.DisplayName -match "Farming Simulator 25") { $candidates += $m.InstallLocation }
            } catch { }
        }
    }
    # Xbox app / Microsoft Store (GDK): each drive's .GamingRoot lists the library folders
    # (default "XboxGames"); the game lives in <library>\<title>\Content.
    foreach ($drive in Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue) {
        $gr = Join-Path $drive.Root ".GamingRoot"
        if (-not (Test-Path $gr)) { continue }
        try {
            $b = [IO.File]::ReadAllBytes($gr)
            $libs = [Text.Encoding]::Unicode.GetString($b, 8, $b.Length - 8).Split([char]0) | Where-Object { $_ }
            foreach ($lib in $libs) {
                Get-ChildItem (Join-Path $drive.Root $lib) -Directory -Filter "Farming Simulator 25*" -ErrorAction SilentlyContinue |
                    ForEach-Object { $candidates += $_.FullName }
            }
        } catch { }
    }
    $candidates += "C:\XboxGames\Farming Simulator 25", "C:\XboxGames\Farming Simulator 25 (PC)"
    $candidates += "C:\Program Files (x86)\Farming Simulator 2025", "C:\Program Files\Farming Simulator 2025"
    foreach ($c in $candidates) {
        $r = Resolve-GameDir $c
        if ($r) { return $r }
    }
    return $null
}

# The folder that contains x64\FarmingSimulator2025Game.exe. Also accepts the Xbox app's install
# folder (the exe is in its Content\x64) and the x64 folder itself.
function Resolve-GameDir($dir) {
    if (-not $dir) { return $null }
    foreach ($d in $dir, (Join-Path $dir "Content"), (Split-Path $dir -Parent)) {
        if ($d -and (Test-Path (Join-Path $d "x64\FarmingSimulator2025Game.exe"))) { return $d }
    }
    return $null
}

if (-not $GameDir) { $GameDir = Find-GameDir }
while (-not (Resolve-GameDir $GameDir)) {
    if ($GameDir) { Write-Host "Farming Simulator 25 was not found in: $GameDir" }
    else { Write-Host "Could not find Farming Simulator 25 automatically." }
    Write-Host "Paste the game's install folder (the one that contains x64\FarmingSimulator2025Game.exe;"
    Write-Host "for the Xbox app that is the game's Content folder) and press Enter, or just press Enter to cancel:"
    $answer = Read-Host
    if ($null -eq $answer -or -not $answer.Trim()) { throw "Cancelled - nothing was installed." }
    $GameDir = $answer.Trim().Trim('"')
}
$GameDir = Resolve-GameDir $GameDir
$x64 = Join-Path $GameDir "x64"

# The Xbox app / Microsoft Store build keeps its profile (gameSettings.xml, game.xml, mods) in its
# package folder, %LOCALAPPDATA%\Packages\GIANTSSoftware.FarmingSimulator25PC_<id>\LocalCache\Local,
# instead of Documents\My Games. Started outside the Xbox app (the exe run directly) it uses
# %LOCALAPPDATA%\GIANTSSoftware.FarmingSimulator25PC instead.
if (-not $ProfileDir) {
    $msStore = (Test-Path (Join-Path $GameDir "MicrosoftGame.config")) -or (Test-Path (Join-Path $GameDir "appxmanifest.xml"))
    if ($msStore) {
        $pkg = Get-ChildItem (Join-Path $env:LOCALAPPDATA "Packages") -Directory -Filter "GIANTSSoftware.FarmingSimulator25PC_*" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        $ProfileDir = if ($pkg) { Join-Path $pkg.FullName "LocalCache\Local" }
                      else { Join-Path $env:LOCALAPPDATA "GIANTSSoftware.FarmingSimulator25PC" }
    } else {
        $ProfileDir = "$([Environment]::GetFolderPath('MyDocuments'))\My Games\FarmingSimulator2025"
    }
}
Write-Host "Game folder:    $GameDir"
Write-Host "Profile folder: $ProfileDir"

if (Get-Process FarmingSimulator2025Game, FarmingSimulator2025 -ErrorAction SilentlyContinue) {
    throw "Farming Simulator 25 is running. Close the game first, then try again."
}

$mods = Join-Path $ProfileDir "mods"
$settingsXml = Join-Path $ProfileDir "gameSettings.xml"
if (Test-Path $settingsXml) {
    $o = ([xml](Get-Content $settingsXml)).gameSettings.modsDirectoryOverride
    if ($o -and $o.active -eq "true" -and $o.directory) { $mods = $o.directory.Replace('/', '\') }
}
$zip = Join-Path $mods "FS25_VR.zip"

if ($Uninstall) {
    # If game.xml still holds the VR render size, put the player's own display settings back (kept by
    # the bridge in game.xml.fs25vr-flat, or by older installers in game.xml.fs25vr-backup).
    $gameXml = Join-Path $ProfileDir "game.xml"
    $vrSizes = @()
    foreach ($pair in @(@("renderWidth", "renderHeight", (Join-Path $x64 "fs25vr.ini")),
                        @("width", "height", (Join-Path $x64 "fs25vr_size.ini")),
                        @("appliedWidth", "appliedHeight", (Join-Path $x64 "fs25vr_size.ini")))) {
        if (-not (Test-Path $pair[2])) { continue }
        $text = [IO.File]::ReadAllText($pair[2])
        $w = [regex]::Match($text, '(?m)^\s*' + $pair[0] + '\s*=\s*(\d+)'); $h = [regex]::Match($text, '(?m)^\s*' + $pair[1] + '\s*=\s*(\d+)')
        if ($w.Success -and $h.Success -and [int]$w.Groups[1].Value -gt 0) { $vrSizes += "$($w.Groups[1].Value)x$($h.Groups[1].Value)" }
    }
    if (Test-Path $gameXml) {
        try {
            [xml]$x = Get-Content $gameXml
            $d = $x.game.graphic.display
            if ($d.fullscreenMode -eq "windowed" -and ($vrSizes -contains "$($d.width)x$($d.height)")) {
                $src = @("$gameXml.fs25vr-flat", "$gameXml.fs25vr-backup") | Where-Object { Test-Path $_ } | Select-Object -First 1
                $w, $h, $m, $v = "0", "0", "windowed_fullscreen", "true"   # the game's defaults: the whole screen
                if ($src) {
                    $f = ([xml](Get-Content $src)).game.graphic.display
                    $w, $h, $m, $v = $f.width, $f.height, $f.fullscreenMode, $f.SelectSingleNode("vsync").InnerText
                }
                $d.width = "$w"; $d.height = "$h"; $d.fullscreenMode = "$m"; $d.SelectSingleNode("vsync").InnerText = "$v"
                $x.Save($gameXml)
                Write-Host "game.xml: display settings back to ${w}x${h} $m"
            }
        } catch { Write-Host "Could not restore the display settings in game.xml: $_" }
    }
    foreach ($f in "dinput8.dll", "dinput8.pdb", "openxr_loader.dll", "fs25vr.ini", "fs25vr_size.ini", "fs25vr.log", "fs25vr_profile.csv") {
        Remove-Item (Join-Path $x64 $f) -ErrorAction SilentlyContinue
    }
    Remove-Item $zip -ErrorAction SilentlyContinue
    Remove-Item "$gameXml.fs25vr-flat", "$gameXml.fs25vr-backup" -ErrorAction SilentlyContinue
    Write-Host "fs25vr removed from $GameDir"
    return
}

# Where the files come from: a release package or a source checkout.
$release = Test-Path (Join-Path $root "x64\dinput8.dll")
if ($release) {
    $dll = Join-Path $root "x64\dinput8.dll"
    $loader = Join-Path $root "x64\openxr_loader.dll"
    $ini = Join-Path $root "x64\fs25vr.ini"
} else {
    $dll = Join-Path $root "build\dinput8.dll"
    $loader = Join-Path $root "third_party\openxr\x64\bin\openxr_loader.dll"
    $ini = Join-Path $root "dist\fs25vr.ini"
    if (-not (Test-Path $dll)) { throw "Build first (build.bat), or run this from a release package." }
}

Copy-Item $dll $x64 -Force
Copy-Item $loader $x64 -Force
if (-not $release) { Copy-Item (Join-Path $root "build\dinput8.pdb") $x64 -Force -ErrorAction SilentlyContinue }
if (-not (Test-Path (Join-Path $x64 "fs25vr.ini"))) { Copy-Item $ini $x64 }  # keep the player's settings

New-Item -ItemType Directory -Force $mods | Out-Null
Remove-Item $zip -ErrorAction SilentlyContinue
if ($release) {
    Copy-Item (Join-Path $root "mod\FS25_VR.zip") $zip
} else {
    # Build the zip by hand: Windows PowerShell 5.1 (.NET Framework) stores "scripts\VRMain.lua" with
    # a backslash, which the game cannot resolve. Entry names must use forward slashes.
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $src = (Resolve-Path (Join-Path $root "mod\FS25_VR")).Path
    $archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in Get-ChildItem $src -Recurse -File) {
            $name = $f.FullName.Substring($src.Length + 1).Replace('\', '/')
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $name) | Out-Null
        }
    } finally {
        $archive.Dispose()
    }
}

# Files downloaded from the internet carry Windows' "came from another computer" mark. On some
# PCs that stops the game from loading the (unsigned) DLLs: the mod shows up but VR never starts.
foreach ($f in (Join-Path $x64 "dinput8.dll"), (Join-Path $x64 "openxr_loader.dll"), (Join-Path $x64 "fs25vr.ini"), $zip) {
    if (-not (Test-Path $f)) { continue }
    try { Unblock-File -Path $f }
    catch { try { Remove-Item -Path $f -Stream Zone.Identifier -ErrorAction SilentlyContinue } catch { } }
}

if ($AutoResolution) {
    $log = Join-Path $x64 "fs25vr.log"
    $m = if (Test-Path $log) { Select-String -Path $log -Pattern 'recommended render size for the centred frustum: (\d+)x(\d+)' | Select-Object -Last 1 }
    if (-not $m) {
        throw "No recommended size yet. Play once in VR (load a savegame with the headset on), quit the game, then run this again."
    }
    $EyeWidth = [int]$m.Matches[0].Groups[1].Value
    $EyeHeight = [int]$m.Matches[0].Groups[2].Value
    $EyeResolution = $true
    Write-Host "Recommended size for your headset: ${EyeWidth}x${EyeHeight}"
}

if ($EyeResolution) {
    $gameXml = Join-Path $ProfileDir "game.xml"
    $bak = "$gameXml.fs25vr-backup"
    if (-not (Test-Path $bak)) { Copy-Item $gameXml $bak }
    [xml]$x = Get-Content $gameXml
    $d = $x.game.graphic.display
    $d.width = "$EyeWidth"; $d.height = "$EyeHeight"; $d.fullscreenMode = "windowed"
    $d.vsync.InnerText = "false"
    $x.game.graphic.renderer = "D3D_12"
    $x.Save($gameXml)
    Write-Host "game.xml: windowed ${EyeWidth}x${EyeHeight}, vsync off, D3D12 (backup: $bak)"

    # The bridge re-applies this size at every start (renderWidth/renderHeight in fs25vr.ini),
    # so a game update or the settings menu cannot reset it.
    $iniPath = Join-Path $x64 "fs25vr.ini"
    if (Test-Path $iniPath) {
        $text = [IO.File]::ReadAllText($iniPath)
        foreach ($kv in @(@("renderWidth", $EyeWidth), @("renderHeight", $EyeHeight))) {
            $pattern = '(?m)^\s*' + $kv[0] + '\s*=.*$'
            if ($text -match $pattern) { $text = $text -replace $pattern, "$($kv[0])=$($kv[1])" }
            else { $text = $text.TrimEnd() + "`r`n$($kv[0])=$($kv[1])`r`n" }
        }
        [IO.File]::WriteAllText($iniPath, $text)
        Write-Host "fs25vr.ini: render size ${EyeWidth}x${EyeHeight} kept from now on"
    }
}

Write-Host "fs25vr installed:"
Write-Host "  $x64\dinput8.dll, openxr_loader.dll, fs25vr.ini"
Write-Host "  $zip  (enable 'VR (OpenXR, stereoscopic 6DOF)' when starting a savegame)"
