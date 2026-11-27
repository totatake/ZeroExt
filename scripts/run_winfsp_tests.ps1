[CmdletBinding()]
param(
    [string]$WinFspTestsExe = "",
    [string]$DriveLetter = "",
    [int]$SizeMb = 64,
    [string[]]$Modes = @("rw-default", "rw-fast-io"),
    [string[]]$TestArgs = @(),
    [switch]$SkipE2fsck
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ZeroExt4Exe = Join-Path $RepoRoot "build\zeroext4.exe"
if (-not (Test-Path $ZeroExt4Exe)) {
    throw "zeroext4.exe not found at $ZeroExt4Exe. Build first with scripts\build_and_test.ps1."
}

# Locate winfsp-tests-x64.exe
if ([string]::IsNullOrWhiteSpace($WinFspTestsExe)) {
    $candidates = @(
        (Join-Path $env:TEMP "winfsp-upstream\build\VStudio\testing\build\Release\winfsp-tests-x64.exe"),
        "C:\Program Files (x86)\WinFsp\bin\winfsp-tests-x64.exe",
        "C:\Program Files\WinFsp\bin\winfsp-tests-x64.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) {
            $WinFspTestsExe = $c
            break
        }
    }
    if ([string]::IsNullOrWhiteSpace($WinFspTestsExe)) {
        $cmd = Get-Command winfsp-tests-x64.exe -ErrorAction SilentlyContinue
        if ($cmd) { $WinFspTestsExe = $cmd.Source }
    }
}
if ([string]::IsNullOrWhiteSpace($WinFspTestsExe) -or -not (Test-Path $WinFspTestsExe)) {
    throw "winfsp-tests-x64.exe not found. Pass -WinFspTestsExe <path>."
}
$WinFspTestsExe = (Resolve-Path $WinFspTestsExe).Path

# Ensure winfsp-x64.dll directory is in PATH for winfsp-tests-x64.exe
$dllDirs = @(
    (Join-Path $env:TEMP "winfsp-upstream\build\VStudio\build\Release"),
    "C:\Program Files (x86)\WinFsp\bin"
)
foreach ($d in $dllDirs) {
    if ((Test-Path (Join-Path $d "winfsp-x64.dll")) -and ($env:PATH -notlike "*$d*")) {
        $env:PATH = "$d;$env:PATH"
    }
}
Write-Host "Using winfsp-tests-x64.exe: $WinFspTestsExe"

if (-not $SkipE2fsck) {
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $e2fsckCheck = (& wsl.exe -- which e2fsck 2>&1 | ForEach-Object { "$_" }) -join "`n"
        $whichExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevEap
    }
    if ($whichExit -ne 0) {
        throw "e2fsck is not available inside WSL2: $e2fsckCheck"
    }
}

