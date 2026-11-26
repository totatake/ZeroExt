param(
    [string]$WorkDir = "$env:LOCALAPPDATA\Temp\zeroext4_ssd_bench",
    [int]$FileSizeMB = 512,
    [int]$VhdSizeMB = 1024,
    [string]$MountDrive = "W:",
    [string]$OutFile = "$PSScriptRoot\..\build\bench_ssd_compare_results.txt",
    [switch]$KeepTemp
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# ---- 1. Elevation check ----
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "ERROR: Administrator privileges are required." -ForegroundColor Red
    Write-Host "Reason: Mount-VHD, \\.\PhysicalDriveN access, and wsl --mount --vhd require elevation." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Run this command from an Administrator PowerShell window:" -ForegroundColor Cyan
    Write-Host "  powershell -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit 1
}

# ---- 2. Pre-flight validation ----
$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
$ExeCli   = Join-Path $RepoRoot "build\zeroext4.exe"
$ExeBench = Join-Path $RepoRoot "build\zeroext4_bench.exe"

if (-not (Test-Path $ExeCli))   { throw "zeroext4.exe not found: $ExeCli (run scripts\build_and_test.ps1 first)" }
if (-not (Test-Path $ExeBench)) { throw "zeroext4_bench.exe not found: $ExeBench (run scripts\build_and_test.ps1 first)" }

if ($VhdSizeMB -lt ($FileSizeMB + 128)) {
    throw "VhdSizeMB ($VhdSizeMB) must be at least FileSizeMB ($FileSizeMB) + 128 MB."
}

