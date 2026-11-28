#include "superblock_probe.h"
#include "../io/win32_io.h"
#include <ext2fs/ext2fs.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void format_uuid(const uint8_t u[16], char out[37]) {
    int all_zero = 1;
    int i;
    for (i = 0; i < 16; i++) {
        if (u[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    if (all_zero) {
        out[0] = '\0';
        return;
    }
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
             u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

int ext4_probe_superblock_buffer(const uint8_t *sb_1024, ext4_probe_result_t *out) {
    struct ext2_super_block sb_val;
    struct ext2_super_block *sb = &sb_val;
    if (!sb_1024 || !out) return -1;
    memset(out, 0, sizeof(*out));
    memcpy(&sb_val, sb_1024, sizeof(sb_val));

    if (sb->s_magic != EXT2_SUPER_MAGIC) {
        return -1;
    }
    if (sb->s_log_block_size > (EXT2_MAX_BLOCK_LOG_SIZE - EXT2_MIN_BLOCK_LOG_SIZE)) {
        return -1;
    }

    out->block_size = EXT2_BLOCK_SIZE(sb);
    out->inode_size = EXT2_INODE_SIZE(sb);
    out->blocks_count = ext2fs_blocks_count(sb);
    out->total_bytes = out->blocks_count * out->block_size;
    out->free_bytes = ext2fs_free_blocks_count(sb) * out->block_size;
    out->feature_compat = sb->s_feature_compat;
    out->feature_incompat = sb->s_feature_incompat;
    out->feature_ro_compat = sb->s_feature_ro_compat;
    out->journal_inum = sb->s_journal_inum;
    out->journal_dev = sb->s_journal_dev;
    out->unsupported_incompat = sb->s_feature_incompat & ~(uint32_t)EXT2_LIB_FEATURE_INCOMPAT_SUPP;
    out->unsupported_ro_compat = sb->s_feature_ro_compat & ~(uint32_t)EXT2_LIB_FEATURE_RO_COMPAT_SUPP;
    out->state_flags = sb->s_state;
    out->last_mount_time = (uint64_t)sb->s_mtime | ((uint64_t)sb->s_mtime_hi << 32);
    out->last_fsck_time = (uint64_t)sb->s_lastcheck | ((uint64_t)sb->s_lastcheck_hi << 32);
    out->fsck_error_count = sb->s_error_count;
    out->metadata_csum_enabled = (sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) ? 1 : 0;
    out->sb_checksum_stored = sb->s_checksum;
    if (out->metadata_csum_enabled) {
        out->sb_checksum_computed = (uint32_t)ext2fs_crc32c_le(
            ~0U, sb_1024, offsetof(struct ext2_super_block, s_checksum));
        out->sb_checksum_valid =
            (sb->s_checksum_type == EXT2_CRC32C_CHKSUM &&
             out->sb_checksum_stored == out->sb_checksum_computed) ? 1 : 0;
    } else {
        out->sb_checksum_computed = 0;
        out->sb_checksum_valid = 1;
    }

    if ((sb->s_feature_incompat & (EXT3_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT |
                                   EXT4_FEATURE_INCOMPAT_FLEX_BG | EXT4_FEATURE_INCOMPAT_EA_INODE |
                                   EXT4_FEATURE_INCOMPAT_CSUM_SEED | EXT4_FEATURE_INCOMPAT_LARGEDIR |
                                   EXT4_FEATURE_INCOMPAT_INLINE_DATA | EXT4_FEATURE_INCOMPAT_ENCRYPT |
                                   EXT4_FEATURE_INCOMPAT_CASEFOLD | EXT4_FEATURE_INCOMPAT_MMP)) != 0 ||
        (sb->s_feature_ro_compat & ~(EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT2_FEATURE_RO_COMPAT_LARGE_FILE | 0x0004U /* legacy BTREE_DIR */)) != 0) {
        out->fs_kind = EXT4_FS_EXT4;
    } else if (sb->s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) {
        out->fs_kind = EXT4_FS_EXT3;
    } else {
        out->fs_kind = EXT4_FS_EXT2;
    }

    memcpy(out->volume_label, sb->s_volume_name, 16);
    out->volume_label[16] = '\0';
    format_uuid(sb->s_uuid, out->uuid);

    if (sb->s_state & EXT2_ERROR_FS) {
        out->journal_state = EXT4_JOURNAL_HAS_ERRORS;
    } else if (sb->s_feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER) {
        out->journal_state = EXT4_JOURNAL_NEEDS_RECOVERY;
    } else if (sb->s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) {
        out->journal_state = EXT4_JOURNAL_CLEAN;
    } else {
        out->journal_state = EXT4_JOURNAL_NONE;
    }

    return 0;
}

int ext4_probe_partition(const char *path_utf8, uint64_t offset_bytes, uint64_t size_bytes,
                         uint32_t sector_size, ext4_probe_result_t *out) {
    win32_io_config_t cfg;
    io_channel io = NULL;
    uint8_t sb_buf[1024];
    errcode_t retval;

    if (!path_utf8 || !out) return -1;
    memset(out, 0, sizeof(*out));
    memset(&cfg, 0, sizeof(cfg));
    cfg.partition_offset_bytes = offset_bytes;
    cfg.partition_size_bytes = size_bytes;
    cfg.sector_size = sector_size;
    cfg.read_only = 1;
    cfg.disable_cache = 1;

    win32_io_set_next_config(&cfg);
    retval = io_channel_open(win32_io_manager, path_utf8, 0, &io);
    if (retval) {
        win32_io_clear_next_config();
        return -1;
    }

    io_channel_set_blksize(io, 1024);
    retval = io_channel_read_blk64(io, 1, 1, sb_buf);
    io_channel_close(io);
    if (retval) {
        return -1;
    }

    if (ext4_probe_superblock_buffer(sb_buf, out) != 0) {
        return -1;
    }

    /* Inspect internal JBD2 journal superblock if HAS_JOURNAL and journal_inum != 0 */
    if ((out->feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) && out->journal_inum != 0) {
        ext2_filsys fs = NULL;
        win32_io_set_next_config(&cfg);
        if (ext2fs_open2(path_utf8, NULL, EXT2_FLAG_IGNORE_CSUM_ERRORS, 0, 0, win32_io_manager, &fs) == 0) {
            blk64_t jblk = 0;
            if (ext2fs_bmap2(fs, out->journal_inum, NULL, NULL, 0, 0, NULL, &jblk) == 0 && jblk != 0) {
                uint8_t *jbuf = (uint8_t *)malloc(fs->blocksize);
                if (jbuf) {
                    if (io_channel_read_blk64(fs->io, jblk, 1, jbuf) == 0 && fs->blocksize >= 64) {
                        uint32_t magic = read_be32(jbuf);
                        uint32_t btype = read_be32(jbuf + 4);
                        if (magic == JBD2_MAGIC_NUMBER && (btype == JBD2_SUPERBLOCK_V1 || btype == JBD2_SUPERBLOCK_V2)) {
                            uint32_t s_start = read_be32(jbuf + 0x1C);
                            int32_t s_errno = (int32_t)read_be32(jbuf + 0x20);
                            out->journal_start = s_start;
                            out->journal_errno = s_errno;
                            if (s_errno != 0) {
                                out->journal_state = EXT4_JOURNAL_HAS_ERRORS;
                            } else if (s_start != 0 && out->journal_state != EXT4_JOURNAL_HAS_ERRORS) {
                                out->journal_state = EXT4_JOURNAL_NEEDS_RECOVERY;
                            }
                        }
                    }
                    free(jbuf);
                }
            }
            ext2fs_free(fs);
        }
    }

    return 0;
}

const char *ext4_fs_kind_name(ext4_fs_kind_t kind) {
    switch (kind) {
    case EXT4_FS_EXT2: return "ext2";
    case EXT4_FS_EXT3: return "ext3";
    case EXT4_FS_EXT4: return "ext4";
    default:           return "unknown";
    }
}

const char *ext4_journal_state_name(ext4_journal_state_t state) {
    switch (state) {
    case EXT4_JOURNAL_NONE:           return "NONE";
    case EXT4_JOURNAL_CLEAN:          return "CLEAN";
    case EXT4_JOURNAL_NEEDS_RECOVERY: return "NEEDS_RECOVERY";
    case EXT4_JOURNAL_HAS_ERRORS:     return "HAS_ERRORS";
    default:                          return "UNKNOWN";
    }
}

typedef struct ext4_feature_bit_desc {
    uint32_t    bit;
    const char *name;
} ext4_feature_bit_desc_t;

static size_t collect_feature_names(uint32_t mask, const ext4_feature_bit_desc_t *table,
                                    size_t table_len, const char **out_names, size_t max_names) {
    size_t n = 0;
    if (!out_names || max_names == 0) return 0;
    for (size_t i = 0; i < table_len && n < max_names; i++) {
        if (mask & table[i].bit) {
            out_names[n++] = table[i].name;
        }
    }
    return n;
}

size_t ext4_feature_compat_names(uint32_t mask, const char **out_names, size_t max_names) {
    static const ext4_feature_bit_desc_t kCompat[] = {
        { EXT2_FEATURE_COMPAT_DIR_PREALLOC,   "dir_prealloc" },
        { EXT2_FEATURE_COMPAT_IMAGIC_INODES,  "imagic_inodes" },
        { EXT3_FEATURE_COMPAT_HAS_JOURNAL,    "has_journal" },
        { EXT2_FEATURE_COMPAT_EXT_ATTR,       "ext_attr" },
        { EXT2_FEATURE_COMPAT_RESIZE_INODE,   "resize_inode" },
        { EXT2_FEATURE_COMPAT_DIR_INDEX,      "dir_index" },
        { EXT2_FEATURE_COMPAT_LAZY_BG,        "lazy_bg" },
        { EXT2_FEATURE_COMPAT_EXCLUDE_BITMAP, "exclude_bitmap" },
        { EXT4_FEATURE_COMPAT_SPARSE_SUPER2,  "sparse_super2" },
        { EXT4_FEATURE_COMPAT_FAST_COMMIT,    "fast_commit" },
        { EXT4_FEATURE_COMPAT_STABLE_INODES,  "stable_inodes" },
        { EXT4_FEATURE_COMPAT_ORPHAN_FILE,    "orphan_file" },
    };
    return collect_feature_names(mask, kCompat, sizeof(kCompat) / sizeof(kCompat[0]), out_names, max_names);
}

size_t ext4_feature_incompat_names(uint32_t mask, const char **out_names, size_t max_names) {
    static const ext4_feature_bit_desc_t kIncompat[] = {
        { EXT2_FEATURE_INCOMPAT_COMPRESSION, "compression" },
        { EXT2_FEATURE_INCOMPAT_FILETYPE,    "filetype" },
        { EXT3_FEATURE_INCOMPAT_RECOVER,     "recover" },
        { EXT3_FEATURE_INCOMPAT_JOURNAL_DEV, "journal_dev" },
        { EXT2_FEATURE_INCOMPAT_META_BG,     "meta_bg" },
        { EXT3_FEATURE_INCOMPAT_EXTENTS,     "extents" },
        { EXT4_FEATURE_INCOMPAT_64BIT,       "64bit" },
        { EXT4_FEATURE_INCOMPAT_MMP,         "mmp" },
        { EXT4_FEATURE_INCOMPAT_FLEX_BG,     "flex_bg" },
        { EXT4_FEATURE_INCOMPAT_EA_INODE,    "ea_inode" },
        { EXT4_FEATURE_INCOMPAT_DIRDATA,     "dirdata" },
        { EXT4_FEATURE_INCOMPAT_CSUM_SEED,   "csum_seed" },
        { EXT4_FEATURE_INCOMPAT_LARGEDIR,    "largedir" },
        { EXT4_FEATURE_INCOMPAT_INLINE_DATA, "inline_data" },
        { EXT4_FEATURE_INCOMPAT_ENCRYPT,     "encrypt" },
        { EXT4_FEATURE_INCOMPAT_CASEFOLD,    "casefold" },
    };
    return collect_feature_names(mask, kIncompat, sizeof(kIncompat) / sizeof(kIncompat[0]), out_names, max_names);
}

size_t ext4_feature_ro_compat_names(uint32_t mask, const char **out_names, size_t max_names) {
    static const ext4_feature_bit_desc_t kRoCompat[] = {
        { EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER,   "sparse_super" },
        { EXT2_FEATURE_RO_COMPAT_LARGE_FILE,     "large_file" },
        { 0x0004U,                               "btree_dir" },
        { EXT4_FEATURE_RO_COMPAT_HUGE_FILE,      "huge_file" },
        { EXT4_FEATURE_RO_COMPAT_GDT_CSUM,       "gdt_csum" },
        { EXT4_FEATURE_RO_COMPAT_DIR_NLINK,      "dir_nlink" },
        { EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE,    "extra_isize" },
        { EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT,   "has_snapshot" },
        { EXT4_FEATURE_RO_COMPAT_QUOTA,          "quota" },
        { EXT4_FEATURE_RO_COMPAT_BIGALLOC,       "bigalloc" },
        { EXT4_FEATURE_RO_COMPAT_METADATA_CSUM,  "metadata_csum" },
        { EXT4_FEATURE_RO_COMPAT_REPLICA,        "replica" },
        { EXT4_FEATURE_RO_COMPAT_READONLY,       "readonly" },
        { EXT4_FEATURE_RO_COMPAT_PROJECT,        "project" },
        { EXT4_FEATURE_RO_COMPAT_SHARED_BLOCKS,  "shared_blocks" },
        { EXT4_FEATURE_RO_COMPAT_VERITY,         "verity" },
        { EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT, "orphan_present" },
    };
    return collect_feature_names(mask, kRoCompat, sizeof(kRoCompat) / sizeof(kRoCompat[0]), out_names, max_names);
}

int ext4_probe_has_external_journal(const ext4_probe_result_t *probe) {
    if (!probe) return 0;
    return ((probe->feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) != 0 &&
            probe->journal_inum == 0 &&
            probe->journal_dev != 0) ? 1 : 0;
}

void ext4_evaluate_mount_advice(const ext4_probe_result_t *probe, ext4_mount_advice_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!probe || probe->fs_kind == EXT4_FS_UNKNOWN) {
        strcpy_s(out->recommended_mode, sizeof(out->recommended_mode), "none");
        strcpy_s(out->ro_reason, sizeof(out->ro_reason), "No ext2/ext3/ext4 filesystem detected.");
        strcpy_s(out->overlay_reason, sizeof(out->overlay_reason), "No ext2/ext3/ext4 filesystem detected.");
        strcpy_s(out->rw_reason, sizeof(out->rw_reason), "No ext2/ext3/ext4 filesystem detected.");
        return;
    }

    if (probe->feature_incompat & EXT3_FEATURE_INCOMPAT_JOURNAL_DEV) {
        strcpy_s(out->recommended_mode, sizeof(out->recommended_mode), "none");
        strcpy_s(out->ro_reason, sizeof(out->ro_reason), "External JBD2 journal device cannot be mounted as a filesystem.");
        strcpy_s(out->overlay_reason, sizeof(out->overlay_reason), "External JBD2 journal device cannot be mounted as a filesystem.");
        strcpy_s(out->rw_reason, sizeof(out->rw_reason), "External JBD2 journal device cannot be mounted as a filesystem.");
        return;
    }

    if (probe->unsupported_incompat != 0) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "Unsupported incompat feature bits 0x%08X; libext2fs cannot safely mount this volume.",
                 probe->unsupported_incompat);
        strcpy_s(out->recommended_mode, sizeof(out->recommended_mode), "none");
        strcpy_s(out->ro_reason, sizeof(out->ro_reason), msg);
        strcpy_s(out->overlay_reason, sizeof(out->overlay_reason), msg);
        strcpy_s(out->rw_reason, sizeof(out->rw_reason), msg);
        return;
    }

    if (probe->metadata_csum_enabled && !probe->sb_checksum_valid) {
        strcpy_s(out->recommended_mode, sizeof(out->recommended_mode), "none");
        strcpy_s(out->ro_reason, sizeof(out->ro_reason),
                 "Superblock CRC32C checksum mismatch; run fsck-lite / Linux e2fsck or --ignore-csum-errors (RO only).");
        strcpy_s(out->overlay_reason, sizeof(out->overlay_reason),
                 "Superblock CRC32C checksum mismatch; run fsck-lite / Linux e2fsck or --ignore-csum-errors (Overlay).");
        strcpy_s(out->rw_reason, sizeof(out->rw_reason),
                 "Superblock CRC32C checksum mismatch detected; mounting RW is unsafe.");
        return;
    }

    out->read_only_allowed = 1;
    out->overlay_allowed = 1;
    strcpy_s(out->recommended_mode, sizeof(out->recommended_mode), "read_only");

    if (probe->feature_incompat & EXT4_FEATURE_INCOMPAT_ENCRYPT) {
        strcpy_s(out->ro_reason, sizeof(out->ro_reason),
                 "Allowed Read-Only, but fscrypt entries appear unreadable/encrypted without Linux kernel keys.");
        strcpy_s(out->overlay_reason, sizeof(out->overlay_reason),
                 "Lower fscrypt entries appear unreadable; only NTFS upperdir files can be written.");
    } else if (ext4_probe_has_external_journal(probe)) {
        strcpy_s(out->ro_reason, sizeof(out->ro_reason),
                 "Allowed Read-Only, but external journal (s_journal_dev) cannot be inspected/replayed on Windows.");
        strcpy_s(out->overlay_reason, sizeof(out->overlay_reason),
                 "Lower keeps ext4 RO; external journal (s_journal_dev) is ignored while changes go to upperdir.");
    } else if (probe->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_VERITY) {
        strcpy_s(out->ro_reason, sizeof(out->ro_reason),
                 "Allowed Read-Only with Direct Extent I/O (fs-verity Merkle verification is not enforced).");
        strcpy_s(out->overlay_reason, sizeof(out->overlay_reason),
                 "Keeps lower ext4 strictly RO (fs-verity protected) and writes changes to an NTFS upperdir.");
    } else {
        strcpy_s(out->ro_reason, sizeof(out->ro_reason),
                 "Safe non-destructive Read-Only access with Direct Extent I/O.");
        strcpy_s(out->overlay_reason, sizeof(out->overlay_reason),
                 "Keeps lower ext4 strictly Read-Only and writes changes to an NTFS upperdir.");
    }

    if (probe->journal_state == EXT4_JOURNAL_HAS_ERRORS) {
        out->read_write_allowed = 0;
        strcpy_s(out->rw_reason, sizeof(out->rw_reason),
                 "Filesystem error flag (EXT2_ERROR_FS) or journal errno is set; run fsck-lite / Linux e2fsck before Read-Write.");
    } else if (probe->journal_state == EXT4_JOURNAL_NEEDS_RECOVERY) {
        out->read_write_allowed = 0;
        strcpy_s(out->rw_reason, sizeof(out->rw_reason),
                 "Unreplayed JBD2 journal (NEEDS_RECOVERY); replay journal in Linux or use Read-Only / Overlay.");
    } else if (ext4_probe_has_external_journal(probe)) {
        out->read_write_allowed = 0;
        strcpy_s(out->rw_reason, sizeof(out->rw_reason),
                 "External journal device (s_journal_dev) is set; Read-Write is blocked for safety.");
    } else if (probe->feature_incompat & EXT4_FEATURE_INCOMPAT_ENCRYPT) {
        out->read_write_allowed = 0;
        strcpy_s(out->rw_reason, sizeof(out->rw_reason),
                 "fscrypt encryption (encrypt) is enabled; direct Read-Write is blocked for safety.");
    } else if (probe->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_VERITY) {
        out->read_write_allowed = 0;
        strcpy_s(out->rw_reason, sizeof(out->rw_reason),
                 "fs-verity (verity) is enabled; direct Read-Write is blocked to avoid Merkle tree mismatch.");
    } else if (probe->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_READONLY) {
        out->read_write_allowed = 0;
        strcpy_s(out->rw_reason, sizeof(out->rw_reason),
                 "Volume has the RO_COMPAT_READONLY flag set; use Read-Only or Overlay.");
    } else if (probe->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_SHARED_BLOCKS) {
        out->read_write_allowed = 0;
        strcpy_s(out->rw_reason, sizeof(out->rw_reason),
                 "Shared blocks feature is enabled; direct Read-Write is blocked.");
    } else if (probe->unsupported_ro_compat != 0) {
        out->read_write_allowed = 0;
        snprintf(out->rw_reason, sizeof(out->rw_reason),
                 "Unsupported ro_compat feature bits 0x%08X; direct Read-Write is blocked.",
                 probe->unsupported_ro_compat);
    } else {
        out->read_write_allowed = 1;
        strcpy_s(out->rw_reason, sizeof(out->rw_reason),
                 "Journal is clean; direct Read-Write (--rw) is available (non-journaled updates).");
    }
}
