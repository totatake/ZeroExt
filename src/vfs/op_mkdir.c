#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include "../overlay/overlay_vfs.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct empty_check_ctx {
    int non_empty;
};

static int empty_check_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                          int offset, int blocksize, char *buf, void *priv_data) {
    struct empty_check_ctx *c = (struct empty_check_ctx *)priv_data;
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

int ext4_vfs_mkdir(ext4_vfs_ctx_t *ctx, const char *path, uint16_t mode, ext2_ino_t *out_ino) {
    char norm[1024];
    char parent_path[1024];
    char name[EXT2_NAME_LEN + 1];
    ext2_ino_t parent_ino = 0, existing = 0, new_ino = 0;
    struct ext2_inode_large parent_st;
    errcode_t retval;
    int rc;

    if (!ctx || !path) return -EINVAL;
    if (ctx->overlay) {
        return overlay_vfs_mkdir(ctx, path, mode, out_ino);
    }
    if (ctx->read_only) return -EROFS;

    rc = ext4_vfs_normalize_path(path, norm, sizeof(norm));
    if (rc != 0) return rc;
    rc = ext4_vfs_split_parent(norm, parent_path, sizeof(parent_path), name, sizeof(name));
    if (rc != 0) return rc;

    AcquireSRWLockExclusive(&ctx->rwlock);

    rc = ext4_vfs_lookup_locked(ctx, parent_path, &parent_ino);
    if (rc != 0) goto out;

    memset(&parent_st, 0, sizeof(parent_st));
    retval = ext2fs_read_inode_full(ctx->fs, parent_ino, (struct ext2_inode *)&parent_st, sizeof(parent_st));
    if (retval) {
        rc = ext4_errcode_to_errno(retval);
        goto out;
    }
    if (!LINUX_S_ISDIR(parent_st.i_mode)) {
        rc = -ENOTDIR;
        goto out;
    }

    {
        char full_existing[1024];
        snprintf(full_existing, sizeof(full_existing), "%s%s%s",
                 parent_path, strcmp(parent_path, "/") == 0 ? "" : "/", name);
        if (ext4_vfs_lookup_locked(ctx, full_existing, &existing) == 0) {
            rc = -EEXIST;
            goto out;
        }
    }

    retval = ext2fs_new_inode(ctx->fs, parent_ino, LINUX_S_IFDIR | (mode ? mode : 0755), ctx->fs->inode_map, &new_ino);
    if (retval) {
        rc = ext4_errcode_to_errno(retval);
        goto out;
    }

    retval = ext2fs_mkdir(ctx->fs, parent_ino, new_ino, name);
    if (retval) {
        rc = ext4_errcode_to_errno(retval);
        goto out;
    }

    if (mode != 0 && mode != 0755) {
        struct ext2_inode_large dir_st;
        memset(&dir_st, 0, sizeof(dir_st));
        if (ext2fs_read_inode_full(ctx->fs, new_ino, (struct ext2_inode *)&dir_st, sizeof(dir_st)) == 0) {
            dir_st.i_mode = (uint16_t)(LINUX_S_IFDIR | (mode & 07777));
            ext2fs_write_inode_full(ctx->fs, new_ino, (struct ext2_inode *)&dir_st, sizeof(dir_st));
        }
    }

    if (out_ino) *out_ino = new_ino;
    rc = 0;

out:
    ReleaseSRWLockExclusive(&ctx->rwlock);
    return rc;
}

int ext4_vfs_can_delete(ext4_vfs_ctx_t *ctx, ext2_ino_t ino) {
    struct ext2_inode ino_st;
    errcode_t retval;
    struct empty_check_ctx check;

    if (!ctx || ino == 0) return -EINVAL;
    if (ctx->read_only) return -EROFS;
    if (ctx->overlay) {
        return overlay_vfs_can_delete(ctx, ino);
    }
    if (ino == EXT2_ROOT_INO) return -EACCES;

    AcquireSRWLockShared(&ctx->rwlock);
    retval = ext2fs_read_inode(ctx->fs, ino, &ino_st);
    if (retval != 0) {
        ReleaseSRWLockShared(&ctx->rwlock);
        return ext4_errcode_to_errno(retval);
    }
    if (!LINUX_S_ISDIR(ino_st.i_mode)) {
        ReleaseSRWLockShared(&ctx->rwlock);
        return 0;
    }

    check.non_empty = 0;
    retval = (errcode_t)ext2fs_dir_iterate2(ctx->fs, ino, 0, NULL, empty_check_cb, &check);
    ReleaseSRWLockShared(&ctx->rwlock);

    if (retval != 0) return ext4_errcode_to_errno(retval);
    return check.non_empty ? -ENOTEMPTY : 0;
}
