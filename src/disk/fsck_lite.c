#include "fsck_lite.h"
#include "superblock_probe.h"
#include "../io/win32_io.h"
#include "../common/ext4_errno.h"
#include <ext2fs/ext2fs.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FSCK_LITE_MAX_FINDINGS        256U
#define FSCK_LITE_BITSET_MAX_BLOCKS   16777216ULL /* 2 MB exact claim bitset for <= 64 GB volumes */
#define FSCK_LITE_HASH_SLOTS          131072U     /* 1 MB bounded hash set for very large volumes */
#define FSCK_LITE_HASH_EMPTY          0xFFFFFFFFFFFFFFFFULL

static void add_finding(fsck_lite_report_t *rep, const char *severity, const char *category,
                        const char *code, uint64_t inode, uint64_t block, const char *fmt, ...) {
    va_list ap;
    if (!rep || !severity || !category || !code) return;

    if (strcmp(severity, "error") == 0) {
        rep->error_count++;
    } else if (strcmp(severity, "warning") == 0) {
        rep->warning_count++;
    } else {
        rep->info_count++;
    }

    if (rep->finding_count >= FSCK_LITE_MAX_FINDINGS) {
        return;
    }

    if (rep->finding_count == rep->finding_cap) {
        size_t new_cap = rep->finding_cap ? rep->finding_cap * 2 : 32;
        fsck_lite_finding_t *nb = (fsck_lite_finding_t *)realloc(
            rep->findings, new_cap * sizeof(*nb));
        if (!nb) return;
        rep->findings = nb;
        rep->finding_cap = new_cap;
    }

    {
        fsck_lite_finding_t *f = &rep->findings[rep->finding_count++];
        memset(f, 0, sizeof(*f));
        strncpy_s(f->severity, sizeof(f->severity), severity, _TRUNCATE);
        strncpy_s(f->category, sizeof(f->category), category, _TRUNCATE);
        strncpy_s(f->code, sizeof(f->code), code, _TRUNCATE);
        f->inode = inode;
        f->block = block;
        va_start(ap, fmt);
        _vsnprintf_s(f->message, sizeof(f->message), _TRUNCATE, fmt ? fmt : "", ap);
        va_end(ap);
    }

    if (rep->finding_count == FSCK_LITE_MAX_FINDINGS && rep->finding_count < rep->finding_cap) {
        fsck_lite_finding_t *trunc = &rep->findings[rep->finding_count++];
        memset(trunc, 0, sizeof(*trunc));
        strcpy_s(trunc->severity, sizeof(trunc->severity), "info");
        strcpy_s(trunc->category, sizeof(trunc->category), "superblock");
        strcpy_s(trunc->code, sizeof(trunc->code), "FINDINGS_TRUNCATED");
        _snprintf_s(trunc->message, sizeof(trunc->message), _TRUNCATE,
                    "Finding detail list capped at %u entries; summary counters remain exact.",
                    (unsigned)FSCK_LITE_MAX_FINDINGS);
    }
}

typedef struct blk_claim_tracker {
    uint8_t  *bitset;
    uint64_t  bitset_blocks;
    uint64_t *hash_slots;
    size_t    hash_used;
    int       saturated;
} blk_claim_tracker_t;

static void tracker_init(blk_claim_tracker_t *tr, uint64_t blocks_count) {
    memset(tr, 0, sizeof(*tr));
    if (blocks_count > 0 && blocks_count <= FSCK_LITE_BITSET_MAX_BLOCKS) {
        size_t bytes = (size_t)((blocks_count + 7ULL) / 8ULL);
        tr->bitset = (uint8_t *)calloc(1, bytes ? bytes : 1);
        if (tr->bitset) {
            tr->bitset_blocks = blocks_count;
            return;
        }
    }
    tr->hash_slots = (uint64_t *)malloc(FSCK_LITE_HASH_SLOTS * sizeof(uint64_t));
    if (tr->hash_slots) {
        for (size_t i = 0; i < FSCK_LITE_HASH_SLOTS; i++) {
            tr->hash_slots[i] = FSCK_LITE_HASH_EMPTY;
        }
    }
}

static void tracker_free(blk_claim_tracker_t *tr) {
    free(tr->bitset);
    free(tr->hash_slots);
    memset(tr, 0, sizeof(*tr));
}

