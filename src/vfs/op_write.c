#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include "../overlay/overlay_vfs.h"
#include "../trace/perfetto_trace.h"
#include <errno.h>
#include <string.h>

int ext4_vfs_write(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, const void *buf, uint64_t offset,
                   uint32_t length, int write_to_eof, uint32_t *out_written, uint64_t *out_new_size) {
    ext2_file_t file = NULL;
    errcode_t retval;
    unsigned int written = 0;
    ext2_off64_t fsize = 0;
    uint64_t t_vfs;
    ztrace_lock_guard_t lg;

    if (!ctx || !out_written || ino == 0) return -EINVAL;
    if (ctx->overlay) {
        return overlay_vfs_write(ctx, ino, buf, offset, length, write_to_eof, out_written, out_new_size);
    }
    *out_written = 0;
    if (out_new_size) *out_new_size = 0;
    if (ctx->read_only) return -EROFS;
    if (length > 0 && !buf) return -EINVAL;

    t_vfs = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    lg = ztrace_srw_acquire_exclusive(&ctx->rwlock, "ctx->rwlock wait (X)", "ctx->rwlock hold (X)");
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
    ztrace_srw_release_exclusive(&ctx->rwlock, &lg);
    if (t_vfs) {
        uint64_t end = ztrace_now_qpc();
        int rc_vfs = (retval != 0) ? ext4_errcode_to_errno(retval) : 0;
        ztrace_record_complete("vfs", "ext2fs_file_write", t_vfs, end - t_vfs,
                               (uint64_t)ino, (uint64_t)written, (int64_t)rc_vfs, ZTRACE_ARGS_RW_OP);
    }

    if (retval != 0) return ext4_errcode_to_errno(retval);
    *out_written = (uint32_t)written;
    if (out_new_size) *out_new_size = fsize;
    return 0;
}
