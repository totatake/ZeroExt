#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include "../overlay/overlay_vfs.h"
#include <errno.h>
#include <string.h>

int ext4_vfs_truncate(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, uint64_t new_size) {
    ext2_file_t file = NULL;
    errcode_t retval;

    if (!ctx || ino == 0) return -EINVAL;
    if (ctx->overlay) {
        return overlay_vfs_truncate(ctx, ino, new_size);
    }
    if (ctx->read_only) return -EROFS;

    AcquireSRWLockExclusive(&ctx->rwlock);
    retval = ext2fs_file_open(ctx->fs, ino, EXT2_FILE_WRITE, &file);
    if (retval == 0) {
        struct ext2_inode *inode = ext2fs_file_get_inode(file);
        if (LINUX_S_ISDIR(inode->i_mode)) {
            retval = EXT2_ET_IS_A_DIR;
        } else {
            retval = ext2fs_file_set_size2(file, new_size);
        }
        ext2fs_file_close(file);
    }
    if (retval == 0) {
        struct ext2_inode_large full_ino;
        memset(&full_ino, 0, sizeof(full_ino));
        if (ext2fs_read_inode_full(ctx->fs, ino, (struct ext2_inode *)&full_ino, sizeof(full_ino)) == 0) {
            ext4_inode_touch_times(&full_ino, 0, 1, 1);
            ext2fs_write_inode_full(ctx->fs, ino, (struct ext2_inode *)&full_ino, sizeof(full_ino));
        }
    }
    ReleaseSRWLockExclusive(&ctx->rwlock);
    return ext4_errcode_to_errno(retval);
}
