param(
    [Parameter(Mandatory = $true)]
    [string]$UpstreamDir,

    [string]$BuildDir = "$PSScriptRoot\..\build",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
$TargetRoot = Join-Path $RepoRoot "third_party\e2fsprogs"

if (-not (Test-Path (Join-Path $UpstreamDir "lib\ext2fs\ext2fs.h"))) {
    throw "UpstreamDir '$UpstreamDir' does not look like an e2fsprogs source tree (lib/ext2fs/ext2fs.h missing)."
}

$EtFiles = @("com_err.h", "error_table.h", "com_err.c")
$Ext2Files = @(
    "ext2_err.et.in", "ext2_types.h.in", "ext2_fs.h", "ext2_io.h", "ext2fs.h",
    "io_manager.c", "openfs.c", "closefs.c", "freefs.c", "initialize.c",
    "blknum.c", "bitmaps.c", "rw_bitmaps.c", "bitops.c", "gen_bitmap64.c",
    "alloc.c", "alloc_sb.c", "alloc_stats.c", "alloc_tables.c",
    "inode.c", "extent.c", "bmap.c", "block.c", "ind_block.c",
    "fileio.c", "inline.c", "inline_data.c",
    "dirblock.c", "dir_iterate.c", "lookup.c", "namei.c",
    "mkdir.c", "newdir.c", "link.c", "unlink.c", "symlink.c",
    "crc16.c", "crc32c.c", "csum.c"
)

New-Item -ItemType Directory -Force -Path (Join-Path $TargetRoot "lib\et") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $TargetRoot "lib\ext2fs") | Out-Null

if (Test-Path (Join-Path $UpstreamDir "version.h")) {
    Copy-Item (Join-Path $UpstreamDir "version.h") (Join-Path $TargetRoot "VERSION") -Force
} elseif (Test-Path (Join-Path $UpstreamDir "VERSION")) {
    Copy-Item (Join-Path $UpstreamDir "VERSION") (Join-Path $TargetRoot "VERSION") -Force
}

foreach ($f in $EtFiles) {
    $src = Join-Path $UpstreamDir "lib\et\$f"
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $TargetRoot "lib\et\$f") -Force
        Write-Host "Synced lib/et/$f"
    }
}

foreach ($f in $Ext2Files) {
    $src = Join-Path $UpstreamDir "lib\ext2fs\$f"
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $TargetRoot "lib\ext2fs\$f") -Force
        Write-Host "Synced lib/ext2fs/$f"
    } else {
        Write-Warning "Optional/missing upstream file skipped: lib/ext2fs/$f"
    }
}

Write-Host "Sync complete. Note: third_party/e2fsprogs contains zero Windows-specific patches."

if (-not $SkipBuild) {
    & "$PSScriptRoot\build_and_test.ps1" -BuildDir $BuildDir
}
