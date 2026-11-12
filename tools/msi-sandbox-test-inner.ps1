# Runs inside Windows Sandbox, started by tools\msi-sandbox-test.ps1 (see there). Windows
# PowerShell 5.1: the Sandbox has no PowerShell 7. Installs, upgrades, uses and removes Vaultik's
# MSI, writing PASS/FAIL lines to C:\msitest\out\result.txt and the console, then shuts the
# Sandbox down.
$ErrorActionPreference = 'Continue'
$in = 'C:\msitest\in'
$out = 'C:\msitest\out'
$log = Join-Path $out 'result.txt'
$script:fails = 0
function L([string]$t) {
    $t | Out-File $log -Append -Encoding utf8
    $c = if ($t -like 'FAIL*') { 'Red' } elseif ($t -like 'PASS*') { 'Green' } else { 'Gray' }
    Write-Host $t -ForegroundColor $c
}
function Check([bool]$ok, [string]$what) { if ($ok) { L "PASS  $what" } else { L "FAIL  $what"; $script:fails++ } }
function Msi([string]$a) {
    $p = Start-Process msiexec.exe -ArgumentList $a -Wait -PassThru
    return $p.ExitCode
}
function MachinePath { [Environment]::GetEnvironmentVariable('PATH', 'Machine') }
function Products {
    @(Get-ItemProperty HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\* -ErrorAction SilentlyContinue |
        Where-Object DisplayName -eq 'Vaultik')
}
"start $(Get-Date)" | Out-File $log -Encoding utf8
try {
    $cfg = Get-Content (Join-Path $in 'test.json') -Raw | ConvertFrom-Json
    $new = Join-Path $in 'new.msi'
    $old = Join-Path $in 'old.msi'
    $hasOld = Test-Path $old
    $dir = 'C:\Program Files\Vaultik'
    L "testing Vaultik $($cfg.version)$(if ($hasOld) { " (upgrade from $($cfg.oldVersion))" })"

    # ---- 1. folders the installer must refuse (it would make them administrator-only)
    foreach ($bad in 'C:\', 'C:\Program Files\', 'C:\Windows\') {
        $name = ($bad -replace '[:\\ ]', '')
        $c = Msi "/i `"$new`" /qn INSTALLFOLDER=`"$bad`" /l*v `"$out\refuse-$name.log`""
        $said = (Get-Content "$out\refuse-$name.log" -Raw -Encoding Unicode) -match 'cannot be installed directly in'
        Check ($c -ne 0 -and $said) "refuses to install directly in $bad (exit $c)"
        Check (-not (Test-Path (Join-Path $bad 'vaultik.exe'))) '  and wrote nothing there'
    }
    Check (@(Products).Count -eq 0) 'nothing installed after the refusals'

    # ---- 2. the older version, if given, into the default folder
    if ($hasOld) {
        L "Installing $($cfg.oldVersion) into the default folder (progress window only)..."
        $c = Msi "/i `"$old`" /qb /l*v `"$out\install-old.log`""
        Check ($c -eq 0) "$($cfg.oldVersion) installs (exit $c)"
        Check ((& "$dir\vaultik.exe" --version) -match [regex]::Escape($cfg.oldVersion)) "$($cfg.oldVersion) runs from $dir (the default)"
    }

    # ---- 3. this version: through the wizard, or with a progress window only
    if ($cfg.wizard) {
        L ''
        L ">>> The $($cfg.version) installer opens now: click through it. Its folder box should show $dir\"
        L '>>> Try typing C:\ there once if you like: it must be refused (the wizard then opens again).'
        $c = Msi "/i `"$new`" /l*v `"$out\install.log`""
    } else {
        L "Installing $($cfg.version) (progress window only)..."
        $c = Msi "/i `"$new`" /qb /l*v `"$out\install.log`""
    }
    $root = (Get-ItemProperty HKLM:\Software\Vaultik -ErrorAction SilentlyContinue).InstallDir
    if ($cfg.wizard -and ($c -ne 0 -or -not $root)) {
        L "      the wizard ended with exit $c (a refused folder, or cancelled); it opens again"
        $c = Msi "/i `"$new`" /l*v `"$out\install-again.log`""
        $root = (Get-ItemProperty HKLM:\Software\Vaultik -ErrorAction SilentlyContinue).InstallDir
    }
    Check ($c -eq 0) "$($cfg.version) installs$(if ($hasOld) { " over $($cfg.oldVersion)" }) (exit $c)"
    L "      install folder: $root"
    $newDir = "$root".TrimEnd('\')
    $ver = & "$newDir\vaultik.exe" --version
    Check ($ver -match [regex]::Escape($cfg.version)) "$($cfg.version) runs from $newDir ($ver)"
    if ($hasOld -and $newDir -ne $dir) {
        Check (-not (Test-Path "$dir\vaultik.exe")) "the old copy in $dir was removed"
    }
    $dir = $newDir
    $p = @(Products)
    Check ($p.Count -eq 1 -and $p[0].DisplayVersion -eq $cfg.version) "one Vaultik in Apps and features ($($p.DisplayVersion -join ', '))"
    foreach ($f in 'vaultik.exe', 'vaultik-gui.exe', 'vaultik-task.exe') {
        Check ((Get-FileHash "$dir\$f").Hash -eq (Get-FileHash "$in\bin\$f").Hash) "$f is the build that was given"
    }
    Check ([bool]$root) 'the install folder is recorded for the next upgrade'

    # A scheduled task as Vaultik makes them; it must survive a reinstall and go with the uninstall.
    $act = New-ScheduledTaskAction -Execute "$dir\vaultik-task.exe" -Argument '--help'
    $made = Register-ScheduledTask -TaskPath '\Vaultik\' -TaskName 'msi-test job' -Action $act -Trigger (New-ScheduledTaskTrigger -Daily -At 3am) -Force
    Check ([bool]$made) 'a test scheduled task made'
    $c = Msi "/fa `"$new`" /qb /l*v `"$out\repair.log`""
    Check ($c -eq 0) "a repair (reinstall) of the same version works (exit $c)"
    Check ([bool](Get-ScheduledTask -TaskPath '\Vaultik\' -TaskName 'msi-test job' -ErrorAction SilentlyContinue)) '  and keeps the scheduled task'

    $pathParts = (MachinePath) -split ';'
    Check ($pathParts -contains "$dir\" -or $pathParts -contains $dir) 'the folder is on PATH'
    Check (@($pathParts | Where-Object { $_ -like '*Vaultik*' }).Count -eq 1) 'on PATH once'
    $sddl = (Get-Acl $dir).Sddl
    L "      folder permissions: $sddl"
    $others = [regex]::Matches($sddl, '\(A;[^)]*;;;(BU|AU|WD|IU)\)') | Where-Object { $_.Value -ne '(A;OICI;0x1200a9;;;BU)' }
    Check ($sddl -match '^O:BA' -and $sddl -match 'D:PA?I?\(' -and $sddl -match '\(A;OICI;0x1200a9;;;BU\)' -and -not $others) 'the folder is administrator-only (users may only run it)'
    $lnk = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Vaultik.lnk'
    Check (Test-Path $lnk) 'Start menu entry'
    reg query 'HKLM\Software\Classes\*\shell\Vaultik.History' 2>$null | Out-Null
    Check ($LASTEXITCODE -eq 0) 'Explorer menu "Earlier versions in Vaultik"'
    Check (Test-Path HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\Application\Vaultik) 'Event Log source'

    # ---- 4. use it: vaultik found on PATH in a new process
    $env:PATH = (MachinePath) + ';' + [Environment]::GetEnvironmentVariable('PATH', 'User')
    $env:VAULTIK_CONFIG_DIR = 'C:\msitest\cfg'
    $env:VAULTIK_PASSWORD = 'sandbox test password'
    $data = 'C:\msitest\data'
    New-Item -ItemType Directory -Force "$data\sub" | Out-Null
    1..20 | ForEach-Object { [IO.File]::WriteAllBytes("$data\f$_.bin", [byte[]](1..(1000 * $_) | ForEach-Object { $_ % 251 })) }
    'hello' | Out-File "$data\sub\note.txt"
    $o = vaultik init -r C:\msitest\repo 2>&1; Check ($LASTEXITCODE -eq 0) 'vaultik init (from PATH)'
    $o = vaultik backup -r C:\msitest\repo $data 2>&1; Check ($LASTEXITCODE -eq 0) 'vaultik backup'
    if ($LASTEXITCODE -ne 0) { $o | ForEach-Object { L "      $_" } }
    $o = vaultik restore -r C:\msitest\repo latest --target C:\msitest\restored 2>&1; Check ($LASTEXITCODE -eq 0) 'vaultik restore'
    if ($LASTEXITCODE -ne 0) { $o | ForEach-Object { L "      $_" } }
    $src = Get-ChildItem $data -Recurse -File
    $bad = 0
    foreach ($f in $src) {
        $r = Get-ChildItem C:\msitest\restored -Recurse -File -Filter $f.Name | Where-Object { $_.FullName.EndsWith($f.FullName.Substring(2)) } | Select-Object -First 1
        if (-not $r -or (Get-FileHash $r.FullName).Hash -ne (Get-FileHash $f.FullName).Hash) { $bad++ }
    }
    Check ($bad -eq 0) "restored files match ($($src.Count) files, $bad different)"
    $g = Start-Process "$dir\vaultik-gui.exe" -PassThru
    Start-Sleep 6
    $alive = -not $g.HasExited
    Check $alive "the window starts and stays open$(if (-not $alive) { " (exit $($g.ExitCode))" })"
    if ($alive) { $g | Stop-Process -Force }
    Start-Sleep 2

    # ---- 5. remove it
    New-Item -ItemType Directory -Force C:\msitest\cfg | Out-Null
    'settings' | Out-File C:\msitest\cfg\jobs-marker.txt
    $code = @(Products)[0].PSChildName
    L 'Uninstalling (progress window only)...'
    $c = Msi "/x $code /qb /l*v `"$out\uninstall.log`""
    Check ($c -eq 0) "uninstall (exit $c)"
    Check (-not (Test-Path "$dir\vaultik.exe")) 'the programs are gone'
    Check (-not (Test-Path $lnk)) 'the Start menu entry is gone'
    Check (@((MachinePath) -split ';' | Where-Object { $_ -like '*Vaultik*' }).Count -eq 0) 'PATH no longer has Vaultik'
    reg query 'HKLM\Software\Classes\*\shell\Vaultik.History' 2>$null | Out-Null
    Check ($LASTEXITCODE -ne 0) 'the Explorer menu is gone'
    Check (-not (Get-ScheduledTask -TaskPath '\Vaultik\' -ErrorAction SilentlyContinue)) 'Vaultik scheduled tasks are removed'
    Check (@(Products).Count -eq 0) 'Vaultik is gone from Apps and features'
    Check (@(Get-ChildItem C:\msitest\repo -Recurse -File).Count -gt 0) 'the backup itself stays'
    Check (Test-Path C:\msitest\cfg\jobs-marker.txt) 'jobs and settings stay'
} catch {
    L "FAIL  stopped: $_ at line $($_.InvocationInfo.ScriptLineNumber)"; $script:fails++
}
L $(if ($script:fails -eq 0) { 'ALL PASSED' } else { "$($script:fails) FAILED" })
New-Item -ItemType File (Join-Path $out 'done') -Force | Out-Null
Write-Host ''
Write-Host 'Done. This Sandbox closes in 30 seconds (or close it yourself).'
Start-Sleep 30
shutdown.exe /s /t 0
