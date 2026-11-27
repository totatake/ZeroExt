[CmdletBinding()]
param(
    [string]$DriveLetter = "",
    [int]$SizeMb = 64,
    [switch]$KeepImage
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ZeroExt4Exe = Join-Path $RepoRoot "build\zeroext4.exe"
if (-not (Test-Path $ZeroExt4Exe)) {
    throw "zeroext4.exe not found at $ZeroExt4Exe. Build first with scripts\build_and_test.ps1."
}

# Verify WSL2 and e2fsck availability
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
Write-Host "Using WSL2 e2fsck: $e2fsckCheck"

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
    if ($DriveLetter.Length -eq 1) {
        $DriveLetter = "$DriveLetter`:"
    }
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

function New-DeterministicBytes {
    param(
        [Parameter(Mandatory = $true)][int]$Length,
        [Parameter(Mandatory = $true)][int]$Seed
    )
    $buf = New-Object byte[] $Length
    $rng = New-Object System.Random $Seed
    $rng.NextBytes($buf)
    return $buf
}

function Get-FileSha256Hex {
    param([Parameter(Mandatory = $true)][string]$Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            $hash = $sha.ComputeHash($fs)
            return ([System.BitConverter]::ToString($hash)).Replace("-", "").ToLowerInvariant()
        } finally {
            $fs.Dispose()
        }
    } finally {
        $sha.Dispose()
    }
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
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $proc = New-Object System.Diagnostics.Process
    $psi.WorkingDirectory = $RepoRoot
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
            if ($proc.HasExited -or ([DateTime]::UtcNow -ge $deadline)) {
                break
            }
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
        $errText = ""
        if ($errTask.Wait(1000)) { $errText = $errTask.Result }
        $exitCodeInfo = if ($proc.HasExited) { $proc.ExitCode } else { "still running" }
        try {
            if (-not $proc.HasExited) { $proc.Kill() }
        } catch {}
        throw "Timed out or failed waiting for mount ready on $MountPoint (exit=$exitCodeInfo). Stdout=[$($stdoutLines -join ' | ')] Stderr=[$errText]"
    }

    # Wait briefly for Windows namespace readiness on the mounted drive
    $nsDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $nsDeadline) {
        if ([System.IO.Directory]::Exists("$MountPoint\")) {
            return @{
                Process = $proc
                ErrTask = $errTask
            }
        }
        Start-Sleep -Milliseconds 50
    }
    throw "Mount event reported ready, but $MountPoint\ did not become visible."
}

function Stop-ZeroExt4Mount {
    param(
        [Parameter(Mandatory = $true)]$MountHandle,
        [Parameter(Mandatory = $true)][string]$StopEventName,
        [Parameter(Mandatory = $true)][string]$MountPoint
    )

    $proc = $MountHandle.Process
    if ($null -eq $proc -or $proc.HasExited) {
        return
    }

    try {
        $ev = [System.Threading.EventWaitHandle]::OpenExisting($StopEventName)
        try {
            [void]$ev.Set()
        } finally {
            $ev.Dispose()
        }
    } catch {
        Write-Warning "Could not signal stop event ${StopEventName}: $_"
    }

    if (-not $proc.WaitForExit(15000)) {
        Write-Warning "Mount process did not exit cleanly within 15s; invoking unmount fallback..."
        & $ZeroExt4Exe unmount $MountPoint | Out-Null
        if (-not $proc.WaitForExit(5000)) {
            $proc.Kill()
            $proc.WaitForExit(5000) | Out-Null
        }
        throw "zeroext4.exe mount on $MountPoint failed to shut down cleanly via stop event."
    }

    if ($proc.ExitCode -ne 0) {
        $errText = if ($MountHandle.ErrTask.Wait(1000)) { $MountHandle.ErrTask.Result } else { "" }
        throw "zeroext4.exe mount exited with non-zero code $($proc.ExitCode). Stderr=[$errText]"
    }
}

