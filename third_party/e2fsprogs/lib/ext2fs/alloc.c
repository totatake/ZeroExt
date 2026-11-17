#include "config.h"
#include <string.h>
#include "ext2fs/ext2fs.h"

errcode_t ext2fs_new_inode(ext2_filsys fs, ext2_ino_t dir, int mode, ext2fs_inode_bitmap map, ext2_ino_t *ret) {
    ext2_ino_t start_ino;
    ext2_ino_t ino;
    dgrp_t start_group = 0;
    dgrp_t g;
    errcode_t retval;
    (void)mode;

    if (!fs || !ret) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    if (!map) {
        map = fs->inode_map;
    }
    if (!map) {
        return EXT2_ET_NO_INODE_BITMAP;
    }

    if (dir > 0) {
        start_group = ext2fs_group_of_ino(fs, dir);
    }

    for (g = 0; g < fs->group_desc_count; g++) {
        dgrp_t group = (start_group + g) % fs->group_desc_count;
        ext2_ino_t grp_first = group * fs->super->s_inodes_per_group + 1;
        ext2_ino_t grp_last = grp_first + fs->super->s_inodes_per_group - 1;
        if (grp_first < fs->super->s_first_ino) {
            grp_first = fs->super->s_first_ino;
        }
        if (grp_last > fs->super->s_inodes_count) {
            grp_last = fs->super->s_inodes_count;
        }
        if (grp_first > grp_last) {
            continue;
        }
        start_ino = grp_first;
        retval = ext2fs_find_first_zero_inode_bitmap2(map, start_ino, grp_last, &ino);
        if (retval == 0) {
            *ret = ino;
            return 0;
        }
    }

    return EXT2_ET_INODE_ALLOC_FAIL;
}

errcode_t ext2fs_new_block2(ext2_filsys fs, blk64_t goal, ext2fs_block_bitmap map, blk64_t *ret) {
    blk64_t start, end;
    errcode_t retval;

    if (!fs || !ret) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    if (!map) {
        map = fs->block_map;
    }
    if (!map) {
        return EXT2_ET_NO_BLOCK_BITMAP;
    }

    start = goal;
    if (start < fs->super->s_first_data_block || start >= fs->blocks_count) {
        start = fs->super->s_first_data_block;
    }
    end = fs->blocks_count - 1;

    retval = ext2fs_find_first_zero_block_bitmap2(map, start, end, ret);
    if (retval == 0) {
        return 0;
    }
    if (start > fs->super->s_first_data_block) {
        retval = ext2fs_find_first_zero_block_bitmap2(map, fs->super->s_first_data_block, start - 1, ret);
        if (retval == 0) {
            return 0;
        }
    }
    return EXT2_ET_BLOCK_ALLOC_FAIL;
}

errcode_t ext2fs_alloc_block2(ext2_filsys fs, blk64_t goal, char *block_buf, blk64_t *ret) {
    blk64_t blk = 0;
    errcode_t retval;

    retval = ext2fs_new_block2(fs, goal, fs->block_map, &blk);
    if (retval) {
        return retval;
    }
    if (block_buf) {
        memset(block_buf, 0, fs->blocksize);
        retval = io_channel_write_blk64(fs->io, blk, 1, block_buf);
        if (retval) {
            return retval;
        }
    }
    ext2fs_block_alloc_stats2(fs, blk, 1);
    *ret = blk;
    return 0;
}

blk64_t ext2fs_find_inode_goal(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode, blk64_t lblk) {
    dgrp_t group;
    (void)inode;
    (void)lblk;
    if (!fs || ino == 0) {
        return fs ? fs->super->s_first_data_block : 0;
    }
    group = ext2fs_group_of_ino(fs, ino);
    return ext2fs_group_first_block2(fs, group);
}

errcode_t ext2fs_zero_blocks2(ext2_filsys fs, blk64_t blk, int num, blk64_t *ret_blk, int *ret_count) {
    char *zero_buf = NULL;
    errcode_t retval = 0;
    int i;

    if (!fs || num <= 0) {
        return 0;
    }
    retval = ext2fs_get_memzero(fs->blocksize, &zero_buf);
    if (retval) {
        return retval;
    }
    for (i = 0; i < num; i++) {
        retval = io_channel_write_blk64(fs->io, blk + i, 1, zero_buf);
        if (retval) {
            if (ret_blk) *ret_blk = blk + i;
            if (ret_count) *ret_count = i;
            ext2fs_free_mem(&zero_buf);
            return retval;
        }
    }
    ext2fs_free_mem(&zero_buf);
    return 0;
}
