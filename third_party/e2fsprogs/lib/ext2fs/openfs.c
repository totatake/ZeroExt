#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "ext2fs/ext2fs.h"

#define SUPERBLOCK_OFFSET 1024
#define SUPERBLOCK_SIZE   1024

#define EXT2_SUPPORTED_INCOMPAT \
    (EXT2_FEATURE_INCOMPAT_FILETYPE | \
     EXT3_FEATURE_INCOMPAT_RECOVER | \
     EXT2_FEATURE_INCOMPAT_META_BG | \
     EXT4_FEATURE_INCOMPAT_EXTENTS | \
     EXT4_FEATURE_INCOMPAT_64BIT | \
     EXT4_FEATURE_INCOMPAT_FLEX_BG | \
     EXT4_FEATURE_INCOMPAT_CSUM_SEED | \
     EXT4_FEATURE_INCOMPAT_LARGEDIR | \
     EXT4_FEATURE_INCOMPAT_INLINE_DATA)

#define EXT2_SUPPORTED_RO_COMPAT \
    (EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | \
     EXT2_FEATURE_RO_COMPAT_LARGE_FILE | \
     EXT2_FEATURE_RO_COMPAT_BTREE_DIR | \
     EXT4_FEATURE_RO_COMPAT_HUGE_FILE | \
     EXT4_FEATURE_RO_COMPAT_GDT_CSUM | \
     EXT4_FEATURE_RO_COMPAT_DIR_NLINK | \
     EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE | \
     EXT4_FEATURE_RO_COMPAT_METADATA_CSUM | \
     EXT4_FEATURE_RO_COMPAT_READONLY | \
     EXT4_FEATURE_RO_COMPAT_PROJECT)

errcode_t ext2fs_open(const char *name, int flags, int superblock,
                      unsigned int block_size, io_manager manager,
                      ext2_filsys *ret_fs) {
    return ext2fs_open2(name, NULL, flags, superblock, block_size, manager, ret_fs);
}