function Invoke-Workload {
    param([Parameter(Mandatory = $true)][string]$RootPath)

    Write-Host "  Running I/O workload on $RootPath ..."

    # 1. Large sequential 8 MB file + unaligned partial overwrites + EOF append
    $seqPath = Join-Path $RootPath "seq_8mb.bin"
    $initialBytes = New-DeterministicBytes -Length (8 * 1024 * 1024) -Seed 1001
    $fs = [System.IO.File]::Open($seqPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::ReadWrite)
    try {
        $fs.Write($initialBytes, 0, $initialBytes.Length)
        $patches = @(
            @{ Offset = 123;     Len = 777;  Seed = 2001 },
            @{ Offset = 4093;    Len = 4100; Seed = 2002 },
            @{ Offset = 65531;   Len = 13;   Seed = 2003 },
            @{ Offset = 1048573; Len = 5000; Seed = 2004 },
            @{ Offset = 8388590; Len = 200;  Seed = 2005 }
        )
        foreach ($p in $patches) {
            [void]$fs.Seek([long]$p.Offset, [System.IO.SeekOrigin]::Begin)
            $patchBuf = New-DeterministicBytes -Length $p.Len -Seed ([uint32]$p.Seed)
            $fs.Write($patchBuf, 0, $patchBuf.Length)
        }
        $fs.Flush()
    } finally {
        $fs.Dispose()
    }

    $fsAppend = [System.IO.File]::Open($seqPath, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write)
    try {
        $tailBuf = New-DeterministicBytes -Length 12345 -Seed 2099
        $fsAppend.Write($tailBuf, 0, $tailBuf.Length)
        $fsAppend.Flush()
    } finally {
        $fsAppend.Dispose()
    }

    # 2. Sparse hole file + hole fill + truncate down + extend + overwrite
    $sparsePath = Join-Path $RootPath "sparse.bin"
    $fsSparse = [System.IO.File]::Open($sparsePath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::ReadWrite)
    try {
        $head = New-DeterministicBytes -Length 5120 -Seed 3001
        $fsSparse.Write($head, 0, $head.Length)

        [void]$fsSparse.Seek(262144, [System.IO.SeekOrigin]::Begin)
        $far = New-DeterministicBytes -Length 7168 -Seed 3002
        $fsSparse.Write($far, 0, $far.Length)

        [void]$fsSparse.Seek(131072, [System.IO.SeekOrigin]::Begin)
        $mid = New-DeterministicBytes -Length 12288 -Seed 3003
        $fsSparse.Write($mid, 0, $mid.Length)

        $fsSparse.SetLength(145000)
        $fsSparse.SetLength(200000)

        [void]$fsSparse.Seek(1024, [System.IO.SeekOrigin]::Begin)
        $ov = New-DeterministicBytes -Length 4976 -Seed 3004
        $fsSparse.Write($ov, 0, $ov.Length)
        $fsSparse.Flush()
    } finally {
        $fsSparse.Dispose()
    }

    # 3. Fragmented multi-file burst: 64 files in 4 nested dirs, overwrite odd, rename 16, delete 32, recreate 16
    $burstRoot = Join-Path $RootPath "burst"
    [void][System.IO.Directory]::CreateDirectory($burstRoot)
    for ($d = 0; $d -lt 4; $d++) {
        [void][System.IO.Directory]::CreateDirectory((Join-Path $burstRoot "dir_$d"))
    }

    for ($i = 0; $i -lt 64; $i++) {
        $d = $i % 4
        $fileName = ("file_{0:D2}.bin" -f $i)
        $path = Join-Path (Join-Path $burstRoot "dir_$d") $fileName
        $len = 500 + (($i * 389) % 24500)
        $data = New-DeterministicBytes -Length $len -Seed ([uint32](4000 + $i))
        [System.IO.File]::WriteAllBytes($path, $data)
    }

    # Overwrite odd files with varied offsets/lengths
    for ($i = 1; $i -lt 64; $i += 2) {
        $d = $i % 4
        $fileName = ("file_{0:D2}.bin" -f $i)
        $path = Join-Path (Join-Path $burstRoot "dir_$d") $fileName
        $newLen = 1024 + (($i * 521) % 16384)
        $data = New-DeterministicBytes -Length $newLen -Seed ([uint32](5000 + $i))
        [System.IO.File]::WriteAllBytes($path, $data)
    }

    # Rename first 16 files
    for ($i = 0; $i -lt 16; $i++) {
        $d = $i % 4
        $dirPath = Join-Path $burstRoot "dir_$d"
        $oldPath = Join-Path $dirPath ("file_{0:D2}.bin" -f $i)
        $newPath = Join-Path $dirPath ("renamed_{0:D2}.bin" -f $i)
        [System.IO.File]::Move($oldPath, $newPath)
    }

    # Delete middle 32 files (indices 16..47)
    for ($i = 16; $i -lt 48; $i++) {
        $d = $i % 4
        $dirPath = Join-Path $burstRoot "dir_$d"
        $delPath = Join-Path $dirPath ("file_{0:D2}.bin" -f $i)
        [System.IO.File]::Delete($delPath)
    }

    # Recreate 16 fresh files to exercise freed blocks/inodes
    for ($i = 0; $i -lt 16; $i++) {
        $d = $i % 4
        $dirPath = Join-Path $burstRoot "dir_$d"
        $freshPath = Join-Path $dirPath ("fresh_{0:D2}.bin" -f $i)
        $len = 2048 + (($i * 733) % 20000)
        $data = New-DeterministicBytes -Length $len -Seed ([uint32](6000 + $i))
        [System.IO.File]::WriteAllBytes($freshPath, $data)
    }

    # Snapshot all surviving files (Length + SHA-256)
    $manifest = [ordered]@{}
    $allFiles = [System.IO.Directory]::GetFiles($RootPath, "*", [System.IO.SearchOption]::AllDirectories) | Sort-Object
    foreach ($fullPath in $allFiles) {
        $rel = $fullPath.Substring($RootPath.Length).TrimStart('\')
        if ($rel -match '^(System Volume Information|\$RECYCLE\.BIN)([\\/]|$)') {
            continue
        }
        $info = New-Object System.IO.FileInfo $fullPath
        $sha = Get-FileSha256Hex -Path $fullPath
        $manifest[$rel] = @{
            Length = $info.Length
            Sha256 = $sha
        }
    }

    # Expected: seq_8mb.bin + sparse.bin + 16 renamed + 16 remaining (48..63) + 16 fresh = 50 files
    if ($manifest.Count -ne 50) {
        throw "Expected 50 surviving workload files on $RootPath, got $($manifest.Count)."
    }
    Write-Host "  Recorded $($manifest.Count) surviving files with SHA-256 digests."
    return $manifest
}

function Test-ReadOnlyManifest {
    param(
        [Parameter(Mandatory = $true)][string]$RootPath,
        [Parameter(Mandatory = $true)]$ExpectedManifest
    )

    $actualFiles = [System.IO.Directory]::GetFiles($RootPath, "*", [System.IO.SearchOption]::AllDirectories) | Sort-Object
    $actualUserCount = 0
    foreach ($f in $actualFiles) {
        $relPath = $f.Substring($RootPath.Length).TrimStart('\')
        if ($relPath -notmatch '^(System Volume Information|\$RECYCLE\.BIN)([\\/]|$)') {
            $actualUserCount++
        }
    }
    if ($actualUserCount -ne $ExpectedManifest.Count) {
        throw "Unexpected file count on RO remount: expected $($ExpectedManifest.Count), got $actualUserCount"
    }

    $checked = 0
    foreach ($rel in $ExpectedManifest.Keys) {
        $fullPath = Join-Path $RootPath $rel
        if (-not (Test-Path -LiteralPath $fullPath)) {
            throw "Missing file on RO remount: $rel"
        }
        $info = New-Object System.IO.FileInfo $fullPath
        $expected = $ExpectedManifest[$rel]
        if ($info.Length -ne $expected.Length) {
            throw "Size mismatch on RO remount for ${rel}: expected $($expected.Length), got $($info.Length)"
        }
        $actualSha = Get-FileSha256Hex -Path $fullPath
        if ($actualSha -ne $expected.Sha256) {
            throw "SHA-256 mismatch on RO remount for ${rel}: expected $($expected.Sha256), got $actualSha"
        }
        $checked++
    }
    Write-Host "  Verified $checked/$($ExpectedManifest.Count) files on RO remount (100% length + SHA-256 match)."
}

$modes = @(
    @{ Name = "rw-default"; ExtraArgs = @("--rw") },
    @{ Name = "rw-fast-io"; ExtraArgs = @("--rw", "--rw-fast-io") }
)

foreach ($mode in $modes) {
    $modeName = $mode.Name
    Write-Host ""
    Write-Host "============================================================"
    Write-Host "Running WinFsp + WSL2 e2fsck -fn verification: $modeName"
    Write-Host "============================================================"

    $imgPath = Join-Path $env:TEMP ("zeroext4_e2fsck_{0}_{1}.img" -f $modeName, $PID)
    if (Test-Path $imgPath) { Remove-Item -Force $imgPath }

    $mountHandle = $null
    $currentEvName = $null

    try {
        Write-Host "[1/6] Creating ${SizeMb}MB ext4 image: $imgPath"
        & $ZeroExt4Exe mkfs $imgPath --size-mb $SizeMb --label E2FSCK
        if ($LASTEXITCODE -ne 0) {
            throw "mkfs failed with exit code $LASTEXITCODE"
        }

        $currentEvName = "Local\ZeroExt4_Verify_RW_${modeName}_$PID"
        Write-Host "[2/6] Mounting $DriveLetter ($($mode.ExtraArgs -join ' '))..."
        $mountHandle = Start-ZeroExt4Mount -ExePath $ZeroExt4Exe -ImagePath $imgPath -MountPoint $DriveLetter -StopEventName $currentEvName -ExtraArgs $mode.ExtraArgs

        Write-Host "[3/6] Executing Win32 file/directory workload..."
        $manifest = Invoke-Workload -RootPath "$DriveLetter\"

        Write-Host "[4/6] Unmounting cleanly via stop event..."
        Stop-ZeroExt4Mount -MountHandle $mountHandle -StopEventName $currentEvName -MountPoint $DriveLetter
        $mountHandle = $null

        $wslImgPath = ConvertTo-WslPath -WindowsPath $imgPath
        Write-Host "[5/6] Running upstream e2fsck -fn $wslImgPath ..."
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $e2fsckOut = & wsl.exe -- e2fsck -fn $wslImgPath 2>&1 | ForEach-Object { "$_" }
            $e2fsckExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $prevEap
        }
        $e2fsckOut | ForEach-Object { Write-Host "  [e2fsck] $_" }
        if ($e2fsckExit -ne 0) {
            throw "e2fsck -fn FAILED for $modeName with exit code $e2fsckExit!"
        }
        Write-Host "  [e2fsck] PASSED (exit code 0)."

        $currentEvName = "Local\ZeroExt4_Verify_RO_${modeName}_$PID"
        Write-Host "[6/6] Remounting Read-Only (Direct Extent I/O) and verifying SHA-256 digests..."
        $mountHandle = Start-ZeroExt4Mount -ExePath $ZeroExt4Exe -ImagePath $imgPath -MountPoint $DriveLetter -StopEventName $currentEvName
        Test-ReadOnlyManifest -RootPath "$DriveLetter\" -ExpectedManifest $manifest
        Stop-ZeroExt4Mount -MountHandle $mountHandle -StopEventName $currentEvName -MountPoint $DriveLetter
        $mountHandle = $null

        Write-Host "RESULT [$modeName]: PASS"
    } finally {
        if ($null -ne $mountHandle -and $null -ne $mountHandle.Process -and -not $mountHandle.Process.HasExited) {
            try {
                if ($null -ne $currentEvName) {
                    $ev = [System.Threading.EventWaitHandle]::OpenExisting($currentEvName)
                    try { [void]$ev.Set() } finally { $ev.Dispose() }
                }
            } catch {}
            if (-not $mountHandle.Process.WaitForExit(3000)) {
                & $ZeroExt4Exe unmount $DriveLetter 2>&1 | Out-Null
                if (-not $mountHandle.Process.WaitForExit(3000)) {
                    $mountHandle.Process.Kill()
                }
            }
        }
        if (-not $KeepImage -and (Test-Path $imgPath)) {
            Remove-Item -Force $imgPath -ErrorAction SilentlyContinue
        }
    }
}

Write-Host ""
Write-Host "ALL MODES PASSED (rw-default + rw-fast-io): e2fsck -fn clean & RO SHA-256 verified."
