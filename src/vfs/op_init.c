#include "ext4_vfs.h"
#include "../io/win32_io.h"
#include "../disk/partition_table.h"
#include "../common/ext4_errno.h"
#include "../extent/extent_map.h"
#include "../overlay/overlay_vfs.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ext4_vfs_normalize_path(const char *in_path, char *out_path, size_t out_sz) {
    size_t i = 0, j = 0;
    if (!in_path || !out_path || out_sz < 2) return -EINVAL;

    out_path[j++] = '/';
    while (in_path[i] != '\0' && j + 1 < out_sz) {
        char ch = in_path[i++];
        if (ch == '\\') ch = '/';
        if (ch == '/' && out_path[j - 1] == '/') {
            continue;
        }
        out_path[j++] = ch;
    }
    while (j > 1 && out_path[j - 1] == '/') {
        j--;
    }
    out_path[j] = '\0';
    return 0;
}

int ext4_vfs_split_parent(const char *norm_path, char *out_parent, size_t parent_sz,
                          char *out_name, size_t name_sz) {
    const char *last_slash;
    size_t plen, nlen;

    if (!norm_path || !out_parent || !out_name || norm_path[0] != '/') return -EINVAL;
    if (strcmp(norm_path, "/") == 0) return -EINVAL;

    last_slash = strrchr(norm_path, '/');
    if (!last_slash) return -EINVAL;

    if (last_slash == norm_path) {
        if (parent_sz < 2) return -ENAMETOOLONG;
        strcpy_s(out_parent, parent_sz, "/");
    } else {
        plen = (size_t)(last_slash - norm_path);
        if (plen >= parent_sz) return -ENAMETOOLONG;
        memcpy(out_parent, norm_path, plen);
        out_parent[plen] = '\0';
    }

    nlen = strlen(last_slash + 1);
    if (nlen == 0 || nlen >= name_sz) return -ENAMETOOLONG;
    memcpy(out_name, last_slash + 1, nlen + 1);
    return 0;
}

struct ci_lookup_ctx {
    const char *target;
    ext2_ino_t  found_ino;
    int         found;
};

static int ci_lookup_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                        int offset, int blocksize, char *buf, void *priv_data) {
    struct ci_lookup_ctx *c = (struct ci_lookup_ctx *)priv_data;
    struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)dirent;
    char name_buf[EXT2_NAME_LEN + 1];
    (void)dir;
    (void)entry;
    (void)offset;
    (void)blocksize;
    (void)buf;

    if (de->inode == 0) return 0;
    if (de->name_len > EXT2_NAME_LEN) return 0;
    memcpy(name_buf, de->name, de->name_len);
    name_buf[de->name_len] = '\0';

    if (_stricmp(name_buf, c->target) == 0) {
        c->found_ino = de->inode;
        c->found = 1;
        return DIRENT_ABORT;
    }
    return 0;
}

int ext4_vfs_lookup_locked(ext4_vfs_ctx_t *ctx, const char *norm_path, ext2_ino_t *out_ino) {
    errcode_t retval;
    ext2_ino_t cur;
    const char *p;

    if (!ctx || !norm_path || !out_ino) return -EINVAL;
    if (strcmp(norm_path, "/") == 0) {
        *out_ino = EXT2_ROOT_INO;
        return 0;
    }

    retval = ext2fs_namei(ctx->fs, EXT2_ROOT_INO, EXT2_ROOT_INO, norm_path, &cur);
    if (retval == 0) {
        *out_ino = cur;
        return 0;
    }
    if (!ctx->case_insensitive) {
        return ext4_errcode_to_errno(retval);
    }

    /* Component-wise exact then case-insensitive fallback */
    cur = EXT2_ROOT_INO;
    p = norm_path + 1;
    while (*p) {
        const char *slash = strchr(p, '/');
        char comp[EXT2_NAME_LEN + 1];
        size_t clen = slash ? (size_t)(slash - p) : strlen(p);
        ext2_ino_t next_ino = 0;

        if (clen == 0) {
            p++;
            continue;
        }
        if (clen > EXT2_NAME_LEN) return -ENAMETOOLONG;
        memcpy(comp, p, clen);
        comp[clen] = '\0';

        retval = ext2fs_lookup(ctx->fs, cur, comp, (int)clen, NULL, &next_ino);
        if (retval != 0) {
            struct ci_lookup_ctx c;
            c.target = comp;
            c.found_ino = 0;
            c.found = 0;
            ext2fs_dir_iterate2(ctx->fs, cur, 0, NULL, ci_lookup_cb, &c);
            if (!c.found) {
                return -ENOENT;
            }
            next_ino = c.found_ino;
        }
        cur = next_ino;
        if (!slash) break;
        p = slash + 1;
    }

    *out_ino = cur;
    return 0;
}

