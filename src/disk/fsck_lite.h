#ifndef ZEROEXT4_FSCK_LITE_H
#define ZEROEXT4_FSCK_LITE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fsck_lite_finding {
    char     severity[16];  /* "error", "warning", "info" */
    char     category[32];  /* "superblock", "bgd", "bitmap", "extent", "directory", "orphan", "journal" */
    char     code[48];
    uint64_t inode;
    uint64_t block;
    char     message[256];
} fsck_lite_finding_t;

typedef struct fsck_lite_report {
    int                  healthy;          /* 1 if error_count == 0, else 0 */
    char                 fs_kind[16];
    char                 volume_label[32];
    char                 uuid[40];
    uint32_t             block_size;
    uint32_t             inode_size;
    uint64_t             blocks_count;
    uint64_t             inodes_count;
    uint32_t             group_count;
    uint64_t             checked_inodes;
    uint64_t             checked_extents;
    uint64_t             checked_dirs;
    uint64_t             checked_dirents;
    uint64_t             orphan_count;
    uint64_t             duplicate_blocks;
    char                 journal_state[32];
    uint64_t             error_count;
    uint64_t             warning_count;
    uint64_t             info_count;
    fsck_lite_finding_t *findings;
    size_t               finding_count;
    size_t               finding_cap;
} fsck_lite_report_t;

/*
 * Performs strictly read-only integrity diagnostics across:
 *   1. Superblock & Block Group Descriptors (magic, bounds, CRC32C checksums)
 *   2. Block/inode bitmaps & inode table bounds
 *   3. Extent tree bounds, logical offset monotonicity, and duplicate block detection
 *   4. Directory entries, orphan inode list, and JBD2 journal header
 *
 * Returns 0 if diagnostics completed (inspect out_report->healthy and error_count),
 * or negative errno if arguments are invalid / file cannot be opened.
 */
int ext4_fsck_lite_run(const char *path_utf8, uint64_t offset_bytes, uint64_t size_bytes,
                       uint32_t sector_size, fsck_lite_report_t *out_report);

void ext4_fsck_lite_free_report(fsck_lite_report_t *report);

#ifdef __cplusplus
}
#endif

#endif /* ZEROEXT4_FSCK_LITE_H */
