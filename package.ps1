<#
  Builds a release package: release\fs25vr-<version>.zip containing

    x64\dinput8.dll, x64\openxr_loader.dll, x64\fs25vr.ini   (copy into <game>\x64)
    mod\FS25_VR.zip                                         (copy into your mods folder)
    HOW TO INSTALL.txt, INSTALL.bat, UNINSTALL.bat, SET VR RESOLUTION.bat, install.ps1, install.sh,
    README.md, LICENSE, THIRD_PARTY_NOTICES.md, LICENSE-openxr-loader.txt
    SHA256SUMS.txt

  Run build.bat first.
#>
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
[xml]$desc = Get-Content (Join-Path $root "mod\FS25_VR\modDesc.xml")
$version = $desc.modDesc.version
$stage = Join-Path $root "release\fs25vr-$version"
$out = Join-Path $root "release\fs25vr-$version.zip"

if (-not (Test-Path (Join-Path $root "build\dinput8.dll"))) { throw "Run build.bat first" }
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $out -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $stage "x64"), (Join-Path $stage "mod") | Out-Null

Copy-Item (Join-Path $root "build\dinput8.dll") (Join-Path $stage "x64")
Copy-Item (Join-Path $root "third_party\openxr\x64\bin\openxr_loader.dll") (Join-Path $stage "x64")
Copy-Item (Join-Path $root "dist\fs25vr.ini") (Join-Path $stage "x64")
foreach ($f in "install.ps1", "install.sh", "README.md", "LICENSE", "THIRD_PARTY_NOTICES.md") { Copy-Item (Join-Path $root $f) $stage }
Copy-Item (Join-Path $root "dist\release\*") $stage   # INSTALL.bat, UNINSTALL.bat, SET VR RESOLUTION.bat, HOW TO INSTALL.txt
Copy-Item (Join-Path $root "third_party\openxr\share\doc\openxr\LICENSE") (Join-Path $stage "LICENSE-openxr-loader.txt")

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
function New-Zip($srcDir, $zipPath) {
    $srcDir = (Resolve-Path $srcDir).Path
    $archive = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in Get-ChildItem $srcDir -Recurse -File) {
            $name = $f.FullName.Substring($srcDir.Length + 1).Replace('\', '/')  # forward slashes only
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $name) | Out-Null
        }
    } finally {
        $archive.Dispose()
    }
}

New-Zip (Join-Path $root "mod\FS25_VR") (Join-Path $stage "mod\FS25_VR.zip")

function Get-Sha256($path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [System.IO.File]::OpenRead($path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace('-', '').ToLower() }
    finally { $fs.Dispose(); $sha.Dispose() }
}

$sums = Get-ChildItem $stage -Recurse -File | ForEach-Object {
    "{0}  {1}" -f (Get-Sha256 $_.FullName), $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
}
$sums | Set-Content (Join-Path $stage "SHA256SUMS.txt")

New-Zip $stage $out
Write-Host "release: $out"
Write-Host "sha256:  $(Get-Sha256 $out)"
