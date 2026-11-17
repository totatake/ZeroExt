#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include <errno.h>
#include <string.h>

int ext4_vfs_write(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, const void *buf, uint64_t offset,
                   uint32_t length, int write_to_eof, uint32_t *out_written, uint64_t *out_new_size) {
    ext2_file_t file = NULL;
    errcode_t retval;
    unsigned int written = 0;
    ext2_off64_t fsize = 0;

    if (!ctx || !out_written || ino == 0) return -EINVAL;
    *out_written = 0;
    if (out_new_size) *out_new_size = 0;
    if (ctx->read_only) return -EROFS;
    if (length > 0 && !buf) return -EINVAL;

    AcquireSRWLockExclusive(&ctx->rwlock);
    retval = ext2fs_file_open(ctx->fs, ino, EXT2_FILE_WRITE, &file);
    if (retval == 0) {
        struct ext2_inode *inode = ext2fs_file_get_inode(file);
        if (LINUX_S_ISDIR(inode->i_mode)) {
            retval = EXT2_ET_IS_A_DIR;
        } else {
            if (write_to_eof) {
                retval = ext2fs_file_llseek(file, 0, EXT2_SEEK_END, NULL);
            } else {
                retval = ext2fs_file_llseek(file, offset, EXT2_SEEK_SET, NULL);
            }
            if (retval == 0 && length > 0) {
                retval = ext2fs_file_write(file, buf, length, &written);
            }
            if (retval == 0) {
                struct ext2_inode_large full_ino;
                ext2fs_file_get_size(file, &fsize);
                ext2fs_file_close(file);
                file = NULL;

                memset(&full_ino, 0, sizeof(full_ino));
                if (ext2fs_read_inode_full(ctx->fs, ino, (struct ext2_inode *)&full_ino, sizeof(full_ino)) == 0) {
                    ext4_inode_touch_times(&full_ino, 0, 1, 1);
                    ext2fs_write_inode_full(ctx->fs, ino, (struct ext2_inode *)&full_ino, sizeof(full_ino));
                }
            }
        }
        if (file) {
            ext2fs_file_close(file);
        }
    }
    ReleaseSRWLockExclusive(&ctx->rwlock);

    if (retval != 0) return ext4_errcode_to_errno(retval);
    *out_written = (uint32_t)written;
    if (out_new_size) *out_new_size = fsize;
    return 0;
}
