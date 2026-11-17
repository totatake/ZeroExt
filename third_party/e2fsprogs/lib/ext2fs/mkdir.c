#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ext2fs/ext2fs.h"

errcode_t ext2fs_mkdir(ext2_filsys fs, ext2_ino_t parent, ext2_ino_t inum, const char *name) {
    ext2_ino_t ino = inum;
    struct ext2_inode_large inode;
    struct ext2_inode parent_inode;
    char *dir_blk = NULL;
    blk64_t new_blk = 0;
    errcode_t retval;
    __u32 now = (__u32)time(NULL);
    struct ext4_extent_header *eh;

    if (!fs) return EXT2_ET_INVALID_ARGUMENT;
    if (!(fs->flags & EXT2_FLAG_RW)) return EXT2_ET_RO_FILSYS;

    if (ino == 0) {
        retval = ext2fs_new_inode(fs, parent, LINUX_S_IFDIR | 0755, fs->inode_map, &ino);
        if (retval) return retval;
    }

    retval = ext2fs_alloc_block2(fs, ext2fs_group_first_block2(fs, ext2fs_group_of_ino(fs, ino)), NULL, &new_blk);
    if (retval) return retval;

    retval = ext2fs_new_dir_block(fs, ino, parent, &dir_blk);
    if (retval) {
        ext2fs_block_alloc_stats2(fs, new_blk, -1);
        return retval;
    }

    memset(&inode, 0, sizeof(inode));
    inode.i_mode = LINUX_S_IFDIR | 0755;
    inode.i_links_count = 2;
    inode.i_size = fs->blocksize;
    inode.i_atime = now;
    inode.i_ctime = now;
    inode.i_mtime = now;
    inode.i_crtime = now;
    inode.i_generation = 1;
    inode.i_extra_isize = 32;
    ext2fs_iblk_add_blocks(fs, (struct ext2_inode *)&inode, 1);

    if (EXT2_HAS_INCOMPAT_FEATURE(fs->super, EXT4_FEATURE_INCOMPAT_EXTENTS)) {
        struct ext4_extent *ex;
        inode.i_flags |= EXT4_EXTENTS_FL;
        eh = (struct ext4_extent_header *)inode.i_block;
        eh->eh_magic = EXT4_EXT_MAGIC;
        eh->eh_entries = 1;
        eh->eh_max = 4;
        eh->eh_depth = 0;
        ex = (struct ext4_extent *)((char *)inode.i_block + sizeof(*eh));
        ex->ee_block = 0;
        ex->ee_len = 1;
        ex->ee_start_lo = (__u32)(new_blk & 0xFFFFFFFFULL);
        ex->ee_start_hi = (__u16)(new_blk >> 32);
    } else {
        inode.i_block[0] = (__u32)new_blk;
    }

    retval = ext2fs_write_inode_full(fs, ino, (struct ext2_inode *)&inode, sizeof(inode));
    if (retval) goto err_free;

    retval = ext2fs_write_dir_block4(fs, new_blk, dir_blk, 0, ino);
    if (retval) goto err_free;

    ext2fs_free_mem(&dir_blk);
    ext2fs_inode_alloc_stats2(fs, ino, 1, 1);

    if (name) {
        retval = ext2fs_link(fs, parent, name, ino, EXT2_FT_DIR);
        if (retval) {
            ext2fs_inode_alloc_stats2(fs, ino, -1, 1);
            ext2fs_block_alloc_stats2(fs, new_blk, -1);
            return retval;
        }
        if (ext2fs_read_inode(fs, parent, &parent_inode) == 0) {
            parent_inode.i_links_count++;
            parent_inode.i_mtime = now;
            parent_inode.i_ctime = now;
            ext2fs_write_inode(fs, parent, &parent_inode);
        }
    }
    return 0;

err_free:
    ext2fs_free_mem(&dir_blk);
    ext2fs_block_alloc_stats2(fs, new_blk, -1);
    return retval;
}