/* Returns 1 if block was ALREADY claimed (duplicate), 0 if newly claimed or untracked. */
static int tracker_claim(blk_claim_tracker_t *tr, uint64_t blk) {
    if (tr->bitset && blk < tr->bitset_blocks) {
        size_t idx = (size_t)(blk >> 3);
        uint8_t mask = (uint8_t)(1U << (blk & 7U));
        if (tr->bitset[idx] & mask) return 1;
        tr->bitset[idx] |= mask;
        return 0;
    }
    if (!tr->hash_slots) return 0;

    if (tr->hash_used >= (FSCK_LITE_HASH_SLOTS * 3U) / 4U) {
        /* Switch to read-only duplicate probe when bounded hash table reaches load cap */
        uint64_t h = (blk * 11400714819323198485ULL) >> 47;
        size_t slot = (size_t)(h & (FSCK_LITE_HASH_SLOTS - 1U));
        for (size_t step = 0; step < 64; step++) {
            uint64_t cur = tr->hash_slots[(slot + step) & (FSCK_LITE_HASH_SLOTS - 1U)];
            if (cur == FSCK_LITE_HASH_EMPTY) return 0;
            if (cur == blk) return 1;
        }
        tr->saturated = 1;
        return 0;
    }

    {
        uint64_t h = (blk * 11400714819323198485ULL) >> 47;
        size_t slot = (size_t)(h & (FSCK_LITE_HASH_SLOTS - 1U));
        for (size_t step = 0; step < FSCK_LITE_HASH_SLOTS; step++) {
            size_t pos = (slot + step) & (FSCK_LITE_HASH_SLOTS - 1U);
            if (tr->hash_slots[pos] == blk) return 1;
            if (tr->hash_slots[pos] == FSCK_LITE_HASH_EMPTY) {
                tr->hash_slots[pos] = blk;
                tr->hash_used++;
                return 0;
            }
        }
    }
    return 0;
}

static uint32_t read_be32_u8(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

typedef struct inline_dir_check_ctx {
    fsck_lite_report_t *rep;
    ext2_ino_t          dir_ino;
    uint32_t            inodes_count;
    int                 saw_dot;
    int                 saw_dotdot;
} inline_dir_check_ctx_t;

static int inline_dir_check_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                               int offset, int blocksize, char *buf, void *priv_data) {
    inline_dir_check_ctx_t *c = (inline_dir_check_ctx_t *)priv_data;
    struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)dirent;
    (void)dir;
    (void)entry;
    (void)offset;
    (void)blocksize;
    (void)buf;

    if (!de || de->inode == 0) return 0;
    c->rep->checked_dirents++;
    if (de->inode > c->inodes_count) {
        add_finding(c->rep, "error", "directory", "DIRENT_INODE_OUT_OF_RANGE",
                    c->dir_ino, 0,
                    "Inline dir inode %u references out-of-range inode %u (max=%u).",
                    c->dir_ino, de->inode, c->inodes_count);
    }
    if (de->name_len == 1 && de->name[0] == '.') c->saw_dot = 1;
    if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') c->saw_dotdot = 1;
    return 0;
}

