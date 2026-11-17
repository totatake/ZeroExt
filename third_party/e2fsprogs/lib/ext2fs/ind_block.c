#include "config.h"
#include "ext2fs/ext2fs.h"

errcode_t ext2fs_read_ind_block(ext2_filsys fs, blk_t blk, void *buf) {
    return io_channel_read_blk64(fs->io, (blk64_t)blk, 1, buf);
}

errcode_t ext2fs_write_ind_block(ext2_filsys fs, blk_t blk, void *buf) {
    return io_channel_write_blk64(fs->io, (blk64_t)blk, 1, buf);
}
