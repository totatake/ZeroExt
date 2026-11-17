#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

extern int ext4_vfs_unlink_locked(ext4_vfs_ctx_t *ctx, ext2_ino_t parent_ino, const char *name);

struct update_dotdot_ctx {
    ext2_ino_t new_parent_ino;
    int        done;
};

static int update_dotdot_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                            int offset, int blocksize, char *buf, void *priv_data) {
    struct update_dotdot_ctx *c = (struct update_dotdot_ctx *)priv_data;
    struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)dirent;
    (void)dir;
    (void)entry;
    (void)offset;
    (void)blocksize;
    (void)buf;

    if (de->inode != 0 && de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') {
        de->inode = c->new_parent_ino;
        c->done = 1;
        return DIRENT_CHANGED | DIRENT_ABORT;
    }
    return 0;
}

struct resolve_name_ctx {
    const char *target;
    char        actual_name[EXT2_NAME_LEN + 1];
    ext2_ino_t  ino;
    int         found;
};

static int resolve_exact_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                            int offset, int blocksize, char *buf, void *priv_data) {
    struct resolve_name_ctx *c = (struct resolve_name_ctx *)priv_data;
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

static int resolve_ci_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                         int offset, int blocksize, char *buf, void *priv_data) {
    struct resolve_name_ctx *c = (struct resolve_name_ctx *)priv_data;
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

static int is_descendant_locked(ext4_vfs_ctx_t *ctx, ext2_ino_t candidate, ext2_ino_t ancestor) {
    ext2_ino_t cur = candidate;
    for (int depth = 0; depth < 256; depth++) {
        ext2_ino_t parent = 0;
        if (cur == ancestor) return 1;
        if (cur == EXT2_ROOT_INO || cur == 0) return 0;
        if (ext2fs_lookup(ctx->fs, cur, "..", 2, NULL, &parent) != 0) return 0;
        if (parent == cur) return 0;
        cur = parent;
    }
    return 0;
}

int ext4_vfs_rename(ext4_vfs_ctx_t *ctx, const char *old_path, const char *new_path, int replace_if_exists) {
    char norm_old[1024], norm_new[1024];
    char old_parent[1024], new_parent[1024];
    char old_name[EXT2_NAME_LEN + 1], new_name[EXT2_NAME_LEN + 1];
    ext2_ino_t old_parent_ino = 0, new_parent_ino = 0;
    struct resolve_name_ctx src_ctx, dst_ctx;
    struct ext2_inode src_st;
    errcode_t retval;
    int rc;

    if (!ctx || !old_path || !new_path) return -EINVAL;
    if (ctx->read_only) return -EROFS;

    rc = ext4_vfs_normalize_path(old_path, norm_old, sizeof(norm_old));
    if (rc != 0) return rc;
    rc = ext4_vfs_normalize_path(new_path, norm_new, sizeof(norm_new));
    if (rc != 0) return rc;
    if (strcmp(norm_old, norm_new) == 0) return 0;
    if (strcmp(norm_old, "/") == 0 || strcmp(norm_new, "/") == 0) return -EINVAL;

    rc = ext4_vfs_split_parent(norm_old, old_parent, sizeof(old_parent), old_name, sizeof(old_name));
    if (rc != 0) return rc;
    rc = ext4_vfs_split_parent(norm_new, new_parent, sizeof(new_parent), new_name, sizeof(new_name));
    if (rc != 0) return rc;

    AcquireSRWLockExclusive(&ctx->rwlock);

    rc = ext4_vfs_lookup_locked(ctx, old_parent, &old_parent_ino);
    if (rc != 0) goto out;
    rc = ext4_vfs_lookup_locked(ctx, new_parent, &new_parent_ino);
    if (rc != 0) goto out;

    {
        struct ext2_inode np_st;
        retval = ext2fs_read_inode(ctx->fs, new_parent_ino, &np_st);
        if (retval) {
            rc = ext4_errcode_to_errno(retval);
            goto out;
        }
        if (!LINUX_S_ISDIR(np_st.i_mode)) {
            rc = -ENOTDIR;
            goto out;
        }
    }

    memset(&src_ctx, 0, sizeof(src_ctx));
    src_ctx.target = old_name;
    ext2fs_dir_iterate2(ctx->fs, old_parent_ino, 0, NULL, resolve_exact_cb, &src_ctx);
    if (!src_ctx.found && ctx->case_insensitive) {
        ext2fs_dir_iterate2(ctx->fs, old_parent_ino, 0, NULL, resolve_ci_cb, &src_ctx);
    }
    if (!src_ctx.found) {
        rc = -ENOENT;
        goto out;
    }

    retval = ext2fs_read_inode(ctx->fs, src_ctx.ino, &src_st);
    if (retval) {
        rc = ext4_errcode_to_errno(retval);
        goto out;
    }

    if (LINUX_S_ISDIR(src_st.i_mode) && is_descendant_locked(ctx, new_parent_ino, src_ctx.ino)) {
        rc = -EINVAL;
        goto out;
    }

    memset(&dst_ctx, 0, sizeof(dst_ctx));
    dst_ctx.target = new_name;
    ext2fs_dir_iterate2(ctx->fs, new_parent_ino, 0, NULL, resolve_exact_cb, &dst_ctx);
    if (!dst_ctx.found && ctx->case_insensitive) {
        ext2fs_dir_iterate2(ctx->fs, new_parent_ino, 0, NULL, resolve_ci_cb, &dst_ctx);
    }

    if (dst_ctx.found) {
        if (dst_ctx.ino == src_ctx.ino && old_parent_ino == new_parent_ino) {
            /* Case-only rename on the same directory entry */
            retval = ext2fs_unlink(ctx->fs, old_parent_ino, src_ctx.actual_name, src_ctx.ino, 0);
            if (retval) {
                rc = ext4_errcode_to_errno(retval);
                goto out;
            }
            retval = ext2fs_link(ctx->fs, new_parent_ino, new_name, src_ctx.ino, ext2fs_file_type(src_st.i_mode));
            rc = ext4_errcode_to_errno(retval);
            goto out;
        }
        if (!replace_if_exists) {
            rc = -EEXIST;
            goto out;
        }
        {
            struct ext2_inode dst_st;
            retval = ext2fs_read_inode(ctx->fs, dst_ctx.ino, &dst_st);
            if (retval) {
                rc = ext4_errcode_to_errno(retval);
                goto out;
            }
            if (LINUX_S_ISDIR(src_st.i_mode) && !LINUX_S_ISDIR(dst_st.i_mode)) {
                rc = -ENOTDIR;
                goto out;
            }
            if (!LINUX_S_ISDIR(src_st.i_mode) && LINUX_S_ISDIR(dst_st.i_mode)) {
                rc = -EISDIR;
                goto out;
            }
            rc = ext4_vfs_unlink_locked(ctx, new_parent_ino, dst_ctx.actual_name);
            if (rc != 0) goto out;
        }
    }

    retval = ext2fs_link(ctx->fs, new_parent_ino, new_name, src_ctx.ino, ext2fs_file_type(src_st.i_mode));
    if (retval) {
        rc = ext4_errcode_to_errno(retval);
        goto out;
    }

    retval = ext2fs_unlink(ctx->fs, old_parent_ino, src_ctx.actual_name, src_ctx.ino, 0);
    if (retval) {
        rc = ext4_errcode_to_errno(retval);
        goto out;
    }

    if (LINUX_S_ISDIR(src_st.i_mode) && old_parent_ino != new_parent_ino) {
        struct update_dotdot_ctx dctx;
        struct ext2_inode_large op_st, np_st;
        dctx.new_parent_ino = new_parent_ino;
        dctx.done = 0;
        ext2fs_dir_iterate2(ctx->fs, src_ctx.ino, 0, NULL, update_dotdot_cb, &dctx);

        memset(&op_st, 0, sizeof(op_st));
        if (ext2fs_read_inode_full(ctx->fs, old_parent_ino, (struct ext2_inode *)&op_st, sizeof(op_st)) == 0) {
            if (op_st.i_links_count > 1) op_st.i_links_count--;
            ext4_inode_touch_times(&op_st, 0, 1, 1);
            ext2fs_write_inode_full(ctx->fs, old_parent_ino, (struct ext2_inode *)&op_st, sizeof(op_st));
        }
        memset(&np_st, 0, sizeof(np_st));
        if (ext2fs_read_inode_full(ctx->fs, new_parent_ino, (struct ext2_inode *)&np_st, sizeof(np_st)) == 0) {
            np_st.i_links_count++;
            ext4_inode_touch_times(&np_st, 0, 1, 1);
            ext2fs_write_inode_full(ctx->fs, new_parent_ino, (struct ext2_inode *)&np_st, sizeof(np_st));
        }
    }

    {
        struct ext2_inode_large s_large;
        memset(&s_large, 0, sizeof(s_large));
        if (ext2fs_read_inode_full(ctx->fs, src_ctx.ino, (struct ext2_inode *)&s_large, sizeof(s_large)) == 0) {
            ext4_inode_touch_times(&s_large, 0, 0, 1);
            ext2fs_write_inode_full(ctx->fs, src_ctx.ino, (struct ext2_inode *)&s_large, sizeof(s_large));
        }
    }

    rc = 0;

out:
    ReleaseSRWLockExclusive(&ctx->rwlock);
    return rc;
}