static void check_inode_extents(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode_large *ino_st,
                                blk_claim_tracker_t *tracker, fsck_lite_report_t *rep,
                                uint32_t *dup_findings_emitted) {
    ext2_extent_handle_t eh = NULL;
    struct ext2fs_extent ext;
    errcode_t retval;
    int op = EXT2_EXTENT_ROOT;
    uint64_t prev_end_lblk = 0;
    blk64_t blocks_count = ext2fs_blocks_count(fs->super);
    int check_shared = (fs->super->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_SHARED_BLOCKS) != 0;

    retval = ext2fs_extent_open2(fs, ino, (struct ext2_inode *)ino_st, &eh);
    if (retval != 0) {
        add_finding(rep, "error", "extent", "EXTENT_TREE_CORRUPT", ino, 0,
                    "Failed to open extent tree on inode %u (err=%ld).", ino, (long)retval);
        return;
    }

    while ((retval = ext2fs_extent_get(eh, op, &ext)) == 0) {
        op = EXT2_EXTENT_NEXT;

        if (ext.e_flags & EXT2_EXTENT_FLAGS_SECOND_VISIT) {
            continue;
        }
        if (!(ext.e_flags & EXT2_EXTENT_FLAGS_LEAF)) {
            if (ext.e_pblk == 0 || ext.e_pblk >= blocks_count) {
                add_finding(rep, "error", "extent", "EXTENT_INDEX_PBLK_OUT_OF_RANGE",
                            ino, ext.e_pblk,
                            "Inode %u extent index node points to out-of-range block %llu.",
                            ino, (unsigned long long)ext.e_pblk);
            }
            continue;
        }

        rep->checked_extents++;

        if (ext.e_len == 0) {
            add_finding(rep, "error", "extent", "EXTENT_ZERO_LEN", ino, ext.e_pblk,
                        "Inode %u has zero-length leaf extent at logical block %llu.",
                        ino, (unsigned long long)ext.e_lblk);
            continue;
        }

        if ((uint64_t)ext.e_lblk < prev_end_lblk) {
            add_finding(rep, "error", "extent", "EXTENT_OVERLAP_OR_UNSORTED", ino, ext.e_pblk,
                        "Inode %u leaf extent lblk=%llu overlaps/follows prior end=%llu.",
                        ino, (unsigned long long)ext.e_lblk, (unsigned long long)prev_end_lblk);
        }
        prev_end_lblk = (uint64_t)ext.e_lblk + ext.e_len;

        if (ext.e_pblk == 0 || ext.e_pblk >= blocks_count ||
            ext.e_len > blocks_count || ext.e_pblk + ext.e_len > blocks_count) {
            add_finding(rep, "error", "extent", "EXTENT_PBLK_OUT_OF_RANGE", ino, ext.e_pblk,
                        "Inode %u extent [%llu+%u) exceeds filesystem blocks_count=%llu.",
                        ino, (unsigned long long)ext.e_pblk, ext.e_len,
                        (unsigned long long)blocks_count);
            continue;
        }

        {
            int reported_unmarked = 0;
            for (uint32_t j = 0; j < ext.e_len; j++) {
                uint64_t b = ext.e_pblk + j;
                if (fs->block_map && !reported_unmarked && !ext2fs_test_block_bitmap2(fs->block_map, b)) {
                    add_finding(rep, "error", "bitmap", "EXTENT_BLOCK_UNMARKED", ino, b,
                                "Inode %u references block %llu that is unmarked in block bitmap.",
                                ino, (unsigned long long)b);
                    reported_unmarked = 1;
                }
                if (!check_shared && tracker_claim(tracker, b)) {
                    rep->duplicate_blocks++;
                    if (*dup_findings_emitted < 32) {
                        add_finding(rep, "error", "extent", "EXTENT_DUPLICATE_BLOCK", ino, b,
                                    "Physical block %llu is claimed by multiple extents/files (detected on inode %u).",
                                    (unsigned long long)b, ino);
                        (*dup_findings_emitted)++;
                    }
                }
            }
        }
    }

    if (retval != EXT2_ET_EXTENT_NO_NEXT && retval != 0) {
        add_finding(rep, "error", "extent", "EXTENT_WALK_ERROR", ino, 0,
                    "Extent iteration aborted on inode %u (err=%ld).", ino, (long)retval);
    }
    ext2fs_extent_free(eh);
}

