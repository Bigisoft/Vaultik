# Builds the release programs and packs them for handing out:
#   dist\Vaultik-<version>-portable.zip   unzip anywhere (an external drive); keeps its settings next to it
#   dist\Vaultik-<version>.msi            installs into Program Files
# Usage: powershell -File tools\package.ps1 [-NoBuild]
# The MSI needs the .NET SDK; WiX comes from NuGet on the first run.
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
Copy-Item -Force (Join-Path $out 'Vaultik.msi') $msi
Remove-Item -Recurse -Force $out

Get-Item $zip, $msi | ForEach-Object { '{0}  {1:N1} MB' -f $_.Name, ($_.Length / 1MB) }
