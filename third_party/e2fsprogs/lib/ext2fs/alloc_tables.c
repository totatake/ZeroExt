#include "config.h"
#include <string.h>
#include "ext2fs/ext2fs.h"

errcode_t ext2fs_allocate_tables(ext2_filsys fs) {
    dgrp_t i;
    blk64_t total_free = 0;
    char *zero_buf = NULL;
    errcode_t retval;

    if (!fs) {
        return EXT2_ET_INVALID_ARGUMENT;
    }

    retval = ext2fs_get_memzero(fs->blocksize, &zero_buf);
    if (retval) {
        return retval;
    }

    for (i = 0; i < fs->group_desc_count; i++) {
        blk64_t group_start = ext2fs_group_first_block2(fs, i);
        blk64_t group_end = ext2fs_group_last_block2(fs, i);
        blk64_t cur = group_start;
        blk64_t bb_blk, ib_blk, it_blk;
        unsigned int j;
        __u32 free_blocks = 0;

        if (ext2fs_bg_has_super(fs, i)) {
            cur += 1 + fs->desc_blocks + fs->super->s_reserved_gdt_blocks;
        }
        bb_blk = cur++;
        ib_blk = cur++;
        it_blk = cur;
        cur += fs->inode_blocks_per_group;

        if (cur - 1 > group_end) {
            ext2fs_free_mem(&zero_buf);
            return EXT2_ET_NO_FREE_SPACE;
        }

        ext2fs_block_bitmap_loc_set(fs, i, bb_blk);
        ext2fs_inode_bitmap_loc_set(fs, i, ib_blk);
        ext2fs_inode_table_loc_set(fs, i, it_blk);
        ext2fs_bg_flags_clear(fs, i, EXT2_BG_BLOCK_UNINIT | EXT2_BG_INODE_UNINIT);
        ext2fs_bg_flags_set(fs, i, EXT2_BG_INODE_ZEROED);

        /* Zero inode table blocks on disk */
        for (j = 0; j < fs->inode_blocks_per_group; j++) {
            retval = io_channel_write_blk64(fs->io, it_blk + j, 1, zero_buf);
            if (retval) {
                ext2fs_free_mem(&zero_buf);
                return retval;
            }
        }

        ext2fs_reserve_super_and_bgd(fs, i, fs->block_map);

        {
            blk64_t b;
            for (b = group_start; b <= group_end; b++) {
                if (!ext2fs_test_block_bitmap2(fs->block_map, b)) {
                    free_blocks++;
                }
            }
        }
        ext2fs_bg_free_blocks_count_set(fs, i, free_blocks);
        total_free += free_blocks;
        ext2fs_group_desc_csum_set(fs, i);
    }

    ext2fs_free_blocks_count_set(fs->super, total_free);
    ext2fs_mark_super_dirty(fs);
    ext2fs_mark_bb_dirty(fs);
    ext2fs_mark_ib_dirty(fs);
    ext2fs_free_mem(&zero_buf);
    return 0;
}
