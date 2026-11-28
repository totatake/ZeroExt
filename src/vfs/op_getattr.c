#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include "../overlay/overlay_vfs.h"
#include "../trace/perfetto_trace.h"
#include <errno.h>
#include <string.h>

#define UNIX_TO_WIN_EPOCH_TICKS 116444736000000000ULL

uint64_t ext4_now_filetime(void) {
    FILETIME ft;
    ULARGE_INTEGER ui;
    GetSystemTimeAsFileTime(&ft);
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return ui.QuadPart;
}

static uint64_t ext4_time_to_filetime(__u32 base_sec, __u32 extra, int has_extra) {
    int64_t sec = (int32_t)base_sec;
    uint32_t nsec = 0;
    if (has_extra) {
        sec += ((int64_t)(extra & EXT4_EPOCH_MASK)) << 32;
        nsec = (extra >> EXT4_EPOCH_BITS);
        if (nsec >= 1000000000U) nsec = 0;
    }
    int64_t unix_100ns = sec * 10000000LL + (int64_t)(nsec / 100U);
    int64_t win_100ns = (int64_t)UNIX_TO_WIN_EPOCH_TICKS + unix_100ns;
    return (win_100ns > 0) ? (uint64_t)win_100ns : UNIX_TO_WIN_EPOCH_TICKS;
}

static void filetime_to_ext4_time(uint64_t ft, __u32 *out_sec, __u32 *out_extra) {
    if (ft < UNIX_TO_WIN_EPOCH_TICKS) {
        *out_sec = 0;
        if (out_extra) *out_extra = 0;
        return;
    }
    uint64_t unix_100ns = ft - UNIX_TO_WIN_EPOCH_TICKS;
    int64_t sec = (int64_t)(unix_100ns / 10000000ULL);
    uint32_t nsec = (uint32_t)((unix_100ns % 10000000ULL) * 100ULL);
    __u32 epoch = (__u32)((sec >> 32) & EXT4_EPOCH_MASK);
    *out_sec = (__u32)(sec & 0xFFFFFFFFULL);
    if (out_extra) {
        *out_extra = (nsec << EXT4_EPOCH_BITS) | epoch;
    }
}

void ext4_inode_touch_times(struct ext2_inode_large *ino_st, int touch_atime,
                            int touch_mtime, int touch_ctime) {
    uint64_t now = ext4_now_filetime();
    __u32 sec = 0, extra = 0;
    filetime_to_ext4_time(now, &sec, &extra);
    if (touch_atime) {
        ino_st->i_atime = sec;
        ino_st->i_atime_extra = extra;
    }
    if (touch_mtime) {
        ino_st->i_mtime = sec;
        ino_st->i_mtime_extra = extra;
    }
    if (touch_ctime) {
        ino_st->i_ctime = sec;
        ino_st->i_ctime_extra = extra;
    }
}

void ext4_inode_set_owner(struct ext2_inode_large *ino_st, uint32_t uid, uint32_t gid) {
    if (!ino_st) return;
    ino_st->i_uid = (uint16_t)(uid & 0xFFFFU);
    ino_st->osd2.linux2.l_i_uid_high = (uint16_t)((uid >> 16) & 0xFFFFU);
    ino_st->i_gid = (uint16_t)(gid & 0xFFFFU);
    ino_st->osd2.linux2.l_i_gid_high = (uint16_t)((gid >> 16) & 0xFFFFU);
}

void ext4_vfs_resolve_new_owner(const ext4_vfs_ctx_t *ctx,
                                const struct ext2_inode_large *parent_st,
                                uint32_t *out_uid, uint32_t *out_gid) {
    uint32_t uid = ctx ? ctx->create_uid : 0U;
    uint32_t gid = ctx ? ctx->create_gid : 0U;

    if (ctx && parent_st) {
        uint32_t p_uid = parent_st->i_uid | ((uint32_t)parent_st->osd2.linux2.l_i_uid_high << 16);
        uint32_t p_gid = parent_st->i_gid | ((uint32_t)parent_st->osd2.linux2.l_i_gid_high << 16);
        if (ctx->inherit_owner) {
            uid = p_uid;
            gid = p_gid;
        } else if ((parent_st->i_mode & LINUX_S_ISGID) && !ctx->has_create_gid) {
            gid = p_gid;
        }
    }

    if (out_uid) *out_uid = uid;
    if (out_gid) *out_gid = gid;
}

