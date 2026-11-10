# Builds the release programs and packs them for handing out:
#   dist\Vaultik-<version>-portable.zip   unzip anywhere (an external drive); keeps its settings next to it
#   dist\Vaultik-<version>.msi            installs into Program Files
#   dist\SHA256SUMS.txt                   the SHA-256 of both, to check a download against
# Usage: powershell -File tools\package.ps1 [-NoBuild]
# The MSI needs the .NET SDK; WiX comes from NuGet on the first run.
#
# Code signing (optional; Windows SmartScreen warns about unsigned downloads): with a code signing
# certificate in the current user's certificate store, set VAULTIK_SIGN_THUMBPRINT to its
# thumbprint; with a .pfx file, set VAULTIK_SIGN_PFX (and VAULTIK_SIGN_PFX_PASSWORD). The programs
# are then signed before they are packed, and the MSI after it is built, all with an RFC 3161
# timestamp (VAULTIK_SIGN_TIMESTAMP, default DigiCert's), so the signatures outlive the
# certificate. signtool.exe comes from the Windows SDK.
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root 'build\release'
$dist = Join-Path $root 'dist'

if (-not $NoBuild) {
    cmd /c "cd /d `"$root`" && .\build.cmd release"
    if ($LASTEXITCODE -ne 0) { throw "the release build failed" }
}
$programs = 'vaultik.exe', 'vaultik-gui.exe', 'vaultik-task.exe'
foreach ($p in $programs) { if (-not (Test-Path (Join-Path $bin $p))) { throw "missing $bin\$p" } }
$version = ((& (Join-Path $bin 'vaultik.exe') --version) -split ' ')[1]
New-Item -ItemType Directory -Force $dist | Out-Null

$signArgs = $null
if ($env:VAULTIK_SIGN_THUMBPRINT) {
    $signArgs = @('/sha1', $env:VAULTIK_SIGN_THUMBPRINT)
} elseif ($env:VAULTIK_SIGN_PFX) {
    $signArgs = @('/f', $env:VAULTIK_SIGN_PFX)
    if ($env:VAULTIK_SIGN_PFX_PASSWORD) { $signArgs += @('/p', $env:VAULTIK_SIGN_PFX_PASSWORD) }
}
$signtool = $null
if ($signArgs) {
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $signtool = Get-ChildItem -Path $kits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like '*\x64\signtool.exe' } | Sort-Object FullName | Select-Object -Last 1
    if (-not $signtool) { throw "signing was asked for, but signtool.exe (Windows SDK) was not found" }
}
$timestamp = if ($env:VAULTIK_SIGN_TIMESTAMP) { $env:VAULTIK_SIGN_TIMESTAMP } else { 'http://timestamp.digicert.com' }
function Sign-Files([string[]]$files) {
    if (-not $signtool) { return }
    & $signtool.FullName sign @signArgs /fd SHA256 /tr $timestamp /td SHA256 /d 'Vaultik' @files
    if ($LASTEXITCODE -ne 0) { throw "signing failed: $($files -join ', ')" }
    & $signtool.FullName verify /pa @files | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "a signature does not verify: $($files -join ', ')" }
}
Sign-Files ($programs | ForEach-Object { Join-Path $bin $_ })

# Portable: the programs, the "portable" marker and a short read-me, in a folder named Vaultik.
$stage = Join-Path $dist 'stage'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
$folder = Join-Path $stage 'Vaultik'
New-Item -ItemType Directory -Force $folder | Out-Null
foreach ($p in $programs) { Copy-Item (Join-Path $bin $p) $folder }
Set-Content -Encoding utf8 (Join-Path $folder 'portable') @"
This file makes this copy of Vaultik portable: its jobs, history and settings are kept in the
Settings folder next to it, and nothing is written to the computer it runs on. Delete this file
to use the settings of the account that runs it instead.
"@
Set-Content -Encoding utf8 (Join-Path $folder 'Read me.txt') @"
Vaultik $version, portable

Start vaultik-gui.exe. Everything Vaultik saves stays in the Settings folder here, so this folder
can live on an external drive and be used on any Windows PC.

On another PC Vaultik asks once for each backup's password: saved passwords are sealed for the
Windows account that saved them. Jobs that run on a schedule or with administrator rights need
Vaultik installed on that PC (Vaultik-$version.msi).
"@
$zip = Join-Path $dist "Vaultik-$version-portable.zip"
Remove-Item -Force $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $folder -DestinationPath $zip
Remove-Item -Recurse -Force $stage

# Installer.
$out = Join-Path $dist 'msi'
dotnet build (Join-Path $root 'installer\Vaultik.wixproj') -c Release -nologo -v quiet `
    "-p:Version=$version" "-p:BinDir=$bin" -o $out
if ($LASTEXITCODE -ne 0) { throw "the installer build failed" }
$msi = Join-Path $dist "Vaultik-$version.msi"
# (with the folder page's wording in Package.wxl the MSI is built per language)
Copy-Item -Force (Join-Path (Join-Path $out 'en-US') 'Vaultik.msi') $msi
Remove-Item -Recurse -Force $out
Sign-Files @($msi)

# Checksums, for the release page: "certutil -hashfile <file> SHA256" shows a download's.
$sums = Join-Path $dist 'SHA256SUMS.txt'
Get-Item $zip, $msi | ForEach-Object { '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLower(), $_.Name } |
    Set-Content -Encoding ascii $sums

Get-Item $zip, $msi | ForEach-Object { '{0}  {1:N1} MB' -f $_.Name, ($_.Length / 1MB) }
if (-not $signtool) { 'Not signed: set VAULTIK_SIGN_THUMBPRINT or VAULTIK_SIGN_PFX to sign (see the top of this script).' }
