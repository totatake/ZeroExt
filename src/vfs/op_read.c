#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include <errno.h>

int ext4_vfs_read(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, void *buf, uint64_t offset,
                  uint32_t length, uint32_t *out_read) {
    ext2_file_t file = NULL;
    errcode_t retval;
    unsigned int got = 0;

    if (!ctx || !out_read || ino == 0) return -EINVAL;
    *out_read = 0;
    if (length == 0) return 0;
    if (!buf) return -EINVAL;

    AcquireSRWLockShared(&ctx->rwlock);
    retval = ext2fs_file_open(ctx->fs, ino, 0, &file);
    if (retval == 0) {
        struct ext2_inode *inode = ext2fs_file_get_inode(file);
        if (LINUX_S_ISDIR(inode->i_mode)) {
            retval = EXT2_ET_IS_A_DIR;
        } else {
            retval = ext2fs_file_llseek(file, offset, EXT2_SEEK_SET, NULL);
            if (retval == 0) {
                retval = ext2fs_file_read(file, buf, length, &got);
            }
        }
        ext2fs_file_close(file);
    }
    ReleaseSRWLockShared(&ctx->rwlock);

    if (retval != 0) return ext4_errcode_to_errno(retval);
    *out_read = (uint32_t)got;
    return 0;
}
