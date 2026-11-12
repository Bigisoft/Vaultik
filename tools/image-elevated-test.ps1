#Requires -Version 7
#Requires -RunAsAdministrator
<#
End-to-end test of disk images with administrator rights. From an elevated PowerShell 7, in the
repository after a release build:

    pwsh -NoProfile -ExecutionPolicy Bypass -File tools\image-elevated-test.ps1

It touches only virtual disk files it makes in its own work folder:
  1. vaultik_tests writes the source disk (2 GiB GPT: an EFI partition of known random bytes and an
     empty NTFS volume from tests\data, so Windows never has to format anything) and a blank target
     disk. The source is attached and test files are written to its NTFS volume (some deleted
     again, so unused clusters hold old data).
  2. vaultik init, an image backup of that disk (by its number, checked to be the attached VHDX), a
     second image after a change (must store little), image info and verify.
  3. Detaches it, restores the image to a second VHDX (--vhdx), attaches that and compares every
     file, the raw EFI partition and the GPT disk GUID; chkdsk of the restored volume.
  4. Restores the image onto the blank target attached as a disk with --to-disk (the guarded path
     that writes to a disk): first with a wrong confirmation (must refuse and leave it blank), then
     with the right one; compares the files again.
  5. vaultik image mount (attached read-only with drive letters) via --json, which closes at once.
  6. Builds the recovery ISO (vaultik recovery-media --iso) into the work folder and checks it.
  7. Detaches everything and deletes the work folder (also on failure).

Every disk it writes to is checked first to be a "File Backed Virtual" disk whose image is one of
its own files. The log goes to -Log (default: image-elevated-test.log in the local temporary folder).
#>
param(
    [string]$Vaultik = (Join-Path $PSScriptRoot '..\build\release\vaultik.exe'),
    [string]$Tests = (Join-Path $PSScriptRoot '..\build\release\vaultik_tests.exe'),
    # Not under a compressed folder: Hyper-V's disk driver refuses compressed virtual disks.
    [string]$Work = (Join-Path $env:LOCALAPPDATA 'Temp\vaultik-image-test'),
    [string]$Log = (Join-Path $env:LOCALAPPDATA 'Temp\image-elevated-test.log'),
    [switch]$SkipIso
)

$ErrorActionPreference = 'Stop'
Start-Transcript -Path $Log -Force | Out-Null
$failures = [System.Collections.Generic.List[string]]::new()
function Pass($m) { Write-Host "PASS  $m" -ForegroundColor Green }
function Fail($m) { Write-Host "FAIL  $m" -ForegroundColor Red; $failures.Add($m) }
function Check($cond, $m) { if ($cond) { Pass $m } else { Fail $m } }

if (-not (Test-Path $Vaultik)) { throw "vaultik.exe not found: $Vaultik" }
if (-not (Test-Path $Tests)) { throw "vaultik_tests.exe not found: $Tests" }
if (Test-Path $Work) { throw "work folder exists already (left from an earlier run?): $Work; remove it first" }
New-Item -ItemType Directory -Path $Work | Out-Null
$vhd1 = Join-Path $Work 'source.vhdx'
$vhd2 = Join-Path $Work 'restored.vhdx'
$vhd3 = Join-Path $Work 'target.vhdx'
$repo = Join-Path $Work 'repo'
$env:VAULTIK_CONFIG_DIR = Join-Path $Work 'config'
$env:VAULTIK_PASSWORD = 'image test password 1'
# The recovery ISO is built in the temporary folder: keep it in the work folder (on an NTFS disk;
# DISM cannot mount an image on some RAM disks), so cleanup removes it too.
$env:TMP = $Work
$env:TEMP = $Work
New-Item -ItemType Directory -Path $env:VAULTIK_CONFIG_DIR | Out-Null

function Get-OwnDisk($path) {
    $img = Get-DiskImage -ImagePath $path
    if (-not $img.Attached) { throw "$path is not attached" }
    $disk = $img | Get-Disk
    if ($disk.BusType -ne 'File Backed Virtual') { throw "disk $($disk.Number) is not a virtual disk ($($disk.BusType))" }
    if ($disk.Location -and ($disk.Location -ne $path)) { throw "disk $($disk.Number) is $($disk.Location), not $path" }
    if ($disk.IsBoot -or $disk.IsSystem) { throw "disk $($disk.Number) is a system disk" }
    return $disk
}

