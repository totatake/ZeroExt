#include "superblock_probe.h"
#include "../io/win32_io.h"
#include <ext2fs/ext2fs.h>
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
    out->state_flags = sb->s_state;

    if ((sb->s_feature_incompat & (EXT3_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_FLEX_BG)) != 0 ||
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
