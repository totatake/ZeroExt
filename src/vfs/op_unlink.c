#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include "../extent/extent_map.h"
#include "../overlay/overlay_vfs.h"
#include <errno.h>
#include <string.h>
#include <time.h>

struct exact_name_ctx {
    const char *target;
    char        actual_name[EXT2_NAME_LEN + 1];
    ext2_ino_t  ino;
    int         found;
};

static int find_exact_name_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                              int offset, int blocksize, char *buf, void *priv_data) {
    struct exact_name_ctx *c = (struct exact_name_ctx *)priv_data;
    struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)dirent;
    char tmp[EXT2_NAME_LEN + 1];
    (void)dir;
    (void)entry;
    (void)offset;
    (void)blocksize;
    (void)buf;

    if (de->inode == 0 || de->name_len > EXT2_NAME_LEN) return 0;
    memcpy(tmp, de->name, de->name_len);
    tmp[de->name_len] = '\0';
    if (strcmp(tmp, c->target) == 0) {
        strcpy_s(c->actual_name, sizeof(c->actual_name), tmp);
        c->ino = de->inode;
        c->found = 1;
        return DIRENT_ABORT;
    }
    return 0;
}

static int find_ci_name_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                           int offset, int blocksize, char *buf, void *priv_data) {
    struct exact_name_ctx *c = (struct exact_name_ctx *)priv_data;
    struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)dirent;
    char tmp[EXT2_NAME_LEN + 1];
    (void)dir;
    (void)entry;
    (void)offset;
    (void)blocksize;
    (void)buf;

    if (de->inode == 0 || de->name_len > EXT2_NAME_LEN) return 0;
    memcpy(tmp, de->name, de->name_len);
    tmp[de->name_len] = '\0';
    if (_stricmp(tmp, c->target) == 0) {
        strcpy_s(c->actual_name, sizeof(c->actual_name), tmp);
        c->ino = de->inode;
        c->found = 1;
        return DIRENT_ABORT;
    }
    return 0;
}

struct dir_empty_ctx {
    int non_empty;
};

static int dir_empty_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                        int offset, int blocksize, char *buf, void *priv_data) {
    struct dir_empty_ctx *c = (struct dir_empty_ctx *)priv_data;
    struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)dirent;
    (void)dir;
    (void)entry;
    (void)offset;
    (void)blocksize;
    (void)buf;

    if (de->inode == 0) return 0;
    if (de->name_len == 1 && de->name[0] == '.') return 0;
    if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') return 0;
    c->non_empty = 1;
    return DIRENT_ABORT;
}

int ext4_vfs_unlink_locked(ext4_vfs_ctx_t *ctx, ext2_ino_t parent_ino, const char *name) {
    struct exact_name_ctx fctx;
    struct ext2_inode target_ino_st;
    struct ext2_inode_large parent_st;
    errcode_t retval;
    int is_dir;

    memset(&fctx, 0, sizeof(fctx));
    fctx.target = name;
    ext2fs_dir_iterate2(ctx->fs, parent_ino, 0, NULL, find_exact_name_cb, &fctx);
    if (!fctx.found && ctx->case_insensitive) {
        ext2fs_dir_iterate2(ctx->fs, parent_ino, 0, NULL, find_ci_name_cb, &fctx);
    }
    if (!fctx.found) {
        return -ENOENT;
    }
    if (fctx.ino == EXT2_ROOT_INO) {
        return -EACCES;
    }

    retval = ext2fs_read_inode(ctx->fs, fctx.ino, &target_ino_st);
    if (retval) return ext4_errcode_to_errno(retval);

    is_dir = LINUX_S_ISDIR(target_ino_st.i_mode);
    if (is_dir) {
        struct dir_empty_ctx empty;
        empty.non_empty = 0;
        ext2fs_dir_iterate2(ctx->fs, fctx.ino, 0, NULL, dir_empty_cb, &empty);
        if (empty.non_empty) {
            return -ENOTEMPTY;
        }
    }

    retval = ext2fs_unlink(ctx->fs, parent_ino, fctx.actual_name, fctx.ino, 0);
    if (retval) return ext4_errcode_to_errno(retval);

    memset(&parent_st, 0, sizeof(parent_st));
    if (ext2fs_read_inode_full(ctx->fs, parent_ino, (struct ext2_inode *)&parent_st, sizeof(parent_st)) == 0) {
        if (is_dir && parent_st.i_links_count > 1) {
            parent_st.i_links_count--;
        }
        ext4_inode_touch_times(&parent_st, 0, 1, 1);
        ext2fs_write_inode_full(ctx->fs, parent_ino, (struct ext2_inode *)&parent_st, sizeof(parent_st));
    }

    if (is_dir) {
        ext2fs_punch(ctx->fs, fctx.ino, &target_ino_st, NULL, 0, (blk64_t)-1);
        target_ino_st.i_links_count = 0;
        target_ino_st.i_dtime = (__u32)time(NULL);
        ext2fs_write_inode(ctx->fs, fctx.ino, &target_ino_st);
        ext2fs_inode_alloc_stats2(ctx->fs, fctx.ino, -1, 1);
    } else {
        if (target_ino_st.i_links_count > 0) {
            target_ino_st.i_links_count--;
        }
        if (target_ino_st.i_links_count == 0) {
            if (!ext2fs_is_fast_symlink(&target_ino_st)) {
                ext2fs_punch(ctx->fs, fctx.ino, &target_ino_st, NULL, 0, (blk64_t)-1);
            }
            target_ino_st.i_dtime = (__u32)time(NULL);
            ext2fs_write_inode(ctx->fs, fctx.ino, &target_ino_st);
            ext2fs_inode_alloc_stats2(ctx->fs, fctx.ino, -1, 0);
        } else {
            target_ino_st.i_ctime = (__u32)time(NULL);
            ext2fs_write_inode(ctx->fs, fctx.ino, &target_ino_st);
        }
    }
    if (ctx->extent_cache) {
        ext4_extent_cache_invalidate(ctx->extent_cache, fctx.ino);
    }

    return 0;
}

int ext4_vfs_unlink(ext4_vfs_ctx_t *ctx, const char *path) {
    char norm[1024];
    char parent_path[1024];
    char name[EXT2_NAME_LEN + 1];
    ext2_ino_t parent_ino = 0;
    int rc;

    if (!ctx || !path) return -EINVAL;
    if (ctx->overlay) {
        return overlay_vfs_unlink(ctx, path);
    }
    if (ctx->read_only) return -EROFS;

    rc = ext4_vfs_normalize_path(path, norm, sizeof(norm));
    if (rc != 0) return rc;
    if (strcmp(norm, "/") == 0) return -EACCES;

    rc = ext4_vfs_split_parent(norm, parent_path, sizeof(parent_path), name, sizeof(name));
    if (rc != 0) return rc;

    AcquireSRWLockExclusive(&ctx->rwlock);
    rc = ext4_vfs_lookup_locked(ctx, parent_path, &parent_ino);
    if (rc == 0) {
        rc = ext4_vfs_unlink_locked(ctx, parent_ino, name);
    }
    ReleaseSRWLockExclusive(&ctx->rwlock);
    return rc;
}