int ext4_vfs_mount(const ext4_mount_config_t *cfg, ext4_vfs_ctx_t **out_ctx) {
    ext4_vfs_ctx_t *ctx = NULL;
    win32_io_config_t io_cfg;
    ext4_probe_result_t probe;
    uint64_t part_offset = 0;
    uint64_t part_size = 0;
    uint32_t sector_size = 0;
    int read_only;
    int flags;
    errcode_t retval;

    if (!cfg || !cfg->source_path || !out_ctx) return -EINVAL;
    *out_ctx = NULL;

    part_offset = cfg->partition_offset;
    part_size = cfg->partition_size;
    sector_size = cfg->sector_size;

    if (part_offset == 0 && part_size == 0) {
        if (cfg->partition_number > 0) {
            if (pt_resolve_partition(cfg->source_path, cfg->partition_number,
                                     &part_offset, &part_size, &sector_size) != 0) {
                return -ENOENT;
            }
        } else {
            pt_resolve_partition(cfg->source_path, 0, &part_offset, &part_size, &sector_size);
        }
    }

    if (sector_size == 0) sector_size = cfg->sector_size ? cfg->sector_size : 4096;

    memset(&probe, 0, sizeof(probe));
    if (ext4_probe_partition(cfg->source_path, part_offset, part_size, sector_size, &probe) != 0) {
        return -EINVAL;
    }

    int has_overlay = (cfg->overlay_upper_dir && cfg->overlay_upper_dir[0] != '\0');
    int lower_ro = has_overlay ? 1 : cfg->read_only;
    read_only = cfg->read_only;
    int dirty = (probe.journal_state == EXT4_JOURNAL_NEEDS_RECOVERY ||
                 probe.journal_state == EXT4_JOURNAL_HAS_ERRORS);
    int fell_back = 0;

    if (cfg->enable_direct_io && cfg->disable_direct_io) {
        ext4_log(EXT4_LOG_ERROR,
                 "Cannot specify both --direct-io and --no-direct-io.");
        return -EINVAL;
    }

    if (cfg->enable_direct_io && !lower_ro) {
        ext4_log(EXT4_LOG_ERROR,
                 "Direct extent I/O (--direct-io) is only supported in Read-Only (default) or --overlay mode.");
        return -EINVAL;
    }

    if (!lower_ro && dirty && !cfg->force_rw_on_dirty) {
        if (cfg->ro_fallback_on_dirty) {
            ext4_log(EXT4_LOG_WARN,
                     "Dirty journal / FS error detected (%s) on '%s'; falling back to READ-ONLY mount.",
                     ext4_journal_state_name(probe.journal_state), cfg->source_path);
            lower_ro = 1;
            read_only = 1;
            fell_back = 1;
        } else {
            ext4_log(EXT4_LOG_ERROR,
                     "Refusing RW mount on dirty ext4 volume (%s) without --force-dirty.",
                     ext4_journal_state_name(probe.journal_state));
            return -EROFS;
        }
    }

    ctx = (ext4_vfs_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return -ENOMEM;

    InitializeSRWLock(&ctx->rwlock);
    ctx->read_only = read_only;
    ctx->lower_read_only = lower_ro;
    ctx->dirty_detected = dirty;
    ctx->fell_back_to_ro = fell_back;
    ctx->case_insensitive = cfg->case_insensitive;
    /* Direct Extent I/O is enabled by default whenever Lower ext4 is Read-Only unless explicitly disabled */
    ctx->enable_direct_io = (lower_ro && !cfg->disable_direct_io) ? 1 : 0;
    ctx->has_file_info_timeout = cfg->has_file_info_timeout ? 1 : 0;
    ctx->file_info_timeout = cfg->file_info_timeout;
    ctx->journal_state = probe.journal_state;
    ctx->partition_offset = part_offset;
    ctx->partition_size = part_size;
    strcpy_s(ctx->source_path, sizeof(ctx->source_path), cfg->source_path);
    strcpy_s(ctx->volume_label, sizeof(ctx->volume_label), probe.volume_label);

    memset(&io_cfg, 0, sizeof(io_cfg));
    io_cfg.partition_offset_bytes = part_offset;
    io_cfg.partition_size_bytes = part_size;
    io_cfg.sector_size = sector_size;
    io_cfg.max_cache_blocks = cfg->max_cache_blocks;
    io_cfg.read_only = lower_ro;

    flags = EXT2_FLAG_64BITS;
    if (!lower_ro) {
        flags |= EXT2_FLAG_RW;
    }
    if (cfg->ignore_csum_errors) {
        flags |= EXT2_FLAG_IGNORE_CSUM_ERRORS;
    }
    if (cfg->force_rw_on_dirty) {
        flags |= EXT2_FLAG_FORCE;
    }

    win32_io_set_next_config(&io_cfg);
    retval = ext2fs_open2(cfg->source_path, NULL, flags, 0, 0, win32_io_manager, &ctx->fs);
    if (retval) {
        free(ctx);
        return ext4_errcode_to_errno(retval);
    }

    retval = ext2fs_read_bitmaps(ctx->fs);
    if (retval) {
        ext2fs_free(ctx->fs);
        free(ctx);
        return ext4_errcode_to_errno(retval);
    }

    if (ctx->enable_direct_io && ctx->lower_read_only) {
        ctx->extent_cache = ext4_extent_cache_create(4096);
    }
    if (has_overlay) {
        int orc = overlay_vfs_create(ctx, cfg->overlay_upper_dir, &ctx->overlay);
        if (orc != 0) {
            ext4_extent_cache_destroy(ctx->extent_cache);
            ext2fs_close_free(&ctx->fs);
            free(ctx);
            return orc;
        }
    }

    *out_ctx = ctx;
    return 0;
}

int ext4_vfs_unmount(ext4_vfs_ctx_t *ctx) {
    errcode_t retval = 0;
    if (!ctx) return 0;

    if (ctx->overlay) {
        overlay_vfs_destroy(ctx->overlay);
        ctx->overlay = NULL;
    }
    if (ctx->extent_cache) {
        ext4_extent_cache_destroy(ctx->extent_cache);
        ctx->extent_cache = NULL;
    }

    AcquireSRWLockExclusive(&ctx->rwlock);
    if (ctx->fs) {
        retval = ext2fs_close_free(&ctx->fs);
    }
    ReleaseSRWLockExclusive(&ctx->rwlock);
    free(ctx);
    return ext4_errcode_to_errno(retval);
}

int ext4_vfs_mkfs(const char *path_utf8, uint64_t size_bytes, const char *label) {
    wchar_t wpath[1024];
    HANDLE h;
    LARGE_INTEGER li;
    struct ext2_super_block param;
    win32_io_config_t io_cfg;
    ext2_filsys fs = NULL;
    errcode_t retval;
    unsigned int blocksize;
    unsigned int log_bsize;
    blk64_t blocks_count;
    ext2_ino_t r;

    if (!path_utf8 || size_bytes < (1024ULL * 1024ULL)) {
        return -EINVAL;
    }
    if (ext4_utf8_to_wide(path_utf8, wpath, sizeof(wpath) / sizeof(wpath[0])) != 0) {
        return -EINVAL;
    }

    blocksize = (size_bytes >= 4ULL * 1024ULL * 1024ULL) ? 4096U : 1024U;
    log_bsize = (blocksize == 4096U) ? 2U : 0U;
    size_bytes = (size_bytes / blocksize) * blocksize;
    blocks_count = size_bytes / blocksize;

    h = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return -EIO;
    }
    li.QuadPart = (LONGLONG)size_bytes;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN) || !SetEndOfFile(h)) {
        CloseHandle(h);
        return -EIO;
    }
    CloseHandle(h);

    memset(&param, 0, sizeof(param));
    param.s_log_block_size = log_bsize;
    param.s_inode_size = 256;
    param.s_feature_compat = EXT2_FEATURE_COMPAT_DIR_INDEX | EXT2_FEATURE_COMPAT_EXT_ATTR;
    param.s_feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE |
                               EXT4_FEATURE_INCOMPAT_EXTENTS |
                               EXT4_FEATURE_INCOMPAT_64BIT;
    param.s_feature_ro_compat = EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER |
                                EXT2_FEATURE_RO_COMPAT_LARGE_FILE |
                                EXT4_FEATURE_RO_COMPAT_HUGE_FILE |
                                EXT4_FEATURE_RO_COMPAT_DIR_NLINK |
                                EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE |
                                EXT4_FEATURE_RO_COMPAT_METADATA_CSUM;
    ext2fs_blocks_count_set(&param, blocks_count);
    {
        static volatile LONG64 s_uuid_seq = 0;
        LARGE_INTEGER qpc;
        ULONGLONG ticks = GetTickCount64();
        LONG64 seq = InterlockedIncrement64(&s_uuid_seq);
        uint64_t a, b;
        QueryPerformanceCounter(&qpc);
        a = (uint64_t)qpc.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^ (uint64_t)seq;
        b = (uint64_t)ticks ^ ((uint64_t)seq * 0x9E3779B97F4A7C15ULL);
        a = (a ^ (a >> 30)) * 0xBF58476D1CE4E5B9ULL;
        a = (a ^ (a >> 27)) * 0x94D049BB133111EBULL;
        a ^= (a >> 31);
        b = (b ^ (b >> 30)) * 0xBF58476D1CE4E5B9ULL;
        b = (b ^ (b >> 27)) * 0x94D049BB133111EBULL;
        b ^= (b >> 31);
        memcpy(param.s_uuid, &a, 8);
        memcpy(param.s_uuid + 8, &b, 8);
        param.s_uuid[6] = (param.s_uuid[6] & 0x0F) | 0x40; /* UUID v4 */
        param.s_uuid[8] = (param.s_uuid[8] & 0x3F) | 0x80; /* RFC 4122 variant */
    }

    if (label && *label) {
        size_t llen = strlen(label);
        if (llen > 16) {
            llen = 16;
            while (llen > 0 && (((unsigned char)label[llen]) & 0xC0) == 0x80) {
                llen--;
            }
        }
        memcpy(param.s_volume_name, label, llen);
    }

    memset(&io_cfg, 0, sizeof(io_cfg));
    io_cfg.partition_size_bytes = size_bytes;
    io_cfg.sector_size = 4096;
    win32_io_set_next_config(&io_cfg);

    retval = ext2fs_initialize(path_utf8, EXT2_FLAG_64BITS | EXT2_FLAG_RW, &param, win32_io_manager, &fs);
    if (retval) {
        return ext4_errcode_to_errno(retval);
    }

    retval = ext2fs_allocate_tables(fs);
    if (retval) {
        ext2fs_free(fs);
        return ext4_errcode_to_errno(retval);
    }

    /* Mark reserved system inodes 1..s_first_ino-1 except ROOT_INO (handled by mkdir below) */
    for (r = 1; r < fs->super->s_first_ino; r++) {
        if (r == EXT2_ROOT_INO) continue;
        ext2fs_inode_alloc_stats2(fs, r, 1, 0);
    }

    /* Create root directory (inode 2) */
    retval = ext2fs_mkdir(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, NULL);
    if (retval) {
        ext2fs_free(fs);
        return ext4_errcode_to_errno(retval);
    }

    /* Create lost+found directory in root */
    retval = ext2fs_mkdir(fs, EXT2_ROOT_INO, 0, "lost+found");
    if (retval) {
        ext2fs_free(fs);
        return ext4_errcode_to_errno(retval);
    }

    retval = ext2fs_close_free(&fs);
    return ext4_errcode_to_errno(retval);
}
