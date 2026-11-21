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

$EtFiles = @(
    "com_err.h", "error_table.h", "internal.h",
    "com_err.c", "error_message.c", "et_name.c", "init_et.c", "com_right.c"
)
$E2pFiles = @("e2p.h")
$Ext2HeadersAndTemplates = @(
    "ext2_err.et.in", "ext2_types.h.in",
    "bitops.h", "bmap64.h", "compiler.h", "crc16.h", "crc32c_defs.h",
    "e2image.h", "ext2_ext_attr.h", "ext2_fs.h", "ext2_io.h", "ext2fs.h",
    "ext2fsP.h", "ext3_extents.h", "ext4_acl.h", "fast_commit.h", "fiemap.h",
    "hashmap.h", "jfs_compat.h", "kernel-jbd.h", "kernel-list.h", "rbtree.h",
    "utf8data.h", "utf8n.h"
)
$Ext2Sources = @(
    "alloc.c", "alloc_sb.c", "alloc_stats.c", "alloc_tables.c", "badblocks.c",
    "bitmaps.c", "bitops.c", "blkmap64_ba.c", "blkmap64_rb.c", "blknum.c",
    "block.c", "bmap.c", "check_desc.c", "closefs.c", "crc16.c", "crc32c.c",
    "csum.c", "dir_iterate.c", "dirblock.c", "dirhash.c", "expanddir.c",
    "ext_attr.c", "extent.c", "fallocate.c", "fileio.c", "freefs.c",
    "gen_bitmap.c", "gen_bitmap64.c", "get_num_dirs.c", "getenv.c",
    "hashmap.c", "i_block.c", "ind_block.c", "initialize.c", "inline.c",
    "inline_data.c", "inode.c", "io_manager.c", "ismounted.c", "link.c",
    "lookup.c", "mkdir.c", "mkjournal.c", "mmp.c", "namei.c", "native.c",
    "newdir.c", "nls_utf8.c", "openfs.c", "punch.c", "rbtree.c", "read_bb.c",
    "rw_bitmaps.c", "sha512.c", "swapfs.c", "symlink.c", "unlink.c",
    "valid_blk.c", "version.c"
)

foreach ($sub in @("lib\et", "lib\e2p", "lib\ext2fs")) {
    $destSub = Join-Path $TargetRoot $sub
    if (Test-Path $destSub) {
        Remove-Item -Recurse -Force $destSub
    }
    New-Item -ItemType Directory -Force -Path $destSub | Out-Null
}

$VersionH = Join-Path $UpstreamDir "version.h"
$VersionTxt = Join-Path $UpstreamDir "VERSION"
if (Test-Path $VersionH) {
    Copy-Item $VersionH (Join-Path $TargetRoot "version.h") -Force
    $vhText = Get-Content -Raw -Path $VersionH
    if ($vhText -match 'E2FSPROGS_VERSION\s+"([^"]+)"') {
        [System.IO.File]::WriteAllText(
            (Join-Path $TargetRoot "VERSION"),
            ($Matches[1] + "`n"),
            [System.Text.UTF8Encoding]::new($false)
        )
    } elseif (Test-Path $VersionTxt) {
        Copy-Item $VersionTxt (Join-Path $TargetRoot "VERSION") -Force
    }
} elseif (Test-Path $VersionTxt) {
    Copy-Item $VersionTxt (Join-Path $TargetRoot "VERSION") -Force
}

function Copy-UpstreamGroup {
    param(
        [string]$SubDir,
        [string[]]$Files,
        [switch]$Required
    )
    foreach ($f in $Files) {
        $src = Join-Path $UpstreamDir "$SubDir\$f"
        $dst = Join-Path $TargetRoot "$SubDir\$f"
        if (Test-Path $src) {
            Copy-Item $src $dst -Force
            Write-Host "Synced $SubDir/$f"
        } elseif ($Required) {
            throw "Required upstream file missing: $src"
        } else {
            Write-Warning "Optional/missing upstream file skipped: $SubDir/$f"
        }
    }
}

Copy-UpstreamGroup -SubDir "lib\et" -Files $EtFiles -Required
Copy-UpstreamGroup -SubDir "lib\e2p" -Files $E2pFiles -Required
Copy-UpstreamGroup -SubDir "lib\ext2fs" -Files $Ext2HeadersAndTemplates -Required
Copy-UpstreamGroup -SubDir "lib\ext2fs" -Files $Ext2Sources -Required

# Apply the 4 minimal MSVC C11 syntax fixes:
# 1. ext2fs.h: EXT2_QSORT_TYPE int __cdecl -> int (MSVC rejects 'int __cdecl (*sortfunc)')
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$ext2fsH = Join-Path $TargetRoot "lib\ext2fs\ext2fs.h"
$ext2fsText = [System.IO.File]::ReadAllText($ext2fsH)
if ($ext2fsText.Contains("#define EXT2_QSORT_TYPE int __cdecl")) {
    $ext2fsText = $ext2fsText.Replace("#define EXT2_QSORT_TYPE int __cdecl", "#define EXT2_QSORT_TYPE int")
    [System.IO.File]::WriteAllText($ext2fsH, $ext2fsText, $utf8NoBom)
    Write-Host "Patched lib/ext2fs/ext2fs.h (EXT2_QSORT_TYPE for MSVC C11)"
}

# 2-4. alloc.c, punch.c, fallocate.c: GNU named variadic macro 'dbg_printf(f, a...)' -> C99 '...'
foreach ($varFile in @("alloc.c", "punch.c", "fallocate.c")) {
    $vp = Join-Path $TargetRoot "lib\ext2fs\$varFile"
    $vt = [System.IO.File]::ReadAllText($vp)
    $orig = $vt
    $vt = [regex]::Replace($vt, 'dbg_printf\(f,\s*a\.\.\.\)', 'dbg_printf(f, ...)')
    $vt = [regex]::Replace($vt, 'printf\(f,\s*##\s*a\)', 'printf(f, __VA_ARGS__)')
    if ($vt -ne $orig) {
        [System.IO.File]::WriteAllText($vp, $vt, $utf8NoBom)
        Write-Host "Patched lib/ext2fs/$varFile (C99 variadic dbg_printf for MSVC C11)"
    }
}

Write-Host "Sync complete. Minimal MSVC C11 compatibility edits applied: ext2fs.h, alloc.c, punch.c, fallocate.c."

if (-not $SkipBuild) {
    & "$PSScriptRoot\build_and_test.ps1" -BuildDir $BuildDir
}
