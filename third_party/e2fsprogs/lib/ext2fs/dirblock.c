#include "config.h"
#include <string.h>
#include "ext2fs/ext2fs.h"

errcode_t ext2fs_read_dir_block4(ext2_filsys fs, blk64_t block, void *buf, int flags, ext2_ino_t ino) {
    errcode_t retval;
    (void)flags;
    if (!fs || !buf) return EXT2_ET_INVALID_ARGUMENT;
    retval = io_channel_read_blk64(fs->io, block, 1, buf);
    if (retval) return retval;
    if (!(fs->flags & EXT2_FLAG_IGNORE_CSUM_ERRORS) &&
        ext2fs_has_feature_metadata_csum(fs->super)) {
        if (!ext2fs_dir_block_csum_verify(fs, ino, (struct ext2_dir_entry *)buf)) {
            return EXT2_ET_DIR_CSUM_INVALID;
        }
    }
    return 0;
}

errcode_t ext2fs_write_dir_block4(ext2_filsys fs, blk64_t block, void *buf, int flags, ext2_ino_t ino) {
    (void)flags;
    if (!fs || !buf) return EXT2_ET_INVALID_ARGUMENT;
    if (!(fs->flags & EXT2_FLAG_RW)) return EXT2_ET_RO_FILSYS;
    if (ext2fs_has_feature_metadata_csum(fs->super)) {
        ext2fs_dir_block_csum_set(fs, ino, (struct ext2_dir_entry *)buf);
    }
    return io_channel_write_blk64(fs->io, block, 1, buf);
}
