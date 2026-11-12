#Requires -Version 7
<#
Tests Vaultik's installer (MSI) in Windows Sandbox, a throwaway Windows that is gone when it
closes, so nothing is installed on this computer (an uninstall here would also remove the real
Vaultik scheduled tasks). After tools\package.ps1, from the repository:

    pwsh -File tools\msi-sandbox-test.ps1                          # the newest MSI in dist
    pwsh -File tools\msi-sandbox-test.ps1 -OldMsi dist\Vaultik-0.7.0.msi -Wizard

Inside the Sandbox (no network; it reads only the MSIs and programs given and writes only to the
results folder), tools\msi-sandbox-test-inner.ps1:
  1. checks the installer refuses C:\, C:\Program Files\ and C:\Windows\ as its folder;
  2. installs -OldMsi first, if given, so the new one is tested as an upgrade;
  3. installs the MSI (with -Wizard through its pages, clicked by you; otherwise with a progress
     window only) and checks the folder, programs (the same as -Bin), Apps and features, a repair,
     PATH, the folder's permissions, Start menu, Explorer menu and Event Log source;
  4. runs vaultik from PATH: init, backup, restore and compare, and starts the window;
  5. uninstalls and checks everything is gone (scheduled tasks too) but backups and settings stay.
Every step shows in a console window in the Sandbox, which closes itself at the end. The result
is printed here and the exit code is 0 only when everything passed. Needs Windows Sandbox (the
"Windows Sandbox" optional feature); only one Sandbox can run at a time.
#>
param(
    [string]$Msi,
    [string]$OldMsi,
    [string]$Bin = (Join-Path $PSScriptRoot '..\build\release'),
    [switch]$Wizard,
    [string]$Work = (Join-Path $env:LOCALAPPDATA 'Temp\vaultik-msi-test'),
    [int]$TimeoutMinutes = 60
)
$ErrorActionPreference = 'Stop'

function Version-Of([string]$path) {
    if ((Split-Path $path -Leaf) -notmatch '^Vaultik-(\d+\.\d+\.\d+)\.msi$') { throw "not a Vaultik-<version>.msi: $path" }
    return $Matches[1]
}
if (-not $Msi) {
    $Msi = Get-ChildItem (Join-Path $PSScriptRoot '..\dist') -Filter 'Vaultik-*.msi' |
        Where-Object Name -match '^Vaultik-\d+\.\d+\.\d+\.msi$' |
        Sort-Object { [version](Version-Of $_.Name) } | Select-Object -Last 1 -ExpandProperty FullName
    if (-not $Msi) { throw 'no MSI in dist; run tools\package.ps1 first' }
}
$Msi = (Resolve-Path $Msi).Path
$version = Version-Of $Msi
$oldVersion = $null
if ($OldMsi) {
    $OldMsi = (Resolve-Path $OldMsi).Path
    $oldVersion = Version-Of $OldMsi
    if ([version]$oldVersion -ge [version]$version) { throw "-OldMsi ($oldVersion) must be older than $version" }
}
$Bin = (Resolve-Path $Bin).Path
$built = (& (Join-Path $Bin 'vaultik.exe') --version) -join ' '
if ($built -notmatch [regex]::Escape($version)) { throw "$Bin holds $built, not ${version}: give -Bin the programs the MSI was made from" }
$sandbox = Join-Path $env:windir 'System32\WindowsSandbox.exe'
if (-not (Test-Path $sandbox)) { throw 'Windows Sandbox is not installed (turn on the "Windows Sandbox" Windows feature)' }
if (Get-Process WindowsSandboxRemoteSession -ErrorAction SilentlyContinue) { throw 'a Windows Sandbox is open already; close it first' }

# The folders the Sandbox sees: in (read-only) and out (results).
$in = Join-Path $Work 'in'
$out = Join-Path $Work 'out'
if (Test-Path $Work) { Remove-Item -LiteralPath $Work -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $in 'bin'), $out | Out-Null
Copy-Item $Msi (Join-Path $in 'new.msi')
if ($OldMsi) { Copy-Item $OldMsi (Join-Path $in 'old.msi') }
foreach ($f in 'vaultik.exe', 'vaultik-gui.exe', 'vaultik-task.exe') { Copy-Item (Join-Path $Bin $f) (Join-Path $in 'bin') }
Copy-Item (Join-Path $PSScriptRoot 'msi-sandbox-test-inner.ps1') $in
@{ version = $version; oldVersion = $oldVersion; wizard = [bool]$Wizard } | ConvertTo-Json | Set-Content (Join-Path $in 'test.json')

$wsb = Join-Path $Work 'msi-test.wsb'
@"
<Configuration>
  <Networking>Disable</Networking>
  <vGPU>Disable</vGPU>
  <ClipboardRedirection>Disable</ClipboardRedirection>
  <AudioInput>Disable</AudioInput>
  <VideoInput>Disable</VideoInput>
  <PrinterRedirection>Disable</PrinterRedirection>
  <MappedFolders>
    <MappedFolder><HostFolder>$in</HostFolder><SandboxFolder>C:\msitest\in</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>$out</HostFolder><SandboxFolder>C:\msitest\out</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>
  </MappedFolders>
  <LogonCommand>
    <Command>cmd.exe /c start "Vaultik installer test" powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\msitest\in\msi-sandbox-test-inner.ps1</Command>
  </LogonCommand>
</Configuration>
"@ | Set-Content $wsb -Encoding utf8

Write-Host "Testing $(Split-Path $Msi -Leaf)$(if ($OldMsi) { " as an upgrade from $oldVersion" }) in Windows Sandbox..."
if ($Wizard) { Write-Host 'Click through the installer when it opens in the Sandbox.' }
Start-Process $wsb
$done = Join-Path $out 'done'
$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
while (-not (Test-Path $done) -and (Get-Date) -lt $deadline) { Start-Sleep 10 }
$result = Join-Path $out 'result.txt'
if (Test-Path $result) { Get-Content $result } else { Write-Host 'no result was written' }
Write-Host "Logs: $out"
if (-not (Test-Path $done)) { Write-Host "timed out after $TimeoutMinutes minutes"; exit 2 }
exit $(if ((Get-Content $result -Raw) -match 'ALL PASSED') { 0 } else { 1 })