errcode_t ext2fs_open2(const char *name, const char *io_options, int flags,
                       int superblock, unsigned int block_size,
                       io_manager manager, ext2_filsys *ret_fs) {
    ext2_filsys fs = NULL;
    errcode_t retval;
    int io_flags = 0;
    char *block_buf = NULL;
    blk64_t group_block;
    dgrp_t groups_per_block;
    unsigned int first_meta_bg;
    size_t desc_size;
    dgrp_t i;

    if (!name || !manager || !ret_fs) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    initialize_ext2_error_table();

    retval = ext2fs_get_memzero(sizeof(struct struct_ext2_filsys), &fs);
    if (retval) {
        return retval;
    }
    fs->magic = EXT2_ET_MAGIC_EXT2FS_FILSYS;
    fs->flags = flags;

    retval = ext2fs_get_mem((unsigned long)strlen(name) + 1, &fs->device_name);
    if (retval) {
        goto cleanup;
    }
    strcpy(fs->device_name, name);

    if (flags & EXT2_FLAG_RW) {
        io_flags |= IO_FLAG_RW;
    }
    if (flags & EXT2_FLAG_EXCLUSIVE) {
        io_flags |= IO_FLAG_EXCLUSIVE;
    }
    retval = io_channel_open(manager, name, io_flags, &fs->io);
    if (retval) {
        goto cleanup;
    }
    if (io_options && *io_options) {
        retval = io_channel_set_options(fs->io, io_options);
        if (retval) {
            goto cleanup;
        }
    }

    /* Read superblock using 1024-byte granularity initially */
    retval = io_channel_set_blksize(fs->io, SUPERBLOCK_SIZE);
    if (retval) {
        goto cleanup;
    }

    retval = ext2fs_get_mem(SUPERBLOCK_SIZE, &fs->super);
    if (retval) {
        goto cleanup;
    }

    if (superblock) {
        if (!block_size) {
            block_size = SUPERBLOCK_SIZE;
        }
        retval = io_channel_read_blk64(fs->io, ((blk64_t)superblock * block_size) / SUPERBLOCK_SIZE, 1, fs->super);
    } else {
        retval = io_channel_read_blk64(fs->io, 1, 1, fs->super);
    }
    if (retval) {
        goto cleanup;
    }

    if (fs->super->s_magic != EXT2_SUPER_MAGIC) {
        retval = EXT2_ET_BAD_MAGIC;
        goto cleanup;
    }
    if (fs->super->s_rev_level > EXT2_DYNAMIC_REV) {
        retval = EXT2_ET_REV_TOO_HIGH;
        goto cleanup;
    }
    if (fs->super->s_log_block_size > (EXT2_MAX_BLOCK_LOG_SIZE - EXT2_MIN_BLOCK_LOG_SIZE)) {
        retval = EXT2_ET_UNEXPECTED_BLOCK_SIZE;
        goto cleanup;
    }

    fs->blocksize = EXT2_BLOCK_SIZE(fs->super);
    fs->fragsize = fs->blocksize;
    if (block_size && block_size != fs->blocksize) {
        retval = EXT2_ET_UNEXPECTED_BLOCK_SIZE;
        goto cleanup;
    }
    if (fs->super->s_blocks_per_group == 0 || fs->super->s_inodes_per_group == 0) {
        retval = EXT2_ET_UNEXPECTED_BLOCK_SIZE;
        goto cleanup;
    }

    /* Check unsupported features unless EXT2_FLAG_FORCE is specified */
    if (!(flags & EXT2_FLAG_FORCE)) {
        __u32 unsupported_incompat = fs->super->s_feature_incompat & ~EXT2_SUPPORTED_INCOMPAT;
        if (unsupported_incompat) {
            retval = EXT2_ET_UNSUPP_FEATURE;
            goto cleanup;
        }
        if ((flags & EXT2_FLAG_RW) && (fs->super->s_feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER)) {
            retval = EXT2_ET_JOURNAL_UNSUPP_INCOMPAT;
            goto cleanup;
        }
        if ((flags & EXT2_FLAG_RW) && (fs->super->s_feature_ro_compat & ~EXT2_SUPPORTED_RO_COMPAT)) {
            retval = EXT2_ET_RO_UNSUPP_FEATURE;
            goto cleanup;
        }
    }

    ext2fs_init_csum_seed(fs);
    if (!(flags & EXT2_FLAG_IGNORE_CSUM_ERRORS)) {
        if (!ext2fs_superblock_csum_verify(fs, fs->super)) {
            retval = EXT2_ET_SB_CSUM_INVALID;
            goto cleanup;
        }
    }

    retval = io_channel_set_blksize(fs->io, (int)fs->blocksize);
    if (retval) {
        goto cleanup;
    }

    fs->blocks_count = ext2fs_blocks_count(fs->super);
    fs->inode_blocks_per_group =
        (fs->super->s_inodes_per_group * EXT2_INODE_SIZE(fs->super) + fs->blocksize - 1) / fs->blocksize;

    {
        blk64_t data_blocks = fs->blocks_count - fs->super->s_first_data_block;
        fs->group_desc_count = (dgrp_t)((data_blocks + fs->super->s_blocks_per_group - 1) / fs->super->s_blocks_per_group);
    }
    if (fs->group_desc_count == 0) {
        fs->group_desc_count = 1;
    }

    desc_size = EXT2_DESC_SIZE(fs->super);
    groups_per_block = (dgrp_t)(fs->blocksize / desc_size);
    fs->desc_blocks = (unsigned long)((fs->group_desc_count + groups_per_block - 1) / groups_per_block);

    if (flags & EXT2_FLAG_SUPER_ONLY) {
        *ret_fs = fs;
        return 0;
    }

    retval = ext2fs_get_memzero((unsigned long)(fs->desc_blocks * fs->blocksize), &fs->group_desc);
    if (retval) {
        goto cleanup;
    }

    retval = ext2fs_get_mem(fs->blocksize, &block_buf);
    if (retval) {
        goto cleanup;
    }

    group_block = superblock ? (blk64_t)superblock : (blk64_t)fs->super->s_first_data_block;
    first_meta_bg = fs->super->s_first_meta_bg;

    for (i = 0; i < fs->desc_blocks; i++) {
        blk64_t blk;
        if (EXT2_HAS_INCOMPAT_FEATURE(fs->super, EXT2_FEATURE_INCOMPAT_META_BG) && i >= first_meta_bg) {
            dgrp_t grp = i * groups_per_block;
            blk = ext2fs_group_first_block2(fs, grp);
            if (ext2fs_bg_has_super(fs, grp)) {
                blk++;
            }
        } else {
            blk = group_block + 1 + i;
        }
        retval = io_channel_read_blk64(fs->io, blk, 1, ((char *)fs->group_desc) + ((size_t)i * fs->blocksize));
        if (retval) {
            goto cleanup;
        }
    }

    if (!(flags & EXT2_FLAG_IGNORE_CSUM_ERRORS)) {
        for (i = 0; i < fs->group_desc_count; i++) {
            if (!ext2fs_group_desc_csum_verify(fs, i)) {
                retval = EXT2_ET_GDESC_CSUM_INVALID;
                goto cleanup;
            }
        }
    }

    ext2fs_free_mem(&block_buf);
    *ret_fs = fs;
    return 0;

cleanup:
    ext2fs_free_mem(&block_buf);
    ext2fs_free(fs);
    return retval;
}
