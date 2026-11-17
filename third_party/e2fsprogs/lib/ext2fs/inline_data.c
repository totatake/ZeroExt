#include "config.h"
#include <string.h>
#include "ext2fs/ext2fs.h"

errcode_t ext2fs_inline_data_size(ext2_filsys fs, ext2_ino_t ino, size_t *size) {
    struct ext2_inode inode;
    errcode_t retval;
    if (!fs || !size) return EXT2_ET_INVALID_ARGUMENT;
    retval = ext2fs_read_inode(fs, ino, &inode);
    if (retval) return retval;
    if (!(inode.i_flags & EXT4_INLINE_DATA_FL)) {
        *size = 0;
        return 0;
    }
    *size = inode.i_size;
    return 0;
}

errcode_t ext2fs_inline_data_get(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode,
                                 void *buf, size_t *size) {
    struct ext2_inode local_ino;
    size_t max_inline = sizeof(local_ino.i_block);
    errcode_t retval;

    if (!fs || !buf) return EXT2_ET_INVALID_ARGUMENT;
    if (!inode) {
        retval = ext2fs_read_inode(fs, ino, &local_ino);
        if (retval) return retval;
        inode = &local_ino;
    }
    if (!(inode->i_flags & EXT4_INLINE_DATA_FL)) {
        if (size) *size = 0;
        return EXT2_ET_OP_NOT_SUPPORTED;
    }
    {
        size_t copy_sz = inode->i_size < max_inline ? inode->i_size : max_inline;
        memcpy(buf, inode->i_block, copy_sz);
        if (size) *size = copy_sz;
    }
    return 0;
}
