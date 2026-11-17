#include "config.h"
#include "ext2fs/ext2fs.h"

errcode_t ext2fs_reserve_super_and_bgd(ext2_filsys fs, dgrp_t group, ext2fs_block_bitmap bmap) {
    blk64_t group_blk, blk;
    dgrp_t i;
    int has_super;

    if (!fs || !bmap) {
        return EXT2_ET_INVALID_ARGUMENT;
    }

    has_super = ext2fs_bg_has_super(fs, group);
    if (has_super) {
        group_blk = ext2fs_group_first_block2(fs, group);
        ext2fs_mark_block_bitmap2(bmap, group_blk);
        for (i = 0; i < fs->desc_blocks; i++) {
            blk64_t gd_blk = group_blk + 1 + i;
            if (gd_blk < fs->blocks_count) {
                ext2fs_mark_block_bitmap2(bmap, gd_blk);
            }
        }
        for (i = 0; i < fs->super->s_reserved_gdt_blocks; i++) {
            blk64_t r_blk = group_blk + 1 + fs->desc_blocks + i;
            if (r_blk <= ext2fs_group_last_block2(fs, group)) {
                ext2fs_mark_block_bitmap2(bmap, r_blk);
            }
        }
    }

    blk = ext2fs_block_bitmap_loc(fs, group);
    if (blk >= fs->super->s_first_data_block && blk < fs->blocks_count) {
        ext2fs_mark_block_bitmap2(bmap, blk);
    }
    blk = ext2fs_inode_bitmap_loc(fs, group);
    if (blk >= fs->super->s_first_data_block && blk < fs->blocks_count) {
        ext2fs_mark_block_bitmap2(bmap, blk);
    }
    blk = ext2fs_inode_table_loc(fs, group);
    if (blk >= fs->super->s_first_data_block && blk < fs->blocks_count) {
        unsigned int j;
        for (j = 0; j < fs->inode_blocks_per_group; j++) {
            if (blk + j < fs->blocks_count) {
                ext2fs_mark_block_bitmap2(bmap, blk + j);
            }
        }
    }
    return 0;
}