function Get-Manifest($root) {
    $m = @{}
    # System Volume Information (made by Windows on mounting) cannot be read even elevated: skip it
    # before recursing, not after.
    Get-ChildItem -LiteralPath $root -Force | Where-Object { $_.Name -ne 'System Volume Information' } |
        ForEach-Object { if ($_.PSIsContainer) { Get-ChildItem -LiteralPath $_.FullName -Recurse -File -Force } else { $_ } } |
        ForEach-Object {
        $rel = $_.FullName.Substring($root.Length)
        $m[$rel] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }
    return $m
}

function Compare-Manifest($a, $b, $what) {
    $bad = 0
    foreach ($k in $a.Keys) { if ($b[$k] -ne $a[$k]) { $bad++; if ($bad -le 5) { Write-Host "  differs or missing: $k" } } }
    foreach ($k in $b.Keys) { if (-not $a.ContainsKey($k)) { $bad++; if ($bad -le 5) { Write-Host "  extra: $k" } } }
    Check ($bad -eq 0 -and $a.Count -gt 0) "$what ($($a.Count) files)"
}

# SHA-256 of a byte range of a physical disk (unbuffered, 1 MiB aligned reads).
function Get-RawHash($diskNumber, [long]$offset, [long]$length) {
    $fs = [System.IO.FileStream]::new("\\.\PhysicalDrive$diskNumber", [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                                      [System.IO.FileShare]::ReadWrite, 0)
    try {
        $h = [System.Security.Cryptography.IncrementalHash]::CreateHash([System.Security.Cryptography.HashAlgorithmName]::SHA256)
        $buf = [byte[]]::new(1MB)
        $fs.Position = $offset
        $left = $length
        while ($left -gt 0) {
            $n = [int][Math]::Min($left, $buf.Length)
            $got = $fs.Read($buf, 0, $n)
            if ($got -le 0) { throw 'short read' }
            $h.AppendData($buf, 0, $got)
            $left -= $got
        }
        return [Convert]::ToHexString($h.GetHashAndReset())
    } finally { $fs.Dispose() }
}

function Get-DataRoot($diskNumber) {
    for ($i = 0; $i -lt 30; $i++) {
        $p = Get-Partition -DiskNumber $diskNumber | Where-Object { $_.GptType -eq '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}' } | Select-Object -First 1
        if ($p) {
            if (-not $p.DriveLetter -or $p.DriveLetter -eq [char]0) {
                $p | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction SilentlyContinue
                $p = Get-Partition -DiskNumber $diskNumber -PartitionNumber $p.PartitionNumber
            }
            if ($p.DriveLetter -and $p.DriveLetter -ne [char]0) { return "$($p.DriveLetter):\" }
        }
        Start-Sleep -Milliseconds 500
    }
    throw "no data partition with a drive letter on disk $diskNumber"
}

function Dismount-Own($path) {
    # Drive letters given by this script are taken away first: Windows keeps a letter it was
    # given (in MountedDevices) after the disk is gone.
    $img = Get-DiskImage -ImagePath $path -ErrorAction SilentlyContinue
    if ($img -and $img.Attached) {
        $disk = $img | Get-Disk
        if ($disk.BusType -eq 'File Backed Virtual') {
            Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter -and $_.DriveLetter -ne [char]0 } |
                ForEach-Object { $_ | Remove-PartitionAccessPath -AccessPath "$($_.DriveLetter):\" -ErrorAction SilentlyContinue }
        }
    }
    Dismount-DiskImage -ImagePath $path | Out-Null
}

function Run-Vaultik([string[]]$argv) {
    Write-Host "> vaultik $($argv -join ' ')"
    $out = & $Vaultik @argv 2>&1
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host "  $_" }
    return [pscustomobject]@{ Code = $code; Output = ($out -join "`n") }
}

try {
    # ---- 1. the source disk -------------------------------------------------------------------
    # Written by the test program (no formatting by Windows), then attached.
    $env:VAULTIK_TEST_DISK_DIR = $Work
    $made = & $Tests '-tc=image: the test disk*' -s 2>&1
    Remove-Item Env:VAULTIK_TEST_DISK_DIR
    $made | Where-Object { $_ -match 'wrote|ERROR|test cases' } | ForEach-Object { Write-Host "  $_" }
    if (-not ((Test-Path $vhd1) -and (Test-Path $vhd3))) { throw 'vaultik_tests did not write the test disks' }
    Pass 'test disks written without formatting'
    Mount-DiskImage -ImagePath $vhd1 | Out-Null
    $d1 = Get-OwnDisk $vhd1
    Update-Disk -Number $d1.Number
    $root1 = Get-DataRoot $d1.Number
    Write-Host "source disk $($d1.Number), data at $root1"
    $rng = [System.Random]::new(42)
    New-Item -ItemType Directory -Path (Join-Path $root1 'docs\sub') -Force | Out-Null
    for ($i = 0; $i -lt 60; $i++) {
        $size = $rng.Next(1, 4MB)
        $bytes = [byte[]]::new($size); $rng.NextBytes($bytes)
        $dir = if ($i % 3 -eq 0) { 'docs\sub' } else { 'docs' }
        [System.IO.File]::WriteAllBytes((Join-Path $root1 "$dir\file$i.bin"), $bytes)
    }
    $big = [byte[]]::new(150MB); $rng.NextBytes($big)
    [System.IO.File]::WriteAllBytes((Join-Path $root1 'big.bin'), $big)
    Set-Content -Path (Join-Path $root1 'docs\hello.txt') -Value 'Hello from the disk image test' -Encoding utf8
    # Old data in unused clusters: written, then deleted.
    $junk = [byte[]]::new(100MB); $rng.NextBytes($junk)
    [System.IO.File]::WriteAllBytes((Join-Path $root1 'deleted.bin'), $junk)
    Remove-Item (Join-Path $root1 'deleted.bin')
    $manifest = Get-Manifest $root1
    $espPart = Get-Partition -DiskNumber $d1.Number | Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' }
    $guid1 = (Get-Disk -Number $d1.Number).Guid
    Write-Host "manifest: $($manifest.Count) files; disk GUID $guid1"

    # ---- 2. image backups ---------------------------------------------------------------------
    $r = Run-Vaultik @('init', '-r', $repo)
    Check ($r.Code -eq 0) 'init'
    $r = Run-Vaultik @('image', 'disks')
    Check ($r.Output -match "Disk $($d1.Number):") 'image disks lists the test disk'
    $r = Run-Vaultik @('image', 'backup', '-r', $repo, "$($d1.Number)")
    Check ($r.Code -eq 0) 'first image backup'
    Check ($r.Output -match 'Disk image [0-9a-f]{8} saved') 'first image saved'
    Check ($r.Output -notmatch 'read while in use') 'read from a shadow copy'
    # The ESP hash now (it is read raw; nothing changes it after the backup).
    $espHash1 = Get-RawHash $d1.Number $espPart.Offset $espPart.Size
    # A change, then a second image: it must store little.
    Set-Content -Path (Join-Path $root1 'docs\hello.txt') -Value 'Changed after the first image' -Encoding utf8
    $manifest = Get-Manifest $root1
    $r = Run-Vaultik @('image', 'backup', '-r', $repo, "$($d1.Number)")
    Check ($r.Code -eq 0) 'second image backup'
    if ($r.Output -match 'new data: ([0-9.]+) (B|KiB|MiB|GiB)') {
        $n = [double]$Matches[1]; $u = $Matches[2]
        $mib = switch ($u) { 'B' { $n / 1MB } 'KiB' { $n / 1024 } 'MiB' { $n } 'GiB' { $n * 1024 } }
        Check ($mib -lt 64) "second image stores little new data ($n $u)"
    } else { Fail 'second image: no new-data line' }
    $r = Run-Vaultik @('image', 'info', '-r', $repo, 'latest')
    Check ($r.Code -eq 0 -and $r.Output -match 'NTFS') 'image info'
    $r = Run-Vaultik @('verify', '-r', $repo, '--read-data')
    Check ($r.Code -eq 0) 'verify --read-data'
    $r = Run-Vaultik @('snapshots', '-r', $repo)
    Dismount-Own $vhd1

    # ---- 3. restore to a VHDX -----------------------------------------------------------------
    $r = Run-Vaultik @('image', 'restore', '-r', $repo, 'latest', '--vhdx', $vhd2)
    Check ($r.Code -eq 0 -and (Test-Path $vhd2)) 'restore to a VHDX'
    Mount-DiskImage -ImagePath $vhd2 | Out-Null
    $d2 = Get-OwnDisk $vhd2
    Check ((Get-Disk -Number $d2.Number).Guid -eq $guid1) 'restored VHDX keeps the disk GUID'
    $root2 = Get-DataRoot $d2.Number
    Compare-Manifest $manifest (Get-Manifest $root2) 'files in the restored VHDX match'
    $esp2 = Get-Partition -DiskNumber $d2.Number | Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' }
    Check ($esp2 -and (Get-RawHash $d2.Number $esp2.Offset $esp2.Size) -eq $espHash1) 'EFI partition restored byte for byte'
    $r = & chkdsk.exe ($root2.TrimEnd('\')) 2>&1
    Check ($LASTEXITCODE -eq 0) "chkdsk of the restored NTFS volume (exit $LASTEXITCODE)"
    Dismount-Own $vhd2

    # ---- 4. restore onto an attached blank disk ----------------------------------------------
    Mount-DiskImage -ImagePath $vhd3 | Out-Null
    $d3 = Get-OwnDisk $vhd3
    Check ($d3.PartitionStyle -eq 'RAW') 'target disk starts blank'
    $r = Run-Vaultik @('image', 'restore', '-r', $repo, 'latest', '--to-disk', "$($d3.Number)", '--confirm', 'yes please')
    Check ($r.Code -ne 0 -and $r.Output -match 'not confirmed') 'a wrong confirmation is refused'
    Check ((Get-Disk -Number $d3.Number).PartitionStyle -eq 'RAW') 'and the disk is left alone'
    $d3 = Get-OwnDisk $vhd3  # checked again right before writing
    $r = Run-Vaultik @('image', 'restore', '-r', $repo, 'latest', '--to-disk', "$($d3.Number)", '--confirm', "ERASE DISK $($d3.Number)")
    Check ($r.Code -eq 0) 'restore onto the blank disk'
    Update-Disk -Number $d3.Number -ErrorAction SilentlyContinue
    $root3 = Get-DataRoot $d3.Number
    Compare-Manifest $manifest (Get-Manifest $root3) 'files on the restored disk match'
    Dismount-Own $vhd3

    # ---- 5. image mount -----------------------------------------------------------------------
    $mountOut = '' | & $Vaultik image mount -r $repo latest --json --target (Join-Path $Work 'mnt') 2>&1
    $mountOut | ForEach-Object { Write-Host "  $_" }
    $mounted = $mountOut | Where-Object { $_ -match '"event":"mounted"' } | Select-Object -First 1
    Check ($mounted -and $mounted -match '"drives":\["[A-Z]:') 'image mount attaches the image with drive letters'
    Check (-not (Get-ChildItem (Join-Path $Work 'mnt') -Filter *.vhdx -ErrorAction SilentlyContinue)) 'image mount removes its temporary disk'

    # ---- 6. recovery ISO ----------------------------------------------------------------------
    if (-not $SkipIso) {
        $iso = Join-Path $Work 'vaultik-recovery.iso'
        $r = Run-Vaultik @('recovery-media', '--iso', $iso)
        Check ($r.Code -eq 0 -and (Test-Path $iso) -and (Get-Item $iso).Length -gt 200MB) 'recovery ISO built'
        Check ($r.Output -notmatch 'lacks libraries') 'Windows PE has every library vaultik.exe needs at start'
        if ($r.Output -match 'warning') { Write-Host 'Note the warnings above (delay-loaded libraries missing in Windows PE).' }
    }
}
catch {
    Fail "stopped: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
}
finally {
    foreach ($v in @($vhd1, $vhd2, $vhd3)) {
        if (Test-Path $v) { try { Dismount-Own $v } catch {} }
    }
    # A Windows PE image left mounted by an interrupted ISO build.
    $mounted = & dism.exe /Get-MountedWimInfo 2>$null | Select-String -Pattern 'Mount Dir : (.*vaultik-winpe-.*)'
    foreach ($m in $mounted) { & dism.exe /Unmount-Image "/MountDir:$($m.Matches[0].Groups[1].Value.Trim())" /Discard | Out-Null }
    Start-Sleep -Seconds 1
    Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path $Work) { Write-Host "Could not remove $Work completely; remove it by hand." }
    Write-Host ''
    if ($failures.Count -eq 0) { Write-Host 'ALL PASSED' -ForegroundColor Green }
    else { Write-Host "$($failures.Count) FAILED:" -ForegroundColor Red; $failures | ForEach-Object { Write-Host "  $_" } }
    Stop-Transcript | Out-Null
}
