#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "ext2fs/ext2fs.h"

static errcode_t get_inode_location(ext2_filsys fs, ext2_ino_t ino, blk64_t *out_blk, unsigned int *out_off) {
    dgrp_t group;
    unsigned long offset_in_group;
    blk64_t itable_blk;
    unsigned int inode_size;

    if (!fs || ino == 0 || ino > fs->super->s_inodes_count) {
        return EXT2_ET_BAD_INODE_NUM;
    }
    inode_size = EXT2_INODE_SIZE(fs->super);
    group = ext2fs_group_of_ino(fs, ino);
    itable_blk = ext2fs_inode_table_loc(fs, group);
    if (itable_blk == 0 || itable_blk >= fs->blocks_count) {
        return EXT2_ET_GDESC_BAD_INODE_TABLE;
    }
    offset_in_group = (ino - 1) % fs->super->s_inodes_per_group;
    {
        __u64 byte_off = (__u64)offset_in_group * inode_size;
        *out_blk = itable_blk + (byte_off / fs->blocksize);
        *out_off = (unsigned int)(byte_off % fs->blocksize);
    }
    return 0;
}

errcode_t ext2fs_read_inode_full(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode, int bufsize) {
    blk64_t blk = 0;
    unsigned int blk_off = 0;
    unsigned int inode_size;
    char *blk_buf = NULL;
    errcode_t retval;

    if (!fs || !inode || bufsize <= 0) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    retval = get_inode_location(fs, ino, &blk, &blk_off);
    if (retval) {
        return retval;
    }
    inode_size = EXT2_INODE_SIZE(fs->super);
    retval = ext2fs_get_mem(fs->blocksize, &blk_buf);
    if (retval) {
        return retval;
    }
    retval = io_channel_read_blk64(fs->io, blk, 1, blk_buf);
    if (retval == 0) {
        struct ext2_inode_large *disk_ino = (struct ext2_inode_large *)(blk_buf + blk_off);
        size_t copy_sz = (size_t)bufsize < inode_size ? (size_t)bufsize : inode_size;
        if (!(fs->flags & EXT2_FLAG_IGNORE_CSUM_ERRORS) &&
            ext2fs_has_feature_metadata_csum(fs->super)) {
            int allocated = fs->inode_map ? ext2fs_test_inode_bitmap2(fs->inode_map, ino)
                                          : (disk_ino->i_mode != 0 || disk_ino->i_links_count != 0);
            if (allocated) {
                retval = ext2fs_inode_csum_verify(fs, ino, disk_ino);
            }
        }
        if (retval == 0) {
            memset(inode, 0, (size_t)bufsize);
            memcpy(inode, disk_ino, copy_sz);
        }
    }
    ext2fs_free_mem(&blk_buf);
    return retval;
}

errcode_t ext2fs_read_inode(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode) {
    return ext2fs_read_inode_full(fs, ino, inode, sizeof(struct ext2_inode));
}

static errcode_t write_inode_internal(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode, int bufsize, int is_new) {
    blk64_t blk = 0;
    unsigned int blk_off = 0;
    unsigned int inode_size;
    char *blk_buf = NULL;
    errcode_t retval;

    if (!fs || !inode || bufsize <= 0) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    if (!(fs->flags & EXT2_FLAG_RW)) {
        return EXT2_ET_RO_FILSYS;
    }
    retval = get_inode_location(fs, ino, &blk, &blk_off);
    if (retval) {
        return retval;
    }
    inode_size = EXT2_INODE_SIZE(fs->super);
    retval = ext2fs_get_mem(fs->blocksize, &blk_buf);
    if (retval) {
        return retval;
    }
    retval = io_channel_read_blk64(fs->io, blk, 1, blk_buf);
    if (retval == 0) {
        struct ext2_inode_large *disk_ino = (struct ext2_inode_large *)(blk_buf + blk_off);
        size_t copy_sz = (size_t)bufsize < inode_size ? (size_t)bufsize : inode_size;
        if (is_new) {
            memset(disk_ino, 0, inode_size);
        }
        memcpy(disk_ino, inode, copy_sz);
        if (inode_size > EXT2_GOOD_OLD_INODE_SIZE && disk_ino->i_extra_isize == 0) {
            __u16 min_extra = fs->super->s_want_extra_isize ? fs->super->s_want_extra_isize : 32;
            if (min_extra > inode_size - EXT2_GOOD_OLD_INODE_SIZE) {
                min_extra = (__u16)(inode_size - EXT2_GOOD_OLD_INODE_SIZE);
            }
            disk_ino->i_extra_isize = min_extra;
        }
        ext2fs_inode_csum_set(fs, ino, disk_ino);
        retval = io_channel_write_blk64(fs->io, blk, 1, blk_buf);
    }
    ext2fs_free_mem(&blk_buf);
    return retval;
}

errcode_t ext2fs_write_inode_full(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode, int bufsize) {
    return write_inode_internal(fs, ino, inode, bufsize, 0);
}

errcode_t ext2fs_write_inode(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode) {
    return write_inode_internal(fs, ino, inode, sizeof(struct ext2_inode), 0);
}

errcode_t ext2fs_write_new_inode(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode) {
    return write_inode_internal(fs, ino, inode, sizeof(struct ext2_inode), 1);
}

errcode_t ext2fs_inode_size_set(ext2_filsys fs, struct ext2_inode *inode, ext2_off64_t size) {
    (void)fs;
    if (!inode) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    inode->i_size = (__u32)(size & 0xFFFFFFFFULL);
    if (LINUX_S_ISREG(inode->i_mode)) {
        inode->i_size_high = (__u32)(size >> 32);
    } else {
        inode->i_size_high = 0;
    }
    return 0;
}

__u64 ext2fs_get_inode_blocks(ext2_filsys fs, const struct ext2_inode *inode) {
    __u64 blocks = inode->i_blocks;
    if (EXT2_HAS_RO_COMPAT_FEATURE(fs->super, EXT4_FEATURE_RO_COMPAT_HUGE_FILE)) {
        blocks |= ((__u64)inode->osd2.linux2.l_i_blocks_hi) << 32;
        if (inode->i_flags & EXT4_HUGE_FILE_FL) {
            blocks *= (fs->blocksize / 512);
        }
    }
    return blocks;
}

errcode_t ext2fs_iblk_add_blocks(ext2_filsys fs, struct ext2_inode *inode, blk64_t num_blocks) {
    __u64 sectors = num_blocks * (fs->blocksize / 512);
    __u64 cur = ext2fs_get_inode_blocks(fs, inode) + sectors;
    inode->i_blocks = (__u32)(cur & 0xFFFFFFFFULL);
    inode->osd2.linux2.l_i_blocks_hi = (__u16)((cur >> 32) & 0xFFFFULL);
    return 0;
}

errcode_t ext2fs_iblk_sub_blocks(ext2_filsys fs, struct ext2_inode *inode, blk64_t num_blocks) {
    __u64 sectors = num_blocks * (fs->blocksize / 512);
    __u64 cur = ext2fs_get_inode_blocks(fs, inode);
    cur = (cur > sectors) ? (cur - sectors) : 0;
    inode->i_blocks = (__u32)(cur & 0xFFFFFFFFULL);
    inode->osd2.linux2.l_i_blocks_hi = (__u16)((cur >> 32) & 0xFFFFULL);
    return 0;
}
