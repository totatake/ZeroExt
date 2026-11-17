#include "config.h"
#include "ext2fs/ext2fs.h"

dgrp_t ext2fs_group_of_blk2(ext2_filsys fs, blk64_t blk) {
    if (blk < fs->super->s_first_data_block) {
        return 0;
    }
    return (dgrp_t)((blk - fs->super->s_first_data_block) / fs->super->s_blocks_per_group);
}

blk64_t ext2fs_group_first_block2(ext2_filsys fs, dgrp_t group) {
    return (blk64_t)fs->super->s_first_data_block + ((blk64_t)group * fs->super->s_blocks_per_group);
}

blk64_t ext2fs_group_last_block2(ext2_filsys fs, dgrp_t group) {
    blk64_t total = ext2fs_blocks_count(fs->super);
    blk64_t last = ext2fs_group_first_block2(fs, group) + fs->super->s_blocks_per_group - 1;
    if (last >= total) {
        last = total - 1;
    }
    return last;
}

int ext2fs_group_blocks_count(ext2_filsys fs, dgrp_t group) {
    blk64_t first = ext2fs_group_first_block2(fs, group);
    blk64_t last = ext2fs_group_last_block2(fs, group);
    if (last < first) {
        return 0;
    }
    return (int)(last - first + 1);
}

dgrp_t ext2fs_group_of_ino(ext2_filsys fs, ext2_ino_t ino) {
    if (ino == 0) {
        return 0;
    }
    return (ino - 1) / fs->super->s_inodes_per_group;
}

struct ext2_group_desc *ext2fs_group_desc(ext2_filsys fs, struct opaque_ext2_group_desc *gdp, dgrp_t group) {
    size_t desc_size = EXT2_DESC_SIZE(fs->super);
    return (struct ext2_group_desc *)(((char *)gdp) + ((size_t)group * desc_size));
}

struct ext4_group_desc *ext4fs_group_desc(ext2_filsys fs, struct opaque_ext2_group_desc *gdp, dgrp_t group) {
    return (struct ext4_group_desc *)ext2fs_group_desc(fs, gdp, group);
}

blk64_t ext2fs_block_bitmap_loc(ext2_filsys fs, dgrp_t group) {
    struct ext4_group_desc *gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    blk64_t ret = gdp->bg_block_bitmap;
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        ret |= ((blk64_t)gdp->bg_block_bitmap_hi) << 32;
    }
    return ret;
}

void ext2fs_block_bitmap_loc_set(ext2_filsys fs, dgrp_t group, blk64_t blk) {
    struct ext4_group_desc *gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    gdp->bg_block_bitmap = (__u32)(blk & 0xFFFFFFFFULL);
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        gdp->bg_block_bitmap_hi = (__u32)(blk >> 32);
    }
}

blk64_t ext2fs_inode_bitmap_loc(ext2_filsys fs, dgrp_t group) {
    struct ext4_group_desc *gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    blk64_t ret = gdp->bg_inode_bitmap;
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        ret |= ((blk64_t)gdp->bg_inode_bitmap_hi) << 32;
    }
    return ret;
}

void ext2fs_inode_bitmap_loc_set(ext2_filsys fs, dgrp_t group, blk64_t blk) {
    struct ext4_group_desc *gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    gdp->bg_inode_bitmap = (__u32)(blk & 0xFFFFFFFFULL);
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        gdp->bg_inode_bitmap_hi = (__u32)(blk >> 32);
    }
}

blk64_t ext2fs_inode_table_loc(ext2_filsys fs, dgrp_t group) {
    struct ext4_group_desc *gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    blk64_t ret = gdp->bg_inode_table;
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        ret |= ((blk64_t)gdp->bg_inode_table_hi) << 32;
    }
    return ret;
}

void ext2fs_inode_table_loc_set(ext2_filsys fs, dgrp_t group, blk64_t blk) {
    struct ext4_group_desc *gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    gdp->bg_inode_table = (__u32)(blk & 0xFFFFFFFFULL);
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        gdp->bg_inode_table_hi = (__u32)(blk >> 32);
    }
}

__u32 ext2fs_bg_free_blocks_count(ext2_filsys fs, dgrp_t group) {
    struct ext4_group_desc *gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    __u32 ret = gdp->bg_free_blocks_count;
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        ret |= ((__u32)gdp->bg_free_blocks_count_hi) << 16;
    }
    return ret;
}

void ext2fs_bg_free_blocks_count_set(ext2_filsys fs, dgrp_t group, __u32 n) {
    struct ext4_group_desc *gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    gdp->bg_free_blocks_count = (__u16)(n & 0xFFFFU);
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        gdp->bg_free_blocks_count_hi = (__u16)(n >> 16);
    }
}

__u32 ext2fs_bg_free_inodes_count(ext2_filsys fs, dgrp_t group) {
    struct ext4_group_desc *gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    __u32 ret = gdp->bg_free_inodes_count;
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        ret |= ((__u32)gdp->bg_free_inodes_count_hi) << 16;
    }
    return ret;
}

void ext2fs_bg_free_inodes_count_set(ext2_filsys fs, dgrp_t group, __u32 n) {
    struct ext4_group_desc *gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    gdp->bg_free_inodes_count = (__u16)(n & 0xFFFFU);
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        gdp->bg_free_inodes_count_hi = (__u16)(n >> 16);
    }
}

__u32 ext2fs_bg_used_dirs_count(ext2_filsys fs, dgrp_t group) {
    struct ext4_group_desc *gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    __u32 ret = gdp->bg_used_dirs_count;
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        ret |= ((__u32)gdp->bg_used_dirs_count_hi) << 16;
    }
    return ret;
}

void ext2fs_bg_used_dirs_count_set(ext2_filsys fs, dgrp_t group, __u32 n) {
    struct ext4_group_desc *gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    gdp->bg_used_dirs_count = (__u16)(n & 0xFFFFU);
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        gdp->bg_used_dirs_count_hi = (__u16)(n >> 16);
    }
}

__u16 ext2fs_bg_flags(ext2_filsys fs, dgrp_t group) {
    return ext4fs_group_desc(fs, fs->group_desc, group)->bg_flags;
}

void ext2fs_bg_flags_set(ext2_filsys fs, dgrp_t group, __u16 flags) {
    ext4fs_group_desc(fs, fs->group_desc, group)->bg_flags |= flags;
}

void ext2fs_bg_flags_clear(ext2_filsys fs, dgrp_t group, __u16 flags) {
    ext4fs_group_desc(fs, fs->group_desc, group)->bg_flags &= (__u16)(~flags);
}

static int test_root(dgrp_t a, int b) {
    unsigned int num = b;
    if (a == 0) {
        return 1;
    }
    while (1) {
        if (a == 1) {
            return 1;
        }
        if (a % num) {
            return 0;
        }
        a /= num;
    }
}

int ext2fs_bg_has_super(ext2_filsys fs, dgrp_t group) {
    if (group == 0) {
        return 1;
    }
    if (EXT2_HAS_RO_COMPAT_FEATURE(fs->super, EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER)) {
        if (group == 1) {
            return 1;
        }
        if (!(group & 1)) {
            return 0;
        }
        return test_root(group, 3) || test_root(group, 5) || test_root(group, 7);
    }
    return 1;
}