# Pick an unused drive letter
$usedDrives = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($d in [System.IO.DriveInfo]::GetDrives()) {
    [void]$usedDrives.Add($d.Name.TrimEnd('\'))
}
if ([string]::IsNullOrWhiteSpace($DriveLetter)) {
    foreach ($candidate in @("W:", "X:", "Y:", "Z:")) {
        if (-not $usedDrives.Contains($candidate) -and -not [System.IO.Directory]::Exists("$candidate\")) {
            $DriveLetter = $candidate
            break
        }
    }
    if ([string]::IsNullOrWhiteSpace($DriveLetter)) {
        throw "No free drive letter found among W:, X:, Y:, Z:. Specify -DriveLetter explicitly."
    }
} else {
    $DriveLetter = $DriveLetter.Trim().TrimEnd('\')
    if ($DriveLetter.Length -eq 1) { $DriveLetter = "$DriveLetter`:" }
    if ($DriveLetter -notmatch '^[A-Za-z]:$') {
        throw "Invalid -DriveLetter '$DriveLetter'. Expected format like 'W:'."
    }
    if ($usedDrives.Contains($DriveLetter) -or [System.IO.Directory]::Exists("$DriveLetter\")) {
        throw "Drive letter $DriveLetter is already in use."
    }
}
Write-Host "Selected mount drive: $DriveLetter"

function ConvertTo-WslPath {
    param([Parameter(Mandatory = $true)][string]$WindowsPath)
    $full = [System.IO.Path]::GetFullPath($WindowsPath)
    if ($full -match '^([A-Za-z]):[\\/](.*)$') {
        $drive = $matches[1].ToLowerInvariant()
        $rest = $matches[2].Replace('\', '/')
        return "/mnt/$drive/$rest"
    }
    throw "Cannot convert path to WSL format: $WindowsPath"
}

function Start-ZeroExt4Mount {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath,
        [Parameter(Mandatory = $true)][string]$ImagePath,
        [Parameter(Mandatory = $true)][string]$MountPoint,
        [Parameter(Mandatory = $true)][string]$StopEventName,
        [string[]]$ExtraArgs = @()
    )

    $argList = @(
        "mount",
        "--source", "`"$ImagePath`"",
        "--mountpoint", $MountPoint,
        "--stop-event", $StopEventName,
        "--json-events"
    ) + $ExtraArgs

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $ExePath
    $psi.Arguments = ($argList -join " ")
    $psi.WorkingDirectory = $RepoRoot
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    if (-not $proc.Start()) {
        throw "Failed to start zeroext4.exe process."
    }

    $errTask = $proc.StandardError.ReadToEndAsync()
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    $mounted = $false
    $stdoutLines = New-Object System.Collections.Generic.List[string]

    while ([DateTime]::UtcNow -lt $deadline) {
        $lineTask = $proc.StandardOutput.ReadLineAsync()
        while (-not $lineTask.Wait(100)) {
            if ($proc.HasExited -or ([DateTime]::UtcNow -ge $deadline)) { break }
        }
        if ($lineTask.IsCompleted -and ($null -ne $lineTask.Result)) {
            $line = $lineTask.Result
            [void]$stdoutLines.Add($line)
            if ($line -match '"event"\s*:\s*"mounted"') {
                $mounted = $true
                break
            }
        } elseif ($proc.HasExited) {
            break
        }
    }

    if (-not $mounted) {
        $errText = if ($errTask.Wait(1000)) { $errTask.Result } else { "" }
        try { if (-not $proc.HasExited) { $proc.Kill() } } catch {}
        throw "Failed waiting for mount ready on $MountPoint. Stdout=[$($stdoutLines -join ' | ')] Stderr=[$errText]"
    }

    $nsDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $nsDeadline) {
        if ([System.IO.Directory]::Exists("$MountPoint\")) {
            return @{ Process = $proc; ErrTask = $errTask }
        }
        Start-Sleep -Milliseconds 50
    }
    throw "Mount event ready, but $MountPoint\ did not become visible."
}

function Stop-ZeroExt4Mount {
    param(
        [Parameter(Mandatory = $true)]$MountHandle,
        [Parameter(Mandatory = $true)][string]$StopEventName,
        [Parameter(Mandatory = $true)][string]$MountPoint
    )

    $proc = $MountHandle.Process
    if ($null -eq $proc -or $proc.HasExited) { return }

    try {
        $ev = [System.Threading.EventWaitHandle]::OpenExisting($StopEventName)
        try { [void]$ev.Set() } finally { $ev.Dispose() }
    } catch {
        Write-Warning "Could not signal stop event ${StopEventName}: $_"
    }

    if (-not $proc.WaitForExit(15000)) {
        & $ZeroExt4Exe unmount $MountPoint | Out-Null
        if (-not $proc.WaitForExit(5000)) {
            $proc.Kill()
            $proc.WaitForExit(5000) | Out-Null
        }
        throw "zeroext4.exe mount on $MountPoint failed to exit cleanly."
    }

    if ($proc.ExitCode -ne 0) {
        $errText = if ($MountHandle.ErrTask.Wait(1000)) { $MountHandle.ErrTask.Result } else { "" }
        throw "zeroext4.exe mount exited with non-zero code $($proc.ExitCode). Stderr=[$errText]"
    }
}

$modeMap = @{
    "rw-default" = @("--rw")
    "rw-fast-io" = @("--rw", "--rw-fast-io")
}

$overallFailed = $false

foreach ($modeName in $Modes) {
    if (-not $modeMap.ContainsKey($modeName)) {
        throw "Unknown mode '$modeName'. Supported: rw-default, rw-fast-io"
    }
    $extraArgs = $modeMap[$modeName]

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "Running WinFsp Official Suite (winfsp-tests-x64.exe): $modeName"
    Write-Host "============================================================"

    $imgPath = Join-Path $env:TEMP ("zeroext4_winfsptests_{0}_{1}.img" -f $modeName, $PID)
    if (Test-Path $imgPath) { Remove-Item -Force $imgPath }
    $mountHandle = $null
    $evName = "Local\ZeroExt4_WinFspTests_${modeName}_$PID"

    try {
        Write-Host "[1/4] Creating ${SizeMb}MB ext4 image: $imgPath"
        & $ZeroExt4Exe mkfs $imgPath --size-mb $SizeMb --label WINFSP
        if ($LASTEXITCODE -ne 0) { throw "mkfs failed with exit code $LASTEXITCODE" }

        Write-Host "[2/4] Mounting $DriveLetter ($($extraArgs -join ' '))..."
        $mountHandle = Start-ZeroExt4Mount -ExePath $ZeroExt4Exe -ImagePath $imgPath -MountPoint $DriveLetter -StopEventName $evName -ExtraArgs $extraArgs

        # Default ext4-applicable test filter if -TestArgs not passed
        # Excludes 7 Windows/NTFS-only or UTF-8/UTF-16 spec-difference items:
        #   - reparse* (ReparsePoints=0), dirnotify_test, lock*
        #   - create_fileattr_test, setfileinfo_test (SYSTEM/HIDDEN attr persistence)
        #   - create_namelen_test, querydir_namelen_test (254 Hiragana UTF-16 = 762B > ext4 255B UTF-8)
        $effectiveTestArgs = $TestArgs
        if ($effectiveTestArgs.Count -eq 0) {
            $effectiveTestArgs = @(
                "-reparse*",
                "-dirnotify_test",
                "-lock*",
                "-create_fileattr_test",
                "-setfileinfo_test",
                "-create_namelen_test",
                "-querydir_namelen_test"
            )
        }

        $suiteArgs = @(
            "--fuse-external",
            "--resilient",
            "--case-insensitive",
            "--case-insensitive-cmp",
            "--no-abort"
        ) + $effectiveTestArgs

        Write-Host "[3/4] Executing winfsp-tests-x64.exe $($suiteArgs -join ' ') in $DriveLetter\ ..."
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $WinFspTestsExe
        $psi.Arguments = ($suiteArgs -join " ")
        $psi.WorkingDirectory = "$DriveLetter\"
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true

        $testProc = New-Object System.Diagnostics.Process
        $testProc.StartInfo = $psi
        if (-not $testProc.Start()) { throw "Failed to start winfsp-tests-x64.exe" }

        $stderrTask = $testProc.StandardError.ReadToEndAsync()
        $outLines = New-Object System.Collections.Generic.List[string]
        while ($null -ne ($line = $testProc.StandardOutput.ReadLine())) {
            [void]$outLines.Add($line)
            Write-Host "  $line"
        }
        if (-not $testProc.WaitForExit(180000)) {
            try { $testProc.Kill() } catch {}
            throw "winfsp-tests-x64.exe timed out after 180s."
        }
        $stderrText = if ($stderrTask.Wait(2000)) { $stderrTask.Result } else { "" }
        if (-not [string]::IsNullOrWhiteSpace($stderrText)) {
            $stderrText -split "`r?`n" | ForEach-Object { if ($_ -ne "") { Write-Host "  [stderr] $_" } }
        }

        $passCount = 0
        $failCount = 0
        $failedTests = New-Object System.Collections.Generic.List[string]
        foreach ($l in $outLines) {
            if ($l -match '^\S+\.*\s+OK\s+[0-9.]+s$') {
                $passCount++
            } elseif ($l -match '^(\S+?)\.*\s+KO\b') {
                $failCount++
                [void]$failedTests.Add($matches[1])
            }
        }
        Write-Host "  Summary [$modeName]: PASS=$passCount, FAIL=$failCount, ExitCode=$($testProc.ExitCode)"
        if ($failedTests.Count -gt 0) {
            Write-Host "  Failed tests [$modeName]: $($failedTests -join ', ')"
            $overallFailed = $true
        }

        Write-Host "[4/4] Unmounting cleanly..."
        Stop-ZeroExt4Mount -MountHandle $mountHandle -StopEventName $evName -MountPoint $DriveLetter
        $mountHandle = $null

        if (-not $SkipE2fsck) {
            $wslImg = ConvertTo-WslPath -WindowsPath $imgPath
            $prevEap = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            try {
                $e2Out = & wsl.exe -- e2fsck -fn $wslImg 2>&1 | ForEach-Object { "$_" }
                $e2Exit = $LASTEXITCODE
            } finally {
                $ErrorActionPreference = $prevEap
            }
            $e2Out | ForEach-Object { Write-Host "  [e2fsck] $_" }
            if ($e2Exit -ne 0) {
                throw "Post-winfsp-tests e2fsck -fn FAILED for $modeName (exit=$e2Exit)!"
            }
            Write-Host "  [e2fsck] Post-test filesystem check PASSED (exit code 0)."
        }
    } finally {
        if ($null -ne $mountHandle -and $null -ne $mountHandle.Process -and -not $mountHandle.Process.HasExited) {
            try {
                $ev = [System.Threading.EventWaitHandle]::OpenExisting($evName)
                try { [void]$ev.Set() } finally { $ev.Dispose() }
            } catch {}
            if (-not $mountHandle.Process.WaitForExit(3000)) {
                & $ZeroExt4Exe unmount $DriveLetter 2>&1 | Out-Null
                if (-not $mountHandle.Process.WaitForExit(3000)) {
                    $mountHandle.Process.Kill()
                }
            }
        }
        if (Test-Path $imgPath) {
            Remove-Item -Force $imgPath -ErrorAction SilentlyContinue
        }
    }
}

if ($overallFailed) {
    exit 1
}
Write-Host ""
Write-Host "ALL SELECTED WINFSP OFFICIAL TESTS PASSED."