static void check_directory_inode(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode_large *ino_st,
                                  uint8_t *dirbuf, fsck_lite_report_t *rep) {
    uint64_t isize = EXT2_I_SIZE(ino_st);
    uint32_t blocksize = fs->blocksize;
    uint64_t nblks;
    int saw_dot = 0, saw_dotdot = 0;
    int has_index = (ino_st->i_flags & EXT2_INDEX_FL) != 0;

    rep->checked_dirs++;

    if (ino_st->i_flags & EXT4_INLINE_DATA_FL) {
        inline_dir_check_ctx_t ictx;
        memset(&ictx, 0, sizeof(ictx));
        ictx.rep = rep;
        ictx.dir_ino = ino;
        ictx.inodes_count = fs->super->s_inodes_count;
        errcode_t rv = ext2fs_dir_iterate2(fs, ino, 0, NULL, inline_dir_check_cb, &ictx);
        if (rv != 0) {
            add_finding(rep, "error", "directory", "DIR_INLINE_ITER_ERROR", ino, 0,
                        "Inline directory inode %u iteration failed (err=%ld).", ino, (long)rv);
        } else if (!ictx.saw_dot || !ictx.saw_dotdot) {
            add_finding(rep, "error", "directory", "DIR_MISSING_DOT_ENTRIES", ino, 0,
                        "Inline directory inode %u is missing '.' or '..' entry.", ino);
        }
        return;
    }

    nblks = (blocksize > 0) ? ((isize + blocksize - 1ULL) / blocksize) : 0;
    for (uint64_t lb = 0; lb < nblks; lb++) {
        blk64_t pblk = 0;
        errcode_t rv = ext2fs_bmap2(fs, ino, (struct ext2_inode *)ino_st, NULL, 0, lb, NULL, &pblk);
        if (rv != 0) {
            add_finding(rep, "error", "directory", "DIR_BMAP_ERROR", ino, 0,
                        "Directory inode %u bmap failed at lblock %llu (err=%ld).",
                        ino, (unsigned long long)lb, (long)rv);
            break;
        }
        if (pblk == 0) {
            add_finding(rep, "error", "directory", "DIR_HOLE_BLOCK", ino, 0,
                        "Directory inode %u contains unallocated hole at lblock %llu.",
                        ino, (unsigned long long)lb);
            continue;
        }
        if (pblk >= ext2fs_blocks_count(fs->super)) {
            add_finding(rep, "error", "directory", "DIR_BLOCK_OUT_OF_RANGE", ino, pblk,
                        "Directory inode %u references out-of-range block %llu.",
                        ino, (unsigned long long)pblk);
            continue;
        }
        if (io_channel_read_blk64(fs->io, pblk, 1, dirbuf) != 0) {
            add_finding(rep, "error", "directory", "DIR_BLOCK_READ_ERROR", ino, pblk,
                        "Failed to read directory inode %u block %llu.",
                        ino, (unsigned long long)pblk);
            continue;
        }

        if (ext2fs_has_feature_metadata_csum(fs->super) &&
            !ext2fs_dir_block_csum_verify(fs, ino, (struct ext2_dir_entry *)dirbuf)) {
            add_finding(rep, "error", "directory", "DIR_BLOCK_CSUM_INVALID", ino, pblk,
                        "Directory inode %u block %llu failed CRC32C checksum.",
                        ino, (unsigned long long)pblk);
        }

        {
            unsigned int off = 0;
            int is_dx_node = 0;
            if (has_index && lb > 0) {
                struct ext2_dir_entry_2 *first = (struct ext2_dir_entry_2 *)dirbuf;
                unsigned int first_rec = 0;
                if (first->inode == 0 &&
                    ext2fs_get_rec_len(fs, (struct ext2_dir_entry *)first, &first_rec) == 0 &&
                    first_rec == blocksize) {
                    is_dx_node = 1;
                }
            }
            if (is_dx_node) {
                continue;
            }

            while (off < blocksize) {
                struct ext2_dir_entry_2 *de;
                unsigned int rec_len = 0;
                if (off + 8 > blocksize) {
                    add_finding(rep, "error", "directory", "DIRENT_TRUNCATED_HEADER", ino, pblk,
                                "Directory inode %u block %llu has truncated dirent at off=%u.",
                                ino, (unsigned long long)pblk, off);
                    break;
                }
                de = (struct ext2_dir_entry_2 *)(dirbuf + off);
                if (ext2fs_get_rec_len(fs, (struct ext2_dir_entry *)de, &rec_len) != 0 ||
                    rec_len < 8 || (rec_len & 3U) != 0 || off + rec_len > blocksize) {
                    add_finding(rep, "error", "directory", "DIRENT_BAD_REC_LEN", ino, pblk,
                                "Directory inode %u block %llu off=%u has invalid rec_len=%u.",
                                ino, (unsigned long long)pblk, off, rec_len);
                    break;
                }
                if (de->inode == 0 && rec_len == sizeof(struct ext2_dir_entry_tail)) {
                    const struct ext2_dir_entry_tail *dt = (const struct ext2_dir_entry_tail *)de;
                    if (dt->det_reserved_zero1 == 0 &&
                        dt->det_reserved_name_len == EXT2_DIR_NAME_LEN_CSUM) {
                        off += rec_len;
                        continue;
                    }
                }
                if (de->inode != 0) {
                    rep->checked_dirents++;
                    if (de->inode > fs->super->s_inodes_count) {
                        add_finding(rep, "error", "directory", "DIRENT_INODE_OUT_OF_RANGE",
                                    ino, pblk,
                                    "Directory inode %u off=%u references out-of-range inode %u (max=%u).",
                                    ino, off, de->inode, fs->super->s_inodes_count);
                    }
                    if (8U + (unsigned int)de->name_len > rec_len) {
                        add_finding(rep, "error", "directory", "DIRENT_BAD_NAME_LEN", ino, pblk,
                                    "Directory inode %u off=%u has name_len=%u exceeding rec_len=%u.",
                                    ino, off, (unsigned)de->name_len, rec_len);
                    } else {
                        if (de->name_len == 1 && de->name[0] == '.') saw_dot = 1;
                        if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') saw_dotdot = 1;
                    }
                }
                off += rec_len;
            }
        }
    }

    if (nblks > 0 && (!saw_dot || !saw_dotdot)) {
        add_finding(rep, "error", "directory", "DIR_MISSING_DOT_ENTRIES", ino, 0,
                    "Directory inode %u is missing mandatory '.' or '..' entry.", ino);
    }
}

