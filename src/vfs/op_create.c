#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include "../overlay/overlay_vfs.h"
#include "../trace/perfetto_trace.h"
#include <errno.h>
#include <string.h>

int ext4_vfs_create(ext4_vfs_ctx_t *ctx, const char *path, int is_dir, uint16_t mode,
                    uint32_t win_attrs, ext2_ino_t *out_ino) {
    char norm[1024];
    char parent_path[1024];
    char name[EXT2_NAME_LEN + 1];
    ext2_ino_t parent_ino = 0, existing_ino = 0, new_ino = 0;
    struct ext2_inode_large parent_st;
    struct ext2_inode_large ino_st;
    errcode_t retval;
    int rc;

    if (!ctx || !path) return -EINVAL;
    if (ctx->overlay) {
        return overlay_vfs_create_file(ctx, path, is_dir, mode, win_attrs, out_ino);
    }
    if (ctx->read_only) return -EROFS;
    if (is_dir) {
        return ext4_vfs_mkdir(ctx, path, mode ? mode : 0755, out_ino);
    }

    rc = ext4_vfs_normalize_path(path, norm, sizeof(norm));
    if (rc != 0) return rc;
    rc = ext4_vfs_split_parent(norm, parent_path, sizeof(parent_path), name, sizeof(name));
    if (rc != 0) return rc;

    ztrace_lock_guard_t lg = ztrace_srw_acquire_exclusive(
        &ctx->rwlock, "ctx->rwlock wait (X)", "ctx->rwlock hold (X)");

    rc = ext4_vfs_lookup_locked(ctx, parent_path, &parent_ino);
    if (rc != 0) {
        if (rc == -ENOENT) rc = -ENOENT;
        goto out;
    }

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

    /* Check if entry already exists */
    {
        char full_existing[1024];
        snprintf(full_existing, sizeof(full_existing), "%s%s%s",
                 parent_path, strcmp(parent_path, "/") == 0 ? "" : "/", name);
        if (ext4_vfs_lookup_locked(ctx, full_existing, &existing_ino) == 0) {
            rc = -EEXIST;
            goto out;
        }
    }

    if (mode == 0) mode = 0644;
    if (win_attrs != INVALID_FILE_ATTRIBUTES && (win_attrs & FILE_ATTRIBUTE_READONLY)) {
        mode &= (uint16_t)(~0222);
    }

    retval = ext2fs_new_inode(ctx->fs, parent_ino, LINUX_S_IFREG | mode, ctx->fs->inode_map, &new_ino);
    if (retval) {
        rc = ext4_errcode_to_errno(retval);
        goto out;
    }

    memset(&ino_st, 0, sizeof(ino_st));
    ino_st.i_mode = (uint16_t)(LINUX_S_IFREG | (mode & 07777));
    ino_st.i_links_count = 1;
    ino_st.i_generation = 1;
    ino_st.i_extra_isize = 32;
    ext4_inode_touch_times(&ino_st, 1, 1, 1);
    ino_st.i_crtime = ino_st.i_ctime;
    ino_st.i_crtime_extra = ino_st.i_ctime_extra;

    if (EXT2_HAS_INCOMPAT_FEATURE(ctx->fs->super, EXT4_FEATURE_INCOMPAT_EXTENTS)) {
        struct ext4_extent_header *eh = (struct ext4_extent_header *)ino_st.i_block;
        ino_st.i_flags |= EXT4_EXTENTS_FL;
        eh->eh_magic = EXT4_EXT_MAGIC;
        eh->eh_entries = 0;
        eh->eh_max = 4;
        eh->eh_depth = 0;
    }

    retval = ext2fs_write_inode_full(ctx->fs, new_ino, (struct ext2_inode *)&ino_st, sizeof(ino_st));
    if (retval) {
        rc = ext4_errcode_to_errno(retval);
        goto out;
    }

    ext2fs_inode_alloc_stats2(ctx->fs, new_ino, 1, 0);

    retval = ext2fs_link(ctx->fs, parent_ino, name, new_ino, EXT2_FT_REG_FILE);
    if (retval) {
        ext2fs_inode_alloc_stats2(ctx->fs, new_ino, -1, 0);
        rc = ext4_errcode_to_errno(retval);
        goto out;
    }

    /* Re-read parent_st because ext2fs_link -> ext2fs_expand_dir may have updated i_size/i_block */
    memset(&parent_st, 0, sizeof(parent_st));
    if (ext2fs_read_inode_full(ctx->fs, parent_ino, (struct ext2_inode *)&parent_st, sizeof(parent_st)) == 0) {
        ext4_inode_touch_times(&parent_st, 0, 1, 1);
        ext2fs_write_inode_full(ctx->fs, parent_ino, (struct ext2_inode *)&parent_st, sizeof(parent_st));
    }

    if (out_ino) *out_ino = new_ino;
    rc = 0;

out:
    ztrace_srw_release_exclusive(&ctx->rwlock, &lg);
    return rc;
}
