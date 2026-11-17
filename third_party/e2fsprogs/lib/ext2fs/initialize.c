#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ext2fs/ext2fs.h"

static void init_deterministic_uuid(__u8 uuid[16]) {
    __u32 now = (__u32)time(NULL);
    int i;
    for (i = 0; i < 16; i++) {
        uuid[i] = (__u8)((now >> ((i % 4) * 8)) ^ (i * 37 + 0x5A));
    }
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
}

errcode_t ext2fs_initialize(const char *name, int flags,
                            struct ext2_super_block *param,
                            io_manager manager, ext2_filsys *ret_fs) {
    ext2_filsys fs = NULL;
    errcode_t retval;
    struct ext2_super_block *sb;
    blk64_t blocks_count;
    unsigned int blocksize;
    unsigned int inode_size;
    unsigned int blocks_per_group;
    unsigned int inodes_per_group;
    dgrp_t i;
    size_t desc_size;
    dgrp_t groups_per_block;
    int io_flags = IO_FLAG_RW;

    if (!name || !param || !manager || !ret_fs) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    initialize_ext2_error_table();

    retval = ext2fs_get_memzero(sizeof(struct struct_ext2_filsys), &fs);
    if (retval) {
        return retval;
    }
    fs->magic = EXT2_ET_MAGIC_EXT2FS_FILSYS;
    fs->flags = flags | EXT2_FLAG_RW;

    retval = ext2fs_get_mem((unsigned long)strlen(name) + 1, &fs->device_name);
    if (retval) {
        goto cleanup;
    }
    strcpy(fs->device_name, name);

    retval = io_channel_open(manager, name, io_flags, &fs->io);
    if (retval) {
        goto cleanup;
    }

    retval = ext2fs_get_memzero(sizeof(struct ext2_super_block), &fs->super);
    if (retval) {
        goto cleanup;
    }
    sb = fs->super;
    memcpy(sb, param, sizeof(*sb));

    sb->s_magic = EXT2_SUPER_MAGIC;
    sb->s_rev_level = EXT2_DYNAMIC_REV;
    sb->s_state = EXT2_VALID_FS;
    sb->s_errors = 1; /* EXT2_ERRORS_CONTINUE */
    sb->s_first_ino = EXT2_GOOD_OLD_FIRST_INO;
    if (sb->s_inode_size == 0) {
        sb->s_inode_size = 256;
    }
    inode_size = sb->s_inode_size;
    if (inode_size > EXT2_GOOD_OLD_INODE_SIZE && sb->s_min_extra_isize == 0) {
        sb->s_min_extra_isize = 32;
        sb->s_want_extra_isize = 32;
    }

    blocksize = EXT2_BLOCK_SIZE(sb);
    fs->blocksize = blocksize;
    fs->fragsize = blocksize;
    sb->s_first_data_block = (blocksize == 1024) ? 1 : 0;
    sb->s_log_cluster_size = sb->s_log_block_size;

    if (sb->s_blocks_per_group == 0) {
        sb->s_blocks_per_group = blocksize * 8;
    }
    blocks_per_group = sb->s_blocks_per_group;
    sb->s_clusters_per_group = blocks_per_group;

    blocks_count = ext2fs_blocks_count(sb);
    fs->blocks_count = blocks_count;

    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        if (sb->s_desc_size < EXT2_MIN_DESC_SIZE_64BIT) {
            sb->s_desc_size = EXT2_MIN_DESC_SIZE_64BIT;
        }
    } else if (sb->s_desc_size == 0) {
        sb->s_desc_size = EXT2_MIN_DESC_SIZE;
    }

    {
        blk64_t data_blocks = blocks_count - sb->s_first_data_block;
        fs->group_desc_count = (dgrp_t)((data_blocks + blocks_per_group - 1) / blocks_per_group);
    }
    if (fs->group_desc_count == 0) {
        fs->group_desc_count = 1;
    }

    if (sb->s_inodes_per_group == 0) {
        blk64_t avg_group_blocks = blocks_count / fs->group_desc_count;
        __u64 target_inodes = (avg_group_blocks * blocksize) / 16384;
        unsigned int ipb = blocksize / inode_size;
        if (target_inodes < 64) {
            target_inodes = 64;
        }
        if (target_inodes > blocks_per_group) {
            target_inodes = blocks_per_group;
        }
        inodes_per_group = (unsigned int)(((target_inodes + ipb - 1) / ipb) * ipb);
        if (inodes_per_group < ipb) {
            inodes_per_group = ipb;
        }
        sb->s_inodes_per_group = inodes_per_group;
    }
    inodes_per_group = sb->s_inodes_per_group;
    sb->s_inodes_count = inodes_per_group * fs->group_desc_count;
    sb->s_free_inodes_count = sb->s_inodes_count;
    sb->s_mkfs_time = (__u32)time(NULL);
    sb->s_wtime = sb->s_mkfs_time;
    sb->s_max_mnt_count = -1;

    {
        int all_zero = 1;
        for (i = 0; i < 16; i++) {
            if (sb->s_uuid[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        if (all_zero) {
            init_deterministic_uuid(sb->s_uuid);
        }
    }

    ext2fs_init_csum_seed(fs);

    retval = io_channel_set_blksize(fs->io, (int)fs->blocksize);
    if (retval) {
        goto cleanup;
    }

    fs->inode_blocks_per_group = (inodes_per_group * inode_size + blocksize - 1) / blocksize;
    desc_size = EXT2_DESC_SIZE(sb);
    groups_per_block = (dgrp_t)(blocksize / desc_size);
    fs->desc_blocks = (unsigned long)((fs->group_desc_count + groups_per_block - 1) / groups_per_block);

    retval = ext2fs_get_memzero((unsigned long)(fs->desc_blocks * blocksize), &fs->group_desc);
    if (retval) {
        goto cleanup;
    }

    retval = ext2fs_allocate_block_bitmap(fs, "block bitmap", &fs->block_map);
    if (retval) {
        goto cleanup;
    }
    retval = ext2fs_allocate_inode_bitmap(fs, "inode bitmap", &fs->inode_map);
    if (retval) {
        goto cleanup;
    }

    ext2fs_free_blocks_count_set(sb, 0);
    for (i = 0; i < fs->group_desc_count; i++) {
        int gblocks = ext2fs_group_blocks_count(fs, i);
        ext2fs_bg_free_blocks_count_set(fs, i, (__u32)gblocks);
        ext2fs_bg_free_inodes_count_set(fs, i, inodes_per_group);
        ext2fs_bg_used_dirs_count_set(fs, i, 0);
        ext2fs_free_blocks_count_set(sb, ext2fs_free_blocks_count(sb) + (__u64)gblocks);
    }

    /* Mark reserved inodes 1..s_first_ino-1? Allocated in root/mkfs step, keep bitmap clean here */
    ext2fs_mark_super_dirty(fs);
    ext2fs_mark_ib_dirty(fs);
    ext2fs_mark_bb_dirty(fs);

    *ret_fs = fs;
    return 0;

cleanup:
    ext2fs_free(fs);
    return retval;
}