void ext4_fill_stat_from_inode(ext4_vfs_ctx_t *ctx, ext2_ino_t ino,
                               const struct ext2_inode_large *ino_st,
                               const char *leaf_name, ext4_stat_t *out_st) {
    int has_extra = (EXT2_INODE_SIZE(ctx->fs->super) > EXT2_GOOD_OLD_INODE_SIZE &&
                     ino_st->i_extra_isize >= 24);
    uint64_t blk_sz = ctx->fs->blocksize;
    uint32_t attrs = 0;

    memset(out_st, 0, sizeof(*out_st));
    out_st->ino = ino;
    out_st->mode = ino_st->i_mode;
    out_st->is_dir = LINUX_S_ISDIR(ino_st->i_mode);
    out_st->is_symlink = LINUX_S_ISLNK(ino_st->i_mode);
    out_st->hard_links = ino_st->i_links_count;
    out_st->uid = ino_st->i_uid | ((uint32_t)ino_st->osd2.linux2.l_i_uid_high << 16);
    out_st->gid = ino_st->i_gid | ((uint32_t)ino_st->osd2.linux2.l_i_gid_high << 16);

    if (out_st->is_dir) {
        out_st->size = 0;
        out_st->alloc_size = 0;
        attrs |= FILE_ATTRIBUTE_DIRECTORY;
    } else {
        out_st->size = EXT2_I_SIZE(ino_st);
        out_st->alloc_size = ((out_st->size + blk_sz - 1) / blk_sz) * blk_sz;
        attrs |= FILE_ATTRIBUTE_ARCHIVE;
    }

    if (ctx->read_only || !(ino_st->i_mode & 0222)) {
        attrs |= FILE_ATTRIBUTE_READONLY;
    }
    if (leaf_name && leaf_name[0] == '.' &&
        strcmp(leaf_name, ".") != 0 && strcmp(leaf_name, "..") != 0) {
        attrs |= FILE_ATTRIBUTE_HIDDEN;
    }
    out_st->file_attributes = attrs;

    out_st->last_access_time = ext4_time_to_filetime(ino_st->i_atime, ino_st->i_atime_extra, has_extra);
    out_st->last_write_time = ext4_time_to_filetime(ino_st->i_mtime, ino_st->i_mtime_extra, has_extra);
    out_st->change_time = ext4_time_to_filetime(ino_st->i_ctime, ino_st->i_ctime_extra, has_extra);
    if (has_extra && ino_st->i_crtime != 0) {
        out_st->creation_time = ext4_time_to_filetime(ino_st->i_crtime, ino_st->i_crtime_extra, 1);
    } else {
        out_st->creation_time = out_st->change_time;
    }
}

int ext4_vfs_lower_stat_ino(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, const char *leaf_name, ext4_stat_t *out_st) {
    struct ext2_inode_large ino_st;
    errcode_t retval;
    ztrace_lock_guard_t lg;

    if (!ctx || !out_st || ino == 0) return -EINVAL;

    lg = ztrace_srw_acquire_shared(&ctx->rwlock, "ctx->rwlock wait (S)", "ctx->rwlock hold (S)");
    EnterCriticalSection(&ctx->fs_lock);
    memset(&ino_st, 0, sizeof(ino_st));
    retval = ext2fs_read_inode_full(ctx->fs, ino, (struct ext2_inode *)&ino_st, sizeof(ino_st));
    if (retval == 0) {
        ext4_fill_stat_from_inode(ctx, ino, &ino_st, leaf_name, out_st);
    }
    LeaveCriticalSection(&ctx->fs_lock);
    ztrace_srw_release_shared(&ctx->rwlock, &lg);
    return ext4_errcode_to_errno(retval);
}

int ext4_vfs_stat_ino(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, const char *leaf_name, ext4_stat_t *out_st) {
    if (ctx && ctx->overlay) {
        return overlay_vfs_stat_ino(ctx, ino, leaf_name, out_st);
    }
    return ext4_vfs_lower_stat_ino(ctx, ino, leaf_name, out_st);
}