static void check_orphan_list(ext2_filsys fs, fsck_lite_report_t *rep) {
    uint32_t cur = fs->super->s_last_orphan;
    const size_t visited_cap = 4096U;
    uint32_t *visited = NULL;
    size_t visited_cnt = 0;

    if (cur == 0) return;
    visited = (uint32_t *)malloc(visited_cap * sizeof(uint32_t));
    if (!visited) return;

    while (cur != 0) {
        struct ext2_inode ino_st;
        if (cur > fs->super->s_inodes_count) {
            add_finding(rep, "error", "orphan", "ORPHAN_INODE_OUT_OF_RANGE", cur, 0,
                        "Orphan list references out-of-range inode %u (max=%u).",
                        cur, fs->super->s_inodes_count);
            break;
        }
        for (size_t i = 0; i < visited_cnt; i++) {
            if (visited[i] == cur) {
                add_finding(rep, "error", "orphan", "ORPHAN_LIST_CYCLE", cur, 0,
                            "Cycle detected in orphan inode list at inode %u.", cur);
                free(visited);
                return;
            }
        }
        if (visited_cnt >= visited_cap) {
            add_finding(rep, "warning", "orphan", "ORPHAN_LIST_TRUNCATED", cur, 0,
                        "Orphan inode list exceeded %u entries; stopping traversal.",
                        (unsigned)visited_cap);
            break;
        }
        visited[visited_cnt++] = cur;
        rep->orphan_count++;
        if (rep->orphan_count <= 16) {
            add_finding(rep, "warning", "orphan", "ORPHAN_INODE_PRESENT", cur, 0,
                        "Unrecovered orphan inode %u present in superblock s_last_orphan chain.", cur);
        }
        memset(&ino_st, 0, sizeof(ino_st));
        if (ext2fs_read_inode(fs, cur, &ino_st) != 0) {
            add_finding(rep, "error", "orphan", "ORPHAN_INODE_UNREADABLE", cur, 0,
                        "Failed to read orphan inode %u.", cur);
            break;
        }
        cur = ino_st.i_dtime;
    }
    free(visited);
}

static void check_journal_header(ext2_filsys fs, const ext4_probe_result_t *probe,
                                 fsck_lite_report_t *rep) {
    blk64_t jblk = 0;
    uint8_t *jbuf = NULL;

    if (!(probe->feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL)) {
        return;
    }
    if (ext4_probe_has_external_journal(probe)) {
        add_finding(rep, "warning", "journal", "JOURNAL_EXTERNAL", 0, 0,
                    "External journal device (s_journal_dev=0x%X) cannot be verified offline on Windows.",
                    probe->journal_dev);
        return;
    }
    if (probe->journal_inum == 0 || probe->journal_inum > fs->super->s_inodes_count) {
        add_finding(rep, "error", "journal", "JOURNAL_INVALID_INUM", probe->journal_inum, 0,
                    "HAS_JOURNAL set, but s_journal_inum=%u is invalid.", probe->journal_inum);
        return;
    }

    if (ext2fs_bmap2(fs, probe->journal_inum, NULL, NULL, 0, 0, NULL, &jblk) != 0 || jblk == 0) {
        add_finding(rep, "error", "journal", "JOURNAL_NO_HEADER_BLOCK", probe->journal_inum, 0,
                    "Journal inode %u has no mapped block 0.", probe->journal_inum);
        return;
    }

    jbuf = (uint8_t *)malloc(fs->blocksize);
    if (!jbuf) return;
    if (io_channel_read_blk64(fs->io, jblk, 1, jbuf) != 0 || fs->blocksize < 64) {
        add_finding(rep, "error", "journal", "JOURNAL_READ_ERROR", probe->journal_inum, jblk,
                    "Failed to read JBD2 superblock header block %llu.", (unsigned long long)jblk);
        free(jbuf);
        return;
    }

    {
        uint32_t magic = read_be32_u8(jbuf);
        uint32_t btype = read_be32_u8(jbuf + 4);
        if (magic != JBD2_MAGIC_NUMBER ||
            (btype != JBD2_SUPERBLOCK_V1 && btype != JBD2_SUPERBLOCK_V2)) {
            add_finding(rep, "error", "journal", "JOURNAL_BAD_HEADER", probe->journal_inum, jblk,
                        "Invalid JBD2 superblock header (magic=0x%08X, blocktype=%u).", magic, btype);
        } else {
            uint32_t s_start = read_be32_u8(jbuf + 0x1C);
            int32_t s_errno = (int32_t)read_be32_u8(jbuf + 0x20);
            if (s_errno != 0) {
                add_finding(rep, "error", "journal", "JOURNAL_HAS_ERRNO", probe->journal_inum, jblk,
                            "JBD2 superblock reports s_errno=%d.", s_errno);
            }
            if (s_start != 0) {
                add_finding(rep, "warning", "journal", "JOURNAL_NEEDS_RECOVERY", probe->journal_inum, jblk,
                            "Unreplayed JBD2 transactions (s_start=%u); fsck-lite inspects post-header metadata only.",
                            s_start);
            }
        }
    }
    free(jbuf);
}

