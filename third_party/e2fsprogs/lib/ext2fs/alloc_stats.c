#include "config.h"
#include "ext2fs/ext2fs.h"

void ext2fs_block_alloc_stats2(ext2_filsys fs, blk64_t blk, int inuse) {
    dgrp_t group;
    __u32 gfree;
    blk64_t sfree;

    if (!fs || blk < fs->super->s_first_data_block || blk >= fs->blocks_count) {
        return;
    }
    group = ext2fs_group_of_blk2(fs, blk);
    if (inuse > 0) {
        ext2fs_mark_block_bitmap2(fs->block_map, blk);
    } else {
        ext2fs_unmark_block_bitmap2(fs->block_map, blk);
    }

    gfree = ext2fs_bg_free_blocks_count(fs, group);
    if (inuse > 0 && gfree > 0) {
        gfree--;
    } else if (inuse < 0) {
        gfree++;
    }
    ext2fs_bg_free_blocks_count_set(fs, group, gfree);
    ext2fs_bg_flags_clear(fs, group, EXT2_BG_BLOCK_UNINIT);
    ext2fs_group_desc_csum_set(fs, group);

    sfree = ext2fs_free_blocks_count(fs->super);
    if (inuse > 0 && sfree > 0) {
        sfree--;
    } else if (inuse < 0) {
        sfree++;
    }
    ext2fs_free_blocks_count_set(fs->super, sfree);
    ext2fs_mark_super_dirty(fs);
    ext2fs_mark_bb_dirty(fs);
}

void ext2fs_inode_alloc_stats2(ext2_filsys fs, ext2_ino_t ino, int inuse, int isdir) {
    dgrp_t group;
    __u32 gfree, gdirs;

    if (!fs || ino == 0 || ino > fs->super->s_inodes_count) {
        return;
    }
    group = ext2fs_group_of_ino(fs, ino);
    if (inuse > 0) {
        ext2fs_mark_inode_bitmap2(fs->inode_map, ino);
    } else {
        ext2fs_unmark_inode_bitmap2(fs->inode_map, ino);
    }

    gfree = ext2fs_bg_free_inodes_count(fs, group);
    if (inuse > 0 && gfree > 0) {
        gfree--;
    } else if (inuse < 0) {
        gfree++;
    }
    ext2fs_bg_free_inodes_count_set(fs, group, gfree);

    if (isdir) {
        gdirs = ext2fs_bg_used_dirs_count(fs, group);
        if (inuse > 0) {
            gdirs++;
        } else if (inuse < 0 && gdirs > 0) {
            gdirs--;
        }
        ext2fs_bg_used_dirs_count_set(fs, group, gdirs);
    }

    ext2fs_bg_flags_clear(fs, group, EXT2_BG_INODE_UNINIT);
    ext2fs_group_desc_csum_set(fs, group);

    if (inuse > 0 && fs->super->s_free_inodes_count > 0) {
        fs->super->s_free_inodes_count--;
    } else if (inuse < 0) {
        fs->super->s_free_inodes_count++;
    }
    ext2fs_mark_super_dirty(fs);
    ext2fs_mark_ib_dirty(fs);
}