$MountDrive = $MountDrive.TrimEnd('\')
if ($MountDrive -notmatch '^[A-Za-z]:$') {
    throw "MountDrive must be a drive letter such as 'W:': $MountDrive"
}
if (Test-Path "$MountDrive\") {
    throw "Drive $MountDrive is already in use. Specify a free drive letter via -MountDrive."
}

$outParent = Split-Path -Parent $OutFile
if ($outParent -and -not (Test-Path $outParent)) {
    New-Item -ItemType Directory -Force -Path $outParent | Out-Null
}
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
Set-Content -Path $OutFile -Value "" -Encoding UTF8

function Write-Log {
    param([string]$Message = "")
    Write-Host $Message
    Add-Content -Path $OutFile -Value $Message -Encoding UTF8
}

function Write-Utf8LfFile {
    param([string]$Path, [string]$Text)
    $lf = $Text -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText($Path, $lf, (New-Object System.Text.UTF8Encoding($false)))
}

function ConvertTo-WslPath {
    param([string]$WinPath)
    $full = [System.IO.Path]::GetFullPath($WinPath)
    if ($full -match '^([A-Za-z]):\\(.*)$') {
        $drv = $Matches[1].ToLowerInvariant()
        $rest = $Matches[2] -replace '\\', '/'
        return "/mnt/$drv/$rest"
    }
    throw "Cannot convert Windows path to WSL path: $WinPath"
}

$NativeFile = Join-Path $WorkDir "native_model.gguf"
$VhdxPath   = Join-Path $WorkDir "bench_ext4.vhdx"
$InitShWin  = Join-Path $WorkDir "init_vhdx.sh"
$WslCWin    = Join-Path $WorkDir "wsl_ext4_bench.c"
$MountOut   = Join-Path $WorkDir "zeroext4_mount.out.log"
$MountErr   = Join-Path $WorkDir "zeroext4_mount.err.log"

$WslVhdxMounted = $false
$HyperVVhdMounted = $false
$ZeroExt4Proc = $null

$Results = New-Object System.Collections.Generic.List[object]

function Invoke-WinBench {
    param(
        [string]$Backend,
        [string]$TargetPath,
        [int]$ChunkKB,
        [string]$CacheMode,
        [string]$AccessPattern,
        [string]$WarmMode,
        [string]$WorkloadLabel
    )
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $ExeBench --read $TargetPath $ChunkKB $CacheMode $AccessPattern $WarmMode 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $prevEap

    foreach ($line in $out) {
        $s = $line.ToString().TrimEnd()
        if ($s) { Write-Log "  [$Backend] $s" }
        if ($s -match '^(UNBUF|CACHED-WARM|CACHED)\s+(SEQ|RND)\s+\((\d+KB)\)\s+\|\s+bytes=(\d+\s*MB)\s+\|\s+([\d.]+)\s+s\s+\|\s+([\d.]+)\s+MB/s\s+\|\s+([\d.]+)\s+IOPS') {
            $Results.Add([PSCustomObject]@{
                Backend  = $Backend
                Workload = $WorkloadLabel
                Secs     = [double]$Matches[5]
                MBps     = [double]$Matches[6]
                IOPS     = [double]$Matches[7]
            })
        }
    }
    if ($exitCode -ne 0) {
        throw "Benchmark failed ($Backend / $WorkloadLabel, exit=$exitCode)"
    }
}

function Invoke-WslDropCaches {
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & wsl.exe -u root sh -c 'sync && echo 3 > /proc/sys/vm/drop_caches'
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    if ($rc -ne 0) {
        throw "Failed to run WSL2 drop_caches (exit=$rc)"
    }
}

function Stop-ZeroExt4Mount {
    if ($script:ZeroExt4Proc -and -not $script:ZeroExt4Proc.HasExited) {
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try { & $ExeCli unmount $MountDrive 2>$null | Out-Null } catch {}
        $ErrorActionPreference = $prevEap
        if (-not $script:ZeroExt4Proc.WaitForExit(5000)) {
            try { Stop-Process -Id $script:ZeroExt4Proc.Id -Force -ErrorAction SilentlyContinue } catch {}
            $script:ZeroExt4Proc.WaitForExit(3000) | Out-Null
        }
    }
    $script:ZeroExt4Proc = $null
}

function Start-ZeroExt4Mount {
    param([string]$PhysPath, [bool]$DirectIo)
    Remove-Item -Force -ErrorAction SilentlyContinue $MountOut, $MountErr
    $argList = @("mount", "--source", $PhysPath, "--partition", "1", "--mountpoint", $MountDrive)
    if (-not $DirectIo) { $argList += "--no-direct-io" }

    $script:ZeroExt4Proc = Start-Process -FilePath $ExeCli -ArgumentList $argList -PassThru -NoNewWindow -RedirectStandardOutput $MountOut -RedirectStandardError $MountErr
    $mountedFile = "$MountDrive\model.gguf"
    $ready = $false
    for ($i = 0; $i -lt 50; $i++) {
        if ($script:ZeroExt4Proc.HasExited) {
            $errText = if (Test-Path $MountErr) { Get-Content $MountErr -Raw } else { "" }
            $outText = if (Test-Path $MountOut) { Get-Content $MountOut -Raw } else { "" }
            throw "zeroext4 mount exited unexpectedly (code=$($script:ZeroExt4Proc.ExitCode))`nSTDOUT: $outText`nSTDERR: $errText"
        }
        if (Test-Path $mountedFile) {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 200
    }
    if (-not $ready) {
        Stop-ZeroExt4Mount
        throw "Timed out waiting for $mountedFile (10s)"
    }
}

try {
    # ---- 3. Hardware / Software Metadata ----
    $workDrive = (Split-Path -Qualifier $WorkDir).TrimEnd(':')
    $diskInfo  = Get-Partition -DriveLetter $workDrive | Get-Disk
    $physInfo  = Get-PhysicalDisk | Where-Object { [int]$_.DeviceId -eq [int]$diskInfo.Number }
    $cpuName   = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
    $ramGB     = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
    $osVer     = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion"

    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $WslDistro = ((& wsl.exe -e printenv WSL_DISTRO_NAME 2>&1) | Out-String).Trim()
    $rcDistro = $LASTEXITCODE
    $WslKernel = ((& wsl.exe -e uname -r 2>&1) | Out-String).Trim()
    $rcKernel = $LASTEXITCODE
    $wslVerRaw = ((& wsl.exe --version 2>&1 | Out-String) -replace "`0", "")
    $ErrorActionPreference = $prevEap

    if ($rcDistro -ne 0 -or -not $WslDistro) {
        throw "Failed to inspect WSL2 distro name: '$WslDistro' (exit=$rcDistro)"
    }
    if ($rcKernel -ne 0 -or -not $WslKernel) {
        throw "Failed to inspect WSL2 kernel version: '$WslKernel' (exit=$rcKernel)"
    }
    $WslVersion = if ($wslVerRaw -match '(\d+\.\d+\.\d+\.\d+)') { $Matches[1] } else { "unknown" }

    Write-Log "========================================================================"
    Write-Log "  Same-SSD Benchmark (Windows NTFS / ZeroExt4 / WSL2)"
    Write-Log "========================================================================"
    Write-Log "Timestamp    : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')"
    Write-Log "CPU          : $cpuName"
    Write-Log "RAM          : $ramGB GB"
    Write-Log "OS           : $($osVer.ProductName) $($osVer.DisplayVersion) (Build $($osVer.CurrentBuild))"
    Write-Log "Target SSD   : Disk $($diskInfo.Number) [$($diskInfo.FriendlyName)] Bus=$($diskInfo.BusType) Media=$($physInfo.MediaType) Size=$([math]::Round($diskInfo.Size/1GB, 1)) GB (${workDrive}:)"
    Write-Log "WorkDir      : $WorkDir"
    Write-Log "Test Fixture : ${FileSizeMB} MB contiguous file / VHDX=${VhdSizeMB} MB (GPT Partition 1 / ext4)"
    Write-Log "WSL2 Env     : WSL=$WslVersion | Distro=$WslDistro | Kernel=$WslKernel"
    Write-Log ""

    # ---- 4. Create identical 512MB fixtures on the same NVMe SSD ----
    Write-Log "[Prep 1/3] Creating Windows Native NTFS ${FileSizeMB} MB fixture ($NativeFile)..."
    $fs = [System.IO.File]::Open($NativeFile, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try {
        $chunkSize = 1048576
        $buf = New-Object byte[] $chunkSize
        for ($i = 0; $i -lt $FileSizeMB; $i++) {
            [byte]$val = ($i -bxor 0xA5) -band 0xFF
            for ($k = 0; $k -lt 64; $k++) { $buf[$k] = $val }
            $filled = 64
            while ($filled -lt $chunkSize) {
                $copyLen = [Math]::Min($filled, $chunkSize - $filled)
                [System.Buffer]::BlockCopy($buf, 0, $buf, $filled, $copyLen)
                $filled += $copyLen
            }
            $fs.Write($buf, 0, $chunkSize)
        }
        $fs.Flush($true)
    } finally {
        $fs.Dispose()
    }

    Write-Log "[Prep 2/3] Creating Fixed VHDX ($VhdxPath, ${VhdSizeMB} MB)..."
    Remove-Item -Force -ErrorAction SilentlyContinue $VhdxPath
    Import-Module Hyper-V
    New-VHD -Path $VhdxPath -SizeBytes ($VhdSizeMB * 1MB) -Fixed -LogicalSectorSizeBytes 512 -PhysicalSectorSizeBytes 4096 | Out-Null

    Write-Log "[Prep 3/3] Creating GPT + ext4 (Partition 1) and writing /model.gguf (${FileSizeMB} MB) via WSL2..."
    $beforeDevs = @(& wsl.exe -u root lsblk -dn -o NAME | ForEach-Object { $_.Trim() } | Where-Object { $_ -and $_ -match '^[a-zA-Z0-9_-]+$' })
    & wsl.exe --mount "$VhdxPath" --vhd --bare | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "wsl --mount --vhd --bare failed (exit=$LASTEXITCODE)" }
    $WslVhdxMounted = $true

    $newDev = $null
    for ($i = 0; $i -lt 15; $i++) {
        Start-Sleep -Milliseconds 250
        $afterDevs = @(& wsl.exe -u root lsblk -dn -o NAME | ForEach-Object { $_.Trim() } | Where-Object { $_ -and $_ -match '^[a-zA-Z0-9_-]+$' })
        $diff = @($afterDevs | Where-Object { $_ -notin $beforeDevs })
        if ($diff.Count -eq 1) {
            $newDev = $diff[0]
            break
        }
    }
    if (-not $newDev) {
        throw "Could not uniquely identify newly attached VHDX block device in WSL2."
    }

    $expectedVhdBytes = [int64]$VhdSizeMB * 1048576
    $initScript = @'
#!/bin/sh
set -eu
DEV="$1"
EXPECTED_BYTES="$2"
FILE_MB="$3"
MNT="/mnt/ext4bench_init_$$"

cleanup() {
    if mountpoint -q "$MNT" 2>/dev/null; then
        umount "$MNT" || true
    fi
    rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

ACTUAL_BYTES=$(blockdev --getsize64 "/dev/$DEV")
if [ "$ACTUAL_BYTES" -ne "$EXPECTED_BYTES" ]; then
    echo "ERROR: /dev/$DEV size mismatch: actual=$ACTUAL_BYTES expected=$EXPECTED_BYTES" >&2
    exit 1
fi
EXISTING_FS=$(lsblk -no FSTYPE "/dev/$DEV" | tr -d '[:space:]')
if [ -n "$EXISTING_FS" ]; then
    echo "ERROR: /dev/$DEV unexpectedly has existing filesystem '$EXISTING_FS'" >&2
    exit 1
fi

printf 'label: gpt\n, , 0FC63DAF-8483-4772-8E79-3D69D8477DE4, -\n' | sfdisk "/dev/$DEV" >/dev/null
blockdev --rereadpt "/dev/$DEV" >/dev/null 2>&1 || true

PART="/dev/${DEV}1"
i=0
while [ ! -b "$PART" ] && [ "$i" -lt 25 ]; do
    sleep 0.2
    i=$((i + 1))
done
if [ ! -b "$PART" ]; then
    echo "ERROR: partition node $PART did not appear" >&2
    exit 1
fi

mkfs.ext4 -q -F -L BENCH "$PART"
mkdir -p "$MNT"
mount "$PART" "$MNT"
python3 - "$FILE_MB" "$MNT/model.gguf" << 'PYEOF'
import sys
n = int(sys.argv[1])
out_path = sys.argv[2]
chunk = 1024 * 1024
with open(out_path, "wb") as f:
    for i in range(n):
        b = (i ^ 0xA5) & 0xFF
        f.write(bytes([b]) * chunk)
PYEOF
sync
umount "$MNT"
rmdir "$MNT"
echo "Initialized GPT + ext4 on $PART ($FILE_MB MB /model.gguf)"
'@
    Write-Utf8LfFile -Path $InitShWin -Text $initScript
    $InitShWsl = ConvertTo-WslPath $InitShWin

    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $initOut = & wsl.exe -u root /bin/sh "$InitShWsl" "$newDev" "$expectedVhdBytes" "$FileSizeMB" 2>&1
    $initExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap

    foreach ($l in $initOut) { Write-Log "  [init] $($l.ToString().TrimEnd())" }
    if ($initExit -ne 0) { throw "ext4 VHDX initialization failed (exit=$initExit)" }

    & wsl.exe --unmount "$VhdxPath" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "wsl --unmount failed after initialization" }
    $WslVhdxMounted = $false
    Write-Log ""

    # ---- 5. Benchmark Suite Definitions ----
    $suite = @(
        @{ Cache = "unbuf";  Pat = "seq"; KB = 1024; Warm = "cold"; Label = "Cold/Unbuf Seq 1024KB" },
        @{ Cache = "unbuf";  Pat = "seq"; KB = 64;   Warm = "cold"; Label = "Cold/Unbuf Seq 64KB"   },
        @{ Cache = "unbuf";  Pat = "rnd"; KB = 4;    Warm = "cold"; Label = "Cold/Unbuf Rnd 4KB"    },
        @{ Cache = "cached"; Pat = "seq"; KB = 1024; Warm = "warm"; Label = "Warm/Cached Seq 1024KB"},
        @{ Cache = "cached"; Pat = "seq"; KB = 64;   Warm = "warm"; Label = "Warm/Cached Seq 64KB"  },
        @{ Cache = "cached"; Pat = "rnd"; KB = 4;    Warm = "warm"; Label = "Warm/Cached Rnd 4KB"   }
    )

    # ---- Phase 1: Windows Native (NTFS) ----
    Write-Log "=== [1/5] Windows Native (NTFS: $NativeFile) ==="
    foreach ($t in $suite) {
        Invoke-WinBench -Backend "Native NTFS" -TargetPath $NativeFile -ChunkKB $t.KB -CacheMode $t.Cache -AccessPattern $t.Pat -WarmMode $t.Warm -WorkloadLabel $t.Label
    }
    Write-Log ""

    # ---- Phase 2 & 3: ZeroExt4 via Mount-VHD (PhysicalDriveN, unbuffered device handle) ----
    $vhdObj = Mount-VHD -Path $VhdxPath -ReadOnly -NoDriveLetter -PassThru
    $HyperVVhdMounted = $true
    if ($null -eq $vhdObj.DiskNumber) {
        $vhdObj = Get-VHD -Path $VhdxPath
    }
    Start-Sleep -Milliseconds 300
    $physPath = "\\.\PhysicalDrive$($vhdObj.DiskNumber)"
    Write-Log "=== VHD attached for ZeroExt4: $physPath (Partition 1 / extent check) ==="
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $extOut = & $ExeCli extents $physPath /model.gguf --partition 1 2>&1
    $ErrorActionPreference = $prevEap
    foreach ($l in $extOut) { Write-Log "  $($l.ToString().TrimEnd())" }
    Write-Log ""

    Write-Log "=== [2/5] ZeroExt4 (Standard: --no-direct-io, $physPath -> $MountDrive) ==="
    Start-ZeroExt4Mount -PhysPath $physPath -DirectIo $false
    foreach ($t in $suite) {
        Invoke-WinBench -Backend "ZeroExt4 (Standard)" -TargetPath "$MountDrive\model.gguf" -ChunkKB $t.KB -CacheMode $t.Cache -AccessPattern $t.Pat -WarmMode $t.Warm -WorkloadLabel $t.Label
    }
    Stop-ZeroExt4Mount
    Write-Log ""

    Write-Log "=== [3/5] ZeroExt4 (Default: Direct I/O ON, $physPath -> $MountDrive) ==="
    Start-ZeroExt4Mount -PhysPath $physPath -DirectIo $true
    foreach ($t in $suite) {
        Invoke-WinBench -Backend "ZeroExt4 (Default DIO)" -TargetPath "$MountDrive\model.gguf" -ChunkKB $t.KB -CacheMode $t.Cache -AccessPattern $t.Pat -WarmMode $t.Warm -WorkloadLabel $t.Label
    }
    Stop-ZeroExt4Mount
    Write-Log ""

    Dismount-VHD -Path $VhdxPath
    $HyperVVhdMounted = $false
    Start-Sleep -Seconds 1

    # ---- Phase 4 & 5: wsl --mount --vhd ----
    Write-Log "=== [4/5 & 5/5] wsl --mount --vhd (partition=1, type=ext4, name=ext4bench) ==="
    & wsl.exe --mount "$VhdxPath" --vhd --partition 1 --type ext4 --name ext4bench | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "wsl --mount --vhd --partition 1 failed (exit=$LASTEXITCODE)" }
    $WslVhdxMounted = $true

    $wslReady = $false
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    for ($i = 0; $i -lt 20; $i++) {
        & wsl.exe -e test -f /mnt/wsl/ext4bench/model.gguf 2>$null
        if ($LASTEXITCODE -eq 0) { $wslReady = $true; break }
        Start-Sleep -Milliseconds 200
    }
    $ErrorActionPreference = $prevEap
    if (-not $wslReady) { throw "/mnt/wsl/ext4bench/model.gguf was not mounted inside WSL2." }

    $wslUncPath = "\\wsl.localhost\$WslDistro\mnt\wsl\ext4bench\model.gguf"
    Write-Log "--- [4/5] WSL2 via 9P (Windows App -> $wslUncPath) ---"
    foreach ($t in $suite) {
        if ($t.Warm -eq "cold") { Invoke-WslDropCaches }
        Invoke-WinBench -Backend "WSL2 9P (\\wsl.localhost)" -TargetPath $wslUncPath -ChunkKB $t.KB -CacheMode $t.Cache -AccessPattern $t.Pat -WarmMode $t.Warm -WorkloadLabel $t.Label
    }
    Write-Log ""

    # Prepare Linux C benchmark tool matching bench_direct_io.c LCG + flags
    $wslBenchC = @'
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double elapsed_sec(struct timespec t0, struct timespec t1) {
    double s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    return s > 0.0 ? s : 1e-9;
}

static uint32_t lcg_next(uint32_t *state) {
    *state = (*state) * 1664525U + 1013904223U;
    return *state;
}

int main(int argc, char **argv) {
    if (argc < 6) return 1;
    const char *path = argv[1];
    uint32_t chunk_kb = (uint32_t)strtoul(argv[2], NULL, 10);
    int unbuf = (strcmp(argv[3], "unbuf") == 0);
    int rnd   = (strcmp(argv[4], "rnd") == 0);
    int warm  = (strcmp(argv[5], "warm") == 0);
    uint64_t rnd_ops = (argc >= 7) ? strtoull(argv[6], NULL, 10) : 16384ULL;
    size_t chunk_sz = (size_t)chunk_kb * 1024U;
    int flags = O_RDONLY | (unbuf ? O_DIRECT : 0);
    int fd = open(path, flags);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < chunk_sz) {
        fprintf(stderr, "fstat failed or file too small\n");
        close(fd);
        return 1;
    }

    if (!unbuf) {
        (void)posix_fadvise(fd, 0, 0, rnd ? POSIX_FADV_RANDOM : POSIX_FADV_SEQUENTIAL);
    }

    if (warm && !unbuf) {
        void *wbuf = NULL;
        size_t wchunk = 1024U * 1024U;
        if (posix_memalign(&wbuf, 4096, wchunk) != 0) { close(fd); return 1; }
        ssize_t r;
        while ((r = read(fd, wbuf, wchunk)) > 0) {}
        free(wbuf);
        if (lseek(fd, 0, SEEK_SET) < 0) { close(fd); return 1; }
    }

    void *buf = NULL;
    if (posix_memalign(&buf, 4096, chunk_sz) != 0) { close(fd); return 1; }

    struct timespec t0, t1;
    uint64_t total_done = 0;
    uint64_t ops = 0;
    uint32_t seed = 0xCAFEBABEU;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (!rnd) {
        ssize_t got;
        while ((got = read(fd, buf, chunk_sz)) > 0) {
            total_done += (uint64_t)got;
            ops++;
        }
    } else {
        uint64_t pages = (uint64_t)st.st_size / chunk_sz;
        ops = rnd_ops;
        for (uint64_t i = 0; i < ops; i++) {
            uint64_t off = ((uint64_t)lcg_next(&seed) % pages) * (uint64_t)chunk_sz;
            if (lseek(fd, (off_t)off, SEEK_SET) < 0) break;
            ssize_t got = read(fd, buf, chunk_sz);
            if (got <= 0) break;
            total_done += (uint64_t)got;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    free(buf);
    close(fd);

    if (total_done == 0) {
        fprintf(stderr, "No bytes read from %s\n", path);
        return 1;
    }
    double secs = elapsed_sec(t0, t1);
    double mbps = ((double)total_done / (1024.0 * 1024.0)) / secs;
    double iops = (double)ops / secs;
    const char *cache_lbl = unbuf ? "UNBUF" : (warm ? "CACHED-WARM" : "CACHED");
    printf("%s %s (%uKB) | bytes=%llu MB | %8.3f s | %9.1f MB/s | %9.0f IOPS\n",
           cache_lbl, rnd ? "RND" : "SEQ", chunk_kb,
           (unsigned long long)(total_done / (1024ULL * 1024ULL)),
           secs, mbps, iops);
    return 0;
}
'@
    Write-Utf8LfFile -Path $WslCWin -Text $wslBenchC
    $WslCWsl = ConvertTo-WslPath $WslCWin
    & wsl.exe -u root gcc -O2 -Wall "$WslCWsl" -o /tmp/wsl_ext4_bench
    if ($LASTEXITCODE -ne 0) { throw "Failed to compile WSL2 C benchmark helper." }

    Write-Log "--- [5/5] WSL2 Linux Kernel Direct (/mnt/wsl/ext4bench/model.gguf, Reference) ---"
    foreach ($t in $suite) {
        if ($t.Warm -eq "cold") { Invoke-WslDropCaches }
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $wOut = & wsl.exe -u root /tmp/wsl_ext4_bench /mnt/wsl/ext4bench/model.gguf $t.KB $t.Cache $t.Pat $t.Warm 16384 2>&1
        $wExit = $LASTEXITCODE
        $ErrorActionPreference = $prevEap
        foreach ($line in $wOut) {
            $s = $line.ToString().TrimEnd()
            if ($s) { Write-Log "  [WSL2 Linux Kernel] $s" }
            if ($s -match '^(UNBUF|CACHED-WARM|CACHED)\s+(SEQ|RND)\s+\((\d+KB)\)\s+\|\s+bytes=(\d+\s*MB)\s+\|\s+([\d.]+)\s+s\s+\|\s+([\d.]+)\s+MB/s\s+\|\s+([\d.]+)\s+IOPS') {
                $Results.Add([PSCustomObject]@{
                    Backend  = "WSL2 Linux Kernel (Ref)"
                    Workload = $t.Label
                    Secs     = [double]$Matches[5]
                    MBps     = [double]$Matches[6]
                    IOPS     = [double]$Matches[7]
                })
            }
        }
        if ($wExit -ne 0) { throw "WSL2 kernel benchmark failed ($($t.Label), exit=$wExit)" }
    }
    Write-Log ""

    & wsl.exe -u root rm -f /tmp/wsl_ext4_bench 2>$null | Out-Null
    & wsl.exe --unmount "$VhdxPath" | Out-Null
    $WslVhdxMounted = $false

    # ---- 6. Comparison Summary Table ----
    $backends = @(
        "Native NTFS",
        "ZeroExt4 (Standard)",
        "ZeroExt4 (Default DIO)",
        "WSL2 9P (\\wsl.localhost)",
        "WSL2 Linux Kernel (Ref)"
    )

    Write-Log "========================================================================"
    Write-Log "  Summary Comparison (MB/s)"
    Write-Log "========================================================================"
    $hdr = "{0,-23} | {1,11} | {2,11} | {3,11} | {4,11} | {5,11}" -f `
        "Workload", "Native NTFS", "ZeroExt4 Std", "ZeroExt4 DIO", "WSL2 9P", "WSL2 Kernel"
    Write-Log $hdr
    Write-Log ("-" * $hdr.Length)

    foreach ($t in $suite) {
        $vals = foreach ($b in $backends) {
            $row = $Results | Where-Object { $_.Backend -eq $b -and $_.Workload -eq $t.Label } | Select-Object -First 1
            if ($row) { "{0,11:N1}" -f $row.MBps } else { "{0,11}" -f "N/A" }
        }
        Write-Log ("{0,-23} | {1} | {2} | {3} | {4} | {5}" -f $t.Label, $vals[0], $vals[1], $vals[2], $vals[3], $vals[4])
    }
    Write-Log "========================================================================"
    Write-Log "Saved results to: $OutFile"
}
finally {
    Stop-ZeroExt4Mount
    if ($HyperVVhdMounted) {
        try { Dismount-VHD -Path $VhdxPath -ErrorAction SilentlyContinue } catch {}
    }
    if ($WslVhdxMounted) {
        try { & wsl.exe --unmount "$VhdxPath" 2>$null | Out-Null } catch {}
    }
    if (-not $KeepTemp) {
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $WorkDir
    }
}