int ext4_vfs_lower_stat_with_name(ext4_vfs_ctx_t *ctx, const char *path, ext4_stat_t *out_st,
                                  char *out_leaf, size_t out_leaf_sz) {
    char norm[1024];
    char actual_leaf[EXT2_NAME_LEN + 1];
    ext2_ino_t ino = 0;
    struct ext2_inode_large ino_st;
    errcode_t retval;
    int rc;

    if (!ctx || !path || !out_st) return -EINVAL;
    rc = ext4_vfs_normalize_path(path, norm, sizeof(norm));
    if (rc != 0) return rc;

    actual_leaf[0] = '\0';

    {
        ztrace_lock_guard_t lg = ztrace_srw_acquire_shared(
            &ctx->rwlock, "ctx->rwlock wait (S)", "ctx->rwlock hold (S)");
        EnterCriticalSection(&ctx->fs_lock);
        rc = ext4_vfs_lookup_locked_ex(ctx, norm, &ino, actual_leaf, sizeof(actual_leaf));
        if (rc == 0) {
            memset(&ino_st, 0, sizeof(ino_st));
            retval = ext2fs_read_inode_full(ctx->fs, ino, (struct ext2_inode *)&ino_st, sizeof(ino_st));
            if (retval == 0) {
                ext4_fill_stat_from_inode(ctx, ino, &ino_st, actual_leaf, out_st);
                if (out_leaf && out_leaf_sz > 0) {
                    strcpy_s(out_leaf, out_leaf_sz, actual_leaf);
                }
            } else {
                rc = ext4_errcode_to_errno(retval);
            }
        }
        LeaveCriticalSection(&ctx->fs_lock);
        ztrace_srw_release_shared(&ctx->rwlock, &lg);
    }
    return rc;
}

int ext4_vfs_lower_stat(ext4_vfs_ctx_t *ctx, const char *path, ext4_stat_t *out_st) {
    return ext4_vfs_lower_stat_with_name(ctx, path, out_st, NULL, 0);
}

int ext4_vfs_stat(ext4_vfs_ctx_t *ctx, const char *path, ext4_stat_t *out_st) {
    if (ctx && ctx->overlay) {
        return overlay_vfs_stat(ctx, path, out_st);
    }
    return ext4_vfs_lower_stat(ctx, path, out_st);
}

int ext4_vfs_set_times(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, uint32_t win_attrs,
                       uint64_t ctime_ft, uint64_t atime_ft, uint64_t mtime_ft, uint64_t chgtime_ft) {
    struct ext2_inode_large ino_st;
    errcode_t retval;

    if (!ctx || ino == 0) return -EINVAL;
    if (ctx->overlay) {
        return overlay_vfs_set_times(ctx, ino, win_attrs, ctime_ft, atime_ft, mtime_ft, chgtime_ft);
    }
    if (ctx->read_only) return -EROFS;

    {
        ztrace_lock_guard_t lg = ztrace_srw_acquire_exclusive(
            &ctx->rwlock, "ctx->rwlock wait (X)", "ctx->rwlock hold (X)");
        memset(&ino_st, 0, sizeof(ino_st));
        retval = ext2fs_read_inode_full(ctx->fs, ino, (struct ext2_inode *)&ino_st, sizeof(ino_st));
        if (retval == 0) {
            if (win_attrs != INVALID_FILE_ATTRIBUTES) {
                if (win_attrs & FILE_ATTRIBUTE_READONLY) {
                    ino_st.i_mode &= (uint16_t)(~0222);
                } else {
                    ino_st.i_mode |= 0200;
                }
            }
            if (ctime_ft != 0) {
                filetime_to_ext4_time(ctime_ft, &ino_st.i_crtime, &ino_st.i_crtime_extra);
            }
            if (atime_ft != 0) {
                filetime_to_ext4_time(atime_ft, &ino_st.i_atime, &ino_st.i_atime_extra);
            }
            if (mtime_ft != 0) {
                filetime_to_ext4_time(mtime_ft, &ino_st.i_mtime, &ino_st.i_mtime_extra);
            }
            if (chgtime_ft != 0) {
                filetime_to_ext4_time(chgtime_ft, &ino_st.i_ctime, &ino_st.i_ctime_extra);
            }
            retval = ext2fs_write_inode_full(ctx->fs, ino, (struct ext2_inode *)&ino_st, sizeof(ino_st));
        }
        ztrace_srw_release_exclusive(&ctx->rwlock, &lg);
    }
    return ext4_errcode_to_errno(retval);
}