int ext4_fsck_lite_run(const char *path_utf8, uint64_t offset_bytes, uint64_t size_bytes,
                       uint32_t sector_size, fsck_lite_report_t *out_report) {
    ext4_probe_result_t probe;
    win32_io_config_t cfg;
    ext2_filsys fs = NULL;
    errcode_t retval;
    blk64_t blocks_count;
    blk_claim_tracker_t tracker;
    uint8_t *dirbuf = NULL;
    uint32_t dup_findings = 0;

    if (!path_utf8 || !out_report) return -EINVAL;
    memset(out_report, 0, sizeof(*out_report));

    if (sector_size == 0) sector_size = 4096;
    if (ext4_probe_partition(path_utf8, offset_bytes, size_bytes, sector_size, &probe) != 0) {
        strcpy_s(out_report->fs_kind, sizeof(out_report->fs_kind), "unknown");
        strcpy_s(out_report->journal_state, sizeof(out_report->journal_state), "UNKNOWN");
        add_finding(out_report, "error", "superblock", "SB_INVALID_OR_UNREADABLE", 0, 0,
                    "Failed to locate or parse valid ext2/ext3/ext4 superblock at offset %llu.",
                    (unsigned long long)offset_bytes);
        out_report->healthy = 0;
        return 0;
    }

    strcpy_s(out_report->fs_kind, sizeof(out_report->fs_kind), ext4_fs_kind_name(probe.fs_kind));
    strcpy_s(out_report->volume_label, sizeof(out_report->volume_label), probe.volume_label);
    strcpy_s(out_report->uuid, sizeof(out_report->uuid), probe.uuid);
    strcpy_s(out_report->journal_state, sizeof(out_report->journal_state),
             ext4_journal_state_name(probe.journal_state));
    out_report->block_size = probe.block_size;
    out_report->inode_size = probe.inode_size;
    out_report->blocks_count = probe.blocks_count;

    if (probe.metadata_csum_enabled && !probe.sb_checksum_valid) {
        add_finding(out_report, "error", "superblock", "SB_CSUM_INVALID", 0, 0,
                    "Superblock CRC32C checksum mismatch (stored=0x%08X, computed=0x%08X).",
                    probe.sb_checksum_stored, probe.sb_checksum_computed);
    }
    if (probe.feature_incompat & EXT3_FEATURE_INCOMPAT_JOURNAL_DEV) {
        add_finding(out_report, "error", "superblock", "SB_JOURNAL_DEV", 0, 0,
                    "Target is an external JBD2 journal device rather than a mountable filesystem.");
        out_report->healthy = 0;
        return 0;
    }
    if (probe.unsupported_incompat != 0) {
        add_finding(out_report, "error", "superblock", "SB_UNSUPPORTED_INCOMPAT", 0, 0,
                    "Unsupported ext4 incompat feature mask 0x%08X.", probe.unsupported_incompat);
    }
    if (probe.state_flags & EXT2_ERROR_FS) {
        add_finding(out_report, "error", "superblock", "SB_ERROR_FS", 0, 0,
                    "Filesystem superblock has EXT2_ERROR_FS error flag set.");
    }
    if (size_bytes > 0 && probe.total_bytes > size_bytes) {
        add_finding(out_report, "error", "superblock", "SB_EXCEEDS_PARTITION", 0, 0,
                    "Filesystem total_bytes (%llu) exceeds partition size (%llu).",
                    (unsigned long long)probe.total_bytes, (unsigned long long)size_bytes);
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.partition_offset_bytes = offset_bytes;
    cfg.partition_size_bytes = size_bytes;
    cfg.sector_size = sector_size;
    cfg.read_only = 1;
    win32_io_set_next_config(&cfg);
    retval = ext2fs_open2(path_utf8, NULL, EXT2_FLAG_64BITS | EXT2_FLAG_IGNORE_CSUM_ERRORS,
                          0, 0, win32_io_manager, &fs);
    if (retval != 0 || !fs) {
        add_finding(out_report, "error", "superblock", "FS_OPEN_FAILED", 0, 0,
                    "libext2fs could not open volume read-only (err=%ld).", (long)retval);
        out_report->healthy = 0;
        return 0;
    }

    blocks_count = ext2fs_blocks_count(fs->super);
    out_report->inodes_count = fs->super->s_inodes_count;
    out_report->group_count = fs->group_desc_count;

    /* Pass 1: Block Group Descriptors */
    for (dgrp_t g = 0; g < fs->group_desc_count; g++) {
        blk64_t bb = ext2fs_block_bitmap_loc(fs, g);
        blk64_t ib = ext2fs_inode_bitmap_loc(fs, g);
        blk64_t it = ext2fs_inode_table_loc(fs, g);

        if (ext2fs_has_group_desc_csum(fs) && !ext2fs_group_desc_csum_verify(fs, g)) {
            add_finding(out_report, "error", "bgd", "BGD_CSUM_INVALID", 0, g,
                        "Block group descriptor %u failed checksum verification.", g);
        }
        if (bb == 0 || bb >= blocks_count || ib == 0 || ib >= blocks_count) {
            add_finding(out_report, "error", "bgd", "BGD_BITMAP_OUT_OF_RANGE", 0, g,
                        "Group %u bitmap out of range (bb=%llu, ib=%llu, max=%llu).",
                        g, (unsigned long long)bb, (unsigned long long)ib,
                        (unsigned long long)blocks_count);
        }
        if (it == 0 || it >= blocks_count ||
            it + fs->inode_blocks_per_group > blocks_count) {
            add_finding(out_report, "error", "bgd", "BGD_ITABLE_OUT_OF_RANGE", 0, it,
                        "Group %u inode table [%llu+%u) exceeds blocks_count=%llu.",
                        g, (unsigned long long)it, fs->inode_blocks_per_group,
                        (unsigned long long)blocks_count);
        }
        if (ext2fs_bg_free_blocks_count(fs, g) > fs->super->s_blocks_per_group ||
            ext2fs_bg_free_inodes_count(fs, g) > fs->super->s_inodes_per_group) {
            add_finding(out_report, "error", "bgd", "BGD_FREE_COUNT_INVALID", 0, g,
                        "Group %u free counts exceed per-group maximums.", g);
        }
    }

    /* Pass 2: Read bitmaps & verify per-group bitmap checksums when applicable */
    retval = ext2fs_read_bitmaps(fs);
    if (retval != 0) {
        add_finding(out_report, "error", "bitmap", "BITMAP_READ_FAILED", 0, 0,
                    "Failed to read block/inode allocation bitmaps (err=%ld).", (long)retval);
    } else if (ext2fs_has_feature_metadata_csum(fs->super)) {
        uint8_t *bm_buf = (uint8_t *)malloc(fs->blocksize);
        if (bm_buf) {
            int bb_sz = (int)((EXT2_CLUSTERS_PER_GROUP(fs->super) + 7U) / 8U);
            int ib_sz = (int)((EXT2_INODES_PER_GROUP(fs->super) + 7U) / 8U);
            for (dgrp_t g = 0; g < fs->group_desc_count; g++) {
                if (!ext2fs_bg_flags_test(fs, g, EXT2_BG_BLOCK_UNINIT)) {
                    blk64_t bb = ext2fs_block_bitmap_loc(fs, g);
                    if (bb > 0 && bb < blocks_count && bb_sz <= (int)fs->blocksize &&
                        io_channel_read_blk64(fs->io, bb, 1, bm_buf) == 0 &&
                        !ext2fs_block_bitmap_csum_verify(fs, g, (char *)bm_buf, bb_sz)) {
                        add_finding(out_report, "error", "bitmap", "BLOCK_BITMAP_CSUM_INVALID",
                                    0, bb, "Group %u block bitmap checksum mismatch.", g);
                    }
                }
                if (!ext2fs_bg_flags_test(fs, g, EXT2_BG_INODE_UNINIT)) {
                    blk64_t ib = ext2fs_inode_bitmap_loc(fs, g);
                    if (ib > 0 && ib < blocks_count && ib_sz <= (int)fs->blocksize &&
                        io_channel_read_blk64(fs->io, ib, 1, bm_buf) == 0 &&
                        !ext2fs_inode_bitmap_csum_verify(fs, g, (char *)bm_buf, ib_sz)) {
                        add_finding(out_report, "error", "bitmap", "INODE_BITMAP_CSUM_INVALID",
                                    0, ib, "Group %u inode bitmap checksum mismatch.", g);
                    }
                }
            }
            free(bm_buf);
        }
    }

    tracker_init(&tracker, blocks_count);
    dirbuf = (uint8_t *)malloc(fs->blocksize ? fs->blocksize : 4096);
    uint32_t inode_buf_sz = EXT2_INODE_SIZE(fs->super);
    if (inode_buf_sz < (uint32_t)sizeof(struct ext2_inode_large)) {
        inode_buf_sz = (uint32_t)sizeof(struct ext2_inode_large);
    }
    uint8_t *inode_buf = (uint8_t *)calloc(1, inode_buf_sz);

    /* Pass 3 & 4a: Inodes, Extent Trees, & Directory Entries */
    for (ext2_ino_t ino = 1; inode_buf && ino <= fs->super->s_inodes_count; ino++) {
        struct ext2_inode_large *ino_st = (struct ext2_inode_large *)inode_buf;

        if (fs->inode_map && !ext2fs_test_inode_bitmap2(fs->inode_map, ino)) {
            continue;
        }
        if (ino < EXT2_FIRST_INODE(fs->super) &&
            ino != EXT2_ROOT_INO &&
            ino != fs->super->s_journal_inum) {
            continue;
        }

        memset(inode_buf, 0, inode_buf_sz);
        retval = ext2fs_read_inode_full(fs, ino, (struct ext2_inode *)inode_buf, (int)inode_buf_sz);
        if (retval != 0) {
            add_finding(out_report, "error", "bitmap", "INODE_READ_ERROR", ino, 0,
                        "Failed to read allocated inode %u (err=%ld).", ino, (long)retval);
            continue;
        }
        out_report->checked_inodes++;

        if (ext2fs_has_feature_metadata_csum(fs->super) &&
            !ext2fs_inode_csum_verify(fs, ino, ino_st)) {
            add_finding(out_report, "error", "bitmap", "INODE_CSUM_INVALID", ino, 0,
                        "Inode %u failed CRC32C checksum verification.", ino);
        }

        if (ino_st->i_mode == 0 && ino != fs->super->s_journal_inum) {
            add_finding(out_report, "warning", "bitmap", "INODE_ZERO_MODE", ino, 0,
                        "Allocated inode %u has i_mode=0 (unlinked or stale bitmap bit).", ino);
            continue;
        }

        if ((ino_st->i_flags & EXT4_EXTENTS_FL) &&
            !(ino_st->i_flags & EXT4_INLINE_DATA_FL) &&
            !ext2fs_is_fast_symlink((struct ext2_inode *)ino_st)) {
            check_inode_extents(fs, ino, ino_st, &tracker, out_report, &dup_findings);
        }

        if (LINUX_S_ISDIR(ino_st->i_mode) && ino_st->i_dtime == 0 && dirbuf) {
            check_directory_inode(fs, ino, ino_st, dirbuf, out_report);
        }
    }

    if (tracker.saturated) {
        add_finding(out_report, "warning", "extent", "DUP_TRACKER_BOUNDED", 0, 0,
                    "Duplicate block tracker reached bounded hash capacity; very large extent coverage is partial.");
    }

    /* Pass 4b & 4c: Orphan list & JBD2 Journal header */
    check_orphan_list(fs, out_report);
    check_journal_header(fs, &probe, out_report);

    free(inode_buf);
    free(dirbuf);
    tracker_free(&tracker);
    ext2fs_free(fs);

    out_report->healthy = (out_report->error_count == 0) ? 1 : 0;
    return 0;
}

void ext4_fsck_lite_free_report(fsck_lite_report_t *report) {
    if (!report) return;
    free(report->findings);
    memset(report, 0, sizeof(*report));
}
