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

# Verify WSL2 and required ext4/loop tools availability
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $toolCheck = (& wsl.exe -u root -- which e2fsck mkfs.ext4 mount umount fallocate sha256sum 2>&1 | ForEach-Object { "$_" }) -join "`n"
    $whichExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $prevEap
}
if ($whichExit -ne 0) {
    throw "Required WSL2 Linux tools (e2fsck/mkfs.ext4/mount/umount/fallocate/sha256sum) are not available: $toolCheck"
}
Write-Host "Verified WSL2 tools: e2fsck, mkfs.ext4, mount, umount, fallocate, sha256sum"

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

    $manifest = Get-WindowsTreeManifest -RootPath $RootPath -ExpectedCount 50 -Label "Windows workload"
    return $manifest
}

function Invoke-WslRootBash {
    param([Parameter(Mandatory = $true)][string]$Script)

    $normalized = $Script.Replace("`r`n", "`n")
    $shPath = Join-Path $env:TEMP ("zeroext4_wsl_{0}_{1}.sh" -f $PID, ([Guid]::NewGuid().ToString("N").Substring(0, 8)))
    try {
        [System.IO.File]::WriteAllText($shPath, $normalized, (New-Object System.Text.UTF8Encoding $false))
        $wslShPath = ConvertTo-WslPath -WindowsPath $shPath
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $lines = @(& wsl.exe -u root -- bash -euo pipefail $wslShPath 2>&1 | ForEach-Object { "$_" })
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $prevEap
        }
        return @{
            ExitCode = $exitCode
            Lines    = $lines
        }
    } finally {
        if (Test-Path $shPath) {
            Remove-Item -Force $shPath -ErrorAction SilentlyContinue
        }
    }
}

function Parse-WslFileManifestLines {
    param([Parameter(Mandatory = $true)][string[]]$Lines)

    $manifest = [ordered]@{}
    foreach ($line in $Lines) {
        if ($line -match '^FILE\|([^|]+)\|([0-9]+)\|([0-9a-fA-F]{64})$') {
            $rel = $matches[1].TrimStart('./').Replace('\', '/')
            $manifest[$rel] = @{
                Length = [int64]$matches[2]
                Sha256 = $matches[3].ToLowerInvariant()
            }
        }
    }
    return $manifest
}

function Get-WindowsTreeManifest {
    param(
        [Parameter(Mandatory = $true)][string]$RootPath,
        [int]$ExpectedCount = -1,
        [string]$Label = "Windows tree"
    )

    $manifest = [ordered]@{}
    $allFiles = [System.IO.Directory]::GetFiles($RootPath, "*", [System.IO.SearchOption]::AllDirectories) | Sort-Object
    foreach ($fullPath in $allFiles) {
        $rel = $fullPath.Substring($RootPath.Length).TrimStart('\')
        if ($rel -match '^(System Volume Information|\$RECYCLE\.BIN|lost\+found)([\\/]|$)') {
            continue
        }
        $relKey = $rel.Replace('\', '/')
        $info = New-Object System.IO.FileInfo $fullPath
        $sha = Get-FileSha256Hex -Path $fullPath
        $manifest[$relKey] = @{
            Length = $info.Length
            Sha256 = $sha
        }
    }
    if ($ExpectedCount -ge 0 -and $manifest.Count -ne $ExpectedCount) {
        throw "Expected $ExpectedCount surviving files in $Label on $RootPath, got $($manifest.Count)."
    }
    Write-Host "  Recorded $($manifest.Count) surviving files ($Label) with SHA-256 digests."
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
        if ($relPath -notmatch '^(System Volume Information|\$RECYCLE\.BIN|lost\+found)([\\/]|$)') {
            $actualUserCount++
        }
    }
    if ($actualUserCount -ne $ExpectedManifest.Count) {
        throw "Unexpected file count on RO remount: expected $($ExpectedManifest.Count), got $actualUserCount"
    }

    $checked = 0
    foreach ($rel in $ExpectedManifest.Keys) {
        $winRel = $rel.Replace('/', '\')
        $fullPath = Join-Path $RootPath $winRel
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

function Test-WslKernelLoopManifest {
    param(
        [Parameter(Mandatory = $true)][string]$WindowsImgPath,
        [Parameter(Mandatory = $true)]$ExpectedManifest,
        [Parameter(Mandatory = $true)][string]$Tag
    )

    $wslSrc = ConvertTo-WslPath -WindowsPath $WindowsImgPath
    $bashScript = @'
src="__WSL_SRC__"
tmpimg="/tmp/zeroext4_loop___TAG___$$.img"
mnt="/tmp/zeroext4_mnt___TAG___$$"
cleanup() {
    umount "$mnt" 2>/dev/null || true
    rm -rf "$mnt" "$tmpimg"
}
trap cleanup EXIT
cp "$src" "$tmpimg"
mkdir -p "$mnt"
mount -t ext4 -o loop,ro "$tmpimg" "$mnt"
(
    cd "$mnt"
    while IFS= read -r f; do
        rel="${f#./}"
        sz=$(stat -c '%s' "$f")
        sha=$(sha256sum "$f" | awk '{print $1}')
        echo "FILE|$rel|$sz|$sha"
    done < <(find . -type f ! -path './lost+found/*' | sort)
)
umount "$mnt"
'@
    $bashScript = $bashScript.Replace("__WSL_SRC__", $wslSrc).Replace("__TAG__", $Tag)
    $res = Invoke-WslRootBash -Script $bashScript
    if ($res.ExitCode -ne 0) {
        $res.Lines | ForEach-Object { Write-Host "  [wsl-loop] $_" }
        throw "WSL2 Linux kernel mount -o loop,ro failed for ${Tag} (exit $($res.ExitCode))."
    }

    $linuxManifest = Parse-WslFileManifestLines -Lines $res.Lines
    if ($linuxManifest.Count -ne $ExpectedManifest.Count) {
        throw "Linux kernel loop mount file count mismatch for ${Tag}: expected $($ExpectedManifest.Count), got $($linuxManifest.Count)."
    }

    $checked = 0
    foreach ($rel in $ExpectedManifest.Keys) {
        if (-not $linuxManifest.Contains($rel)) {
            throw "Missing file in Linux kernel loop mount for ${Tag}: $rel"
        }
        $exp = $ExpectedManifest[$rel]
        $act = $linuxManifest[$rel]
        if ($act.Length -ne $exp.Length) {
            throw "Size mismatch in Linux kernel loop mount (${Tag}) for ${rel}: expected $($exp.Length), got $($act.Length)"
        }
        if ($act.Sha256 -ne $exp.Sha256) {
            throw "SHA-256 mismatch in Linux kernel loop mount (${Tag}) for ${rel}: expected $($exp.Sha256), got $($act.Sha256)"
        }
        $checked++
    }
    Write-Host "  Verified $checked/$($ExpectedManifest.Count) files via Linux kernel mount -o loop,ro (100% length + SHA-256 match)."
}

function New-WslKernelExt4Image {
    param(
        [Parameter(Mandatory = $true)][string]$WindowsDestPath,
        [Parameter(Mandatory = $true)][int]$SizeMb,
        [Parameter(Mandatory = $true)][string]$Tag
    )

    $wslDest = ConvertTo-WslPath -WindowsPath $WindowsDestPath
    $bashScript = @'
dest="__WSL_DEST__"
tmpimg="/tmp/zeroext4_src___TAG___$$.img"
mnt="/tmp/zeroext4_srcmnt___TAG___$$"
cleanup() {
    umount "$mnt" 2>/dev/null || true
    rm -rf "$mnt" "$tmpimg"
}
trap cleanup EXIT

truncate -s "__SIZE_MB__M" "$tmpimg"
mkfs.ext4 -q -F -L LINUXEXT4 "$tmpimg"
mkdir -p "$mnt"
mount -t ext4 -o loop "$tmpimg" "$mnt"

# 1. 4 MB sequential random file
dd if=/dev/urandom of="$mnt/linux_seq.bin" bs=1M count=4 status=none

# 2. Sparse file with holes
dd if=/dev/urandom of="$mnt/linux_sparse.bin" bs=4096 count=2 status=none
dd if=/dev/urandom of="$mnt/linux_sparse.bin" bs=4096 count=2 seek=64 conv=notrunc status=none
dd if=/dev/urandom of="$mnt/linux_sparse.bin" bs=4096 count=3 seek=128 conv=notrunc status=none

# 3. Genuine Linux UNINIT extents via fallocate + partial overwrites
fallocate -l 262144 "$mnt/linux_uninit.bin"
dd if=/dev/urandom of="$mnt/linux_uninit.bin" bs=5000 count=1 conv=notrunc status=none
dd if=/dev/urandom of="$mnt/linux_uninit.bin" bs=1 count=5000 seek=131072 conv=notrunc status=none

# 4. Fragmented file interleaved with sibling temp allocations
for i in $(seq 0 15); do
    dd if=/dev/urandom bs=8192 count=1 status=none >> "$mnt/linux_frag.bin"
    dd if=/dev/urandom of="$mnt/frag_pad_$i.tmp" bs=16384 count=1 status=none
done
rm -f "$mnt"/frag_pad_*.tmp

# 5. Nested directory tree (16 files across sub_0..3)
for i in $(seq 0 15); do
    sub=$((i % 4))
    mkdir -p "$mnt/linux_dir/sub_$sub"
    flen=$((137 + i * 503))
    fname=$(printf "f_%02d.bin" "$i")
    head -c "$flen" /dev/urandom > "$mnt/linux_dir/sub_$sub/$fname"
done

sync
(
    cd "$mnt"
    while IFS= read -r f; do
        rel="${f#./}"
        sz=$(stat -c '%s' "$f")
        sha=$(sha256sum "$f" | awk '{print $1}')
        echo "FILE|$rel|$sz|$sha"
    done < <(find . -type f ! -path './lost+found/*' | sort)
)
umount "$mnt"
e2fsck -fn "$tmpimg" >/dev/null
cp "$tmpimg" "$dest"
'@
    $bashScript = $bashScript.Replace("__WSL_DEST__", $wslDest).Replace("__SIZE_MB__", "$SizeMb").Replace("__TAG__", $Tag)
    $res = Invoke-WslRootBash -Script $bashScript
    if ($res.ExitCode -ne 0) {
        $res.Lines | ForEach-Object { Write-Host "  [wsl-mkfs] $_" }
        throw "WSL2 Linux kernel mkfs.ext4 + populate failed for ${Tag} (exit $($res.ExitCode))."
    }

    $manifest = Parse-WslFileManifestLines -Lines $res.Lines
    if ($manifest.Count -ne 20) {
        throw "Expected 20 files in WSL2 Linux kernel image for ${Tag}, got $($manifest.Count)."
    }
    Write-Host "  Created 64MB Linux kernel ext4 image with $($manifest.Count) files (incl. fallocate UNINIT, holes, fragmented extents)."
    return $manifest
}

function Invoke-LinuxImageModifications {
    param([Parameter(Mandatory = $true)][string]$RootPath)

    Write-Host "  Applying Windows Win32 RW modifications to Linux-created ext4 files..."

    # Overwrite linux_uninit.bin across UNINIT boundary (offset 4096, len 16384)
    $uninitPath = Join-Path $RootPath "linux_uninit.bin"
    $fsU = [System.IO.File]::Open($uninitPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite)
    try {
        [void]$fsU.Seek(4096, [System.IO.SeekOrigin]::Begin)
        $bufU = New-DeterministicBytes -Length 16384 -Seed 7001
        $fsU.Write($bufU, 0, $bufU.Length)
        $fsU.Flush()
    } finally {
        $fsU.Dispose()
    }

    # Overwrite inside linux_sparse.bin hole (offset 65536, len 12288) + append 4096 bytes
    $sparsePath = Join-Path $RootPath "linux_sparse.bin"
    $fsS = [System.IO.File]::Open($sparsePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite)
    try {
        [void]$fsS.Seek(65536, [System.IO.SeekOrigin]::Begin)
        $bufS = New-DeterministicBytes -Length 12288 -Seed 7002
        $fsS.Write($bufS, 0, $bufS.Length)
        $fsS.Flush()
    } finally {
        $fsS.Dispose()
    }
    $fsSApp = [System.IO.File]::Open($sparsePath, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write)
    try {
        $bufApp = New-DeterministicBytes -Length 4096 -Seed 7003
        $fsSApp.Write($bufApp, 0, $bufApp.Length)
        $fsSApp.Flush()
    } finally {
        $fsSApp.Dispose()
    }

    # Overwrite linux_seq.bin at unaligned offset 12345 (len 7777)
    $seqPath = Join-Path $RootPath "linux_seq.bin"
    $fsQ = [System.IO.File]::Open($seqPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite)
    try {
        [void]$fsQ.Seek(12345, [System.IO.SeekOrigin]::Begin)
        $bufQ = New-DeterministicBytes -Length 7777 -Seed 7004
        $fsQ.Write($bufQ, 0, $bufQ.Length)
        $fsQ.Flush()
    } finally {
        $fsQ.Dispose()
    }

    # Delete f_00..03.bin and rename f_04..07.bin -> win_renamed_04..07.bin
    $linuxDir = Join-Path $RootPath "linux_dir"
    for ($i = 0; $i -lt 4; $i++) {
        $sub = "sub_$($i % 4)"
        $delTarget = Join-Path (Join-Path $linuxDir $sub) ("f_{0:D2}.bin" -f $i)
        [System.IO.File]::Delete($delTarget)
    }
    for ($i = 4; $i -lt 8; $i++) {
        $subDir = Join-Path $linuxDir ("sub_{0}" -f ($i % 4))
        $oldP = Join-Path $subDir ("f_{0:D2}.bin" -f $i)
        $newP = Join-Path $subDir ("win_renamed_{0:D2}.bin" -f $i)
        [System.IO.File]::Move($oldP, $newP)
    }

    # Add 512 KB new file from Windows
    $addedPath = Join-Path $RootPath "win_added.bin"
    $addedData = New-DeterministicBytes -Length (512 * 1024) -Seed 7005
    [System.IO.File]::WriteAllBytes($addedPath, $addedData)

    # Expected: 20 - 4 deleted + 1 added = 17 surviving files
    return Get-WindowsTreeManifest -RootPath $RootPath -ExpectedCount 17 -Label "Windows-modified Linux image"
}

$modes = @(
    @{ Name = "rw-default"; ExtraArgs = @("--rw") },
    @{ Name = "rw-fast-io"; ExtraArgs = @("--rw", "--rw-fast-io") }
)

foreach ($mode in $modes) {
    $modeName = $mode.Name
    Write-Host ""
    Write-Host "============================================================"
    Write-Host "Direction A (ZeroExt4 -> e2fsck + Linux loop + RO): $modeName"
    Write-Host "============================================================"

    $imgPath = Join-Path $env:TEMP ("zeroext4_e2fsck_{0}_{1}.img" -f $modeName, $PID)
    if (Test-Path $imgPath) { Remove-Item -Force $imgPath }

    $mountHandle = $null
    $currentEvName = $null

    try {
        Write-Host "[1/7] Creating ${SizeMb}MB ext4 image: $imgPath"
        & $ZeroExt4Exe mkfs $imgPath --size-mb $SizeMb --label E2FSCK
        if ($LASTEXITCODE -ne 0) {
            throw "mkfs failed with exit code $LASTEXITCODE"
        }

        $currentEvName = "Local\ZeroExt4_Verify_RW_${modeName}_$PID"
        Write-Host "[2/7] Mounting $DriveLetter ($($mode.ExtraArgs -join ' '))..."
        $mountHandle = Start-ZeroExt4Mount -ExePath $ZeroExt4Exe -ImagePath $imgPath -MountPoint $DriveLetter -StopEventName $currentEvName -ExtraArgs $mode.ExtraArgs

        Write-Host "[3/7] Executing Win32 file/directory workload..."
        $manifest = Invoke-Workload -RootPath "$DriveLetter\"

        Write-Host "[4/7] Unmounting cleanly via stop event..."
        Stop-ZeroExt4Mount -MountHandle $mountHandle -StopEventName $currentEvName -MountPoint $DriveLetter
        $mountHandle = $null

        $wslImgPath = ConvertTo-WslPath -WindowsPath $imgPath
        Write-Host "[5/7] Running upstream e2fsck -fn $wslImgPath ..."
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

        Write-Host "[6/7] Cross-verifying via real WSL2 Linux kernel mount -t ext4 -o loop,ro..."
        Test-WslKernelLoopManifest -WindowsImgPath $imgPath -ExpectedManifest $manifest -Tag "dirA_${modeName}"

        $currentEvName = "Local\ZeroExt4_Verify_RO_${modeName}_$PID"
        Write-Host "[7/7] Remounting Read-Only (Direct Extent I/O) and verifying SHA-256 digests..."
        $mountHandle = Start-ZeroExt4Mount -ExePath $ZeroExt4Exe -ImagePath $imgPath -MountPoint $DriveLetter -StopEventName $currentEvName
        Test-ReadOnlyManifest -RootPath "$DriveLetter\" -ExpectedManifest $manifest
        Stop-ZeroExt4Mount -MountHandle $mountHandle -StopEventName $currentEvName -MountPoint $DriveLetter
        $mountHandle = $null

        Write-Host "RESULT [Direction A / $modeName]: PASS"
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

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "Direction B (Linux mkfs.ext4/loop -> ZeroExt4 RO & RW -> Linux loop): $modeName"
    Write-Host "============================================================"

    $linuxImgPath = Join-Path $env:TEMP ("zeroext4_linux_src_{0}_{1}.img" -f $modeName, $PID)
    if (Test-Path $linuxImgPath) { Remove-Item -Force $linuxImgPath }
    $mountHandle = $null
    $currentEvName = $null

    try {
        Write-Host "[1/6] Creating & populating ${SizeMb}MB ext4 image in WSL2 Linux kernel (mount -o loop)..."
        $linuxManifest = New-WslKernelExt4Image -WindowsDestPath $linuxImgPath -SizeMb $SizeMb -Tag "dirB_${modeName}"

        $currentEvName = "Local\ZeroExt4_Verify_DirB_RO_${modeName}_$PID"
        Write-Host "[2/6] Mounting Linux-created image Read-Only (Direct Extent I/O) and verifying 20/20 files..."
        $mountHandle = Start-ZeroExt4Mount -ExePath $ZeroExt4Exe -ImagePath $linuxImgPath -MountPoint $DriveLetter -StopEventName $currentEvName
        Test-ReadOnlyManifest -RootPath "$DriveLetter\" -ExpectedManifest $linuxManifest
        Stop-ZeroExt4Mount -MountHandle $mountHandle -StopEventName $currentEvName -MountPoint $DriveLetter
        $mountHandle = $null

        $currentEvName = "Local\ZeroExt4_Verify_DirB_RW_${modeName}_$PID"
        Write-Host "[3/6] Mounting $DriveLetter ($($mode.ExtraArgs -join ' ')) and modifying Linux-created files from Windows..."
        $mountHandle = Start-ZeroExt4Mount -ExePath $ZeroExt4Exe -ImagePath $linuxImgPath -MountPoint $DriveLetter -StopEventName $currentEvName -ExtraArgs $mode.ExtraArgs
        $modifiedManifest = Invoke-LinuxImageModifications -RootPath "$DriveLetter\"

        Write-Host "[4/6] Unmounting cleanly via stop event..."
        Stop-ZeroExt4Mount -MountHandle $mountHandle -StopEventName $currentEvName -MountPoint $DriveLetter
        $mountHandle = $null

        $wslLinuxImgPath = ConvertTo-WslPath -WindowsPath $linuxImgPath
        Write-Host "[5/6] Running upstream e2fsck -fn $wslLinuxImgPath ..."
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $e2fsckOut = & wsl.exe -- e2fsck -fn $wslLinuxImgPath 2>&1 | ForEach-Object { "$_" }
            $e2fsckExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $prevEap
        }
        $e2fsckOut | ForEach-Object { Write-Host "  [e2fsck] $_" }
        if ($e2fsckExit -ne 0) {
            throw "e2fsck -fn FAILED for Direction B $modeName with exit code $e2fsckExit!"
        }
        Write-Host "  [e2fsck] PASSED (exit code 0)."

        Write-Host "[6/6] Cross-verifying modified image via real WSL2 Linux kernel mount -t ext4 -o loop,ro..."
        Test-WslKernelLoopManifest -WindowsImgPath $linuxImgPath -ExpectedManifest $modifiedManifest -Tag "dirB_after_${modeName}"

        Write-Host "RESULT [Direction B / $modeName]: PASS"
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
        if (-not $KeepImage -and (Test-Path $linuxImgPath)) {
            Remove-Item -Force $linuxImgPath -ErrorAction SilentlyContinue
        }
    }
}

Write-Host ""
Write-Host "ALL MODES PASSED (Direction A & Direction B for rw-default + rw-fast-io): e2fsck -fn, Linux kernel loop mount & RO SHA-256 verified."
