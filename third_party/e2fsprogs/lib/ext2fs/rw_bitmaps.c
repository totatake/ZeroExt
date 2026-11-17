#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "ext2fs/ext2fs.h"

extern unsigned char *ext2fs_get_bitmap_raw_buffer(ext2fs_generic_bitmap bitmap, size_t *out_nbytes);

errcode_t ext2fs_read_block_bitmap(ext2_filsys fs) {
    errcode_t retval;
    char *buf = NULL;
    unsigned char *raw;
    size_t raw_bytes = 0;
    dgrp_t i;
    int block_nbytes;

    if (!fs) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    if (fs->block_map) {
        ext2fs_free_block_bitmap(fs->block_map);
        fs->block_map = NULL;
    }
    retval = ext2fs_allocate_block_bitmap(fs, "block bitmap", &fs->block_map);
    if (retval) {
        return retval;
    }
    raw = ext2fs_get_bitmap_raw_buffer(fs->block_map, &raw_bytes);
    block_nbytes = (int)((fs->super->s_blocks_per_group + 7) / 8);

    retval = ext2fs_get_mem(fs->blocksize, &buf);
    if (retval) {
        return retval;
    }

    for (i = 0; i < fs->group_desc_count; i++) {
        blk64_t blk = ext2fs_block_bitmap_loc(fs, i);
        size_t byte_off = ((size_t)i * fs->super->s_blocks_per_group) / 8;
        int gblocks = ext2fs_group_blocks_count(fs, i);
        int gbytes = (gblocks + 7) / 8;
        __u16 flags = ext2fs_bg_flags(fs, i);

        if (flags & EXT2_BG_BLOCK_UNINIT) {
            memset(buf, 0, fs->blocksize);
            if (byte_off + gbytes <= raw_bytes) {
                memcpy(raw + byte_off, buf, (size_t)gbytes);
            }
            /* Reserve super/bgd/bitmaps/inode table blocks in memory for uninit group */
            ext2fs_reserve_super_and_bgd(fs, i, fs->block_map);
            continue;
        }
        if (blk == 0 || blk >= fs->blocks_count) {
            retval = EXT2_ET_GDESC_BAD_BLOCK_MAP;
            break;
        }
        retval = io_channel_read_blk64(fs->io, blk, 1, buf);
        if (retval) {
            break;
        }
        if (!(fs->flags & EXT2_FLAG_IGNORE_CSUM_ERRORS)) {
            if (!ext2fs_block_bitmap_csum_verify(fs, i, buf, block_nbytes)) {
                retval = EXT2_ET_BLOCK_BITMAP_CSUM_INVALID;
                break;
            }
        }
        if (byte_off + gbytes <= raw_bytes) {
            memcpy(raw + byte_off, buf, (size_t)gbytes);
        }
    }

    ext2fs_free_mem(&buf);
    return retval;
}

errcode_t ext2fs_read_inode_bitmap(ext2_filsys fs) {
    errcode_t retval;
    char *buf = NULL;
    unsigned char *raw;
    size_t raw_bytes = 0;
    dgrp_t i;
    int inode_nbytes;

    if (!fs) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    if (fs->inode_map) {
        ext2fs_free_inode_bitmap(fs->inode_map);
        fs->inode_map = NULL;
    }
    retval = ext2fs_allocate_inode_bitmap(fs, "inode bitmap", &fs->inode_map);
    if (retval) {
        return retval;
    }
    raw = ext2fs_get_bitmap_raw_buffer(fs->inode_map, &raw_bytes);
    inode_nbytes = (int)((fs->super->s_inodes_per_group + 7) / 8);

    retval = ext2fs_get_mem(fs->blocksize, &buf);
    if (retval) {
        return retval;
    }

    for (i = 0; i < fs->group_desc_count; i++) {
        blk64_t blk = ext2fs_inode_bitmap_loc(fs, i);
        size_t byte_off = ((size_t)i * fs->super->s_inodes_per_group) / 8;
        __u16 flags = ext2fs_bg_flags(fs, i);

        if (flags & EXT2_BG_INODE_UNINIT) {
            if (byte_off + inode_nbytes <= raw_bytes) {
                memset(raw + byte_off, 0, (size_t)inode_nbytes);
            }
            continue;
        }
        if (blk == 0 || blk >= fs->blocks_count) {
            retval = EXT2_ET_GDESC_BAD_INODE_MAP;
            break;
        }
        retval = io_channel_read_blk64(fs->io, blk, 1, buf);
        if (retval) {
            break;
        }
        if (!(fs->flags & EXT2_FLAG_IGNORE_CSUM_ERRORS)) {
            if (!ext2fs_inode_bitmap_csum_verify(fs, i, buf, inode_nbytes)) {
                retval = EXT2_ET_INODE_BITMAP_CSUM_INVALID;
                break;
            }
        }
        if (byte_off + inode_nbytes <= raw_bytes) {
            memcpy(raw + byte_off, buf, (size_t)inode_nbytes);
        }
    }

    ext2fs_free_mem(&buf);
    return retval;
}

errcode_t ext2fs_read_bitmaps(ext2_filsys fs) {
    errcode_t retval;
    if (!fs) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    if (!fs->inode_map) {
        retval = ext2fs_read_inode_bitmap(fs);
        if (retval) {
            return retval;
        }
    }
    if (!fs->block_map) {
        retval = ext2fs_read_block_bitmap(fs);
        if (retval) {
            return retval;
        }
    }
    return 0;
}

errcode_t ext2fs_write_block_bitmap(ext2_filsys fs) {
    errcode_t retval = 0;
    char *buf = NULL;
    unsigned char *raw;
    size_t raw_bytes = 0;
    dgrp_t i;
    int block_nbytes;

    if (!fs || !fs->block_map) {
        return 0;
    }
    if (!(fs->flags & EXT2_FLAG_RW)) {
        return EXT2_ET_RO_FILSYS;
    }
    raw = ext2fs_get_bitmap_raw_buffer(fs->block_map, &raw_bytes);
    block_nbytes = (int)((fs->super->s_blocks_per_group + 7) / 8);

    retval = ext2fs_get_mem(fs->blocksize, &buf);
    if (retval) {
        return retval;
    }

    for (i = 0; i < fs->group_desc_count; i++) {
        blk64_t blk = ext2fs_block_bitmap_loc(fs, i);
        size_t byte_off = ((size_t)i * fs->super->s_blocks_per_group) / 8;
        int gblocks = ext2fs_group_blocks_count(fs, i);
        int gbytes = (gblocks + 7) / 8;
        int rem_bits = gblocks & 7;

        memset(buf, 0xFF, fs->blocksize);
        if (byte_off + gbytes <= raw_bytes) {
            memcpy(buf, raw + byte_off, (size_t)gbytes);
        }
        if (rem_bits && gbytes > 0) {
            buf[gbytes - 1] |= (char)(0xFF << rem_bits);
        }
        ext2fs_bg_flags_clear(fs, i, EXT2_BG_BLOCK_UNINIT);
        ext2fs_block_bitmap_csum_set(fs, i, buf, block_nbytes);
        ext2fs_group_desc_csum_set(fs, i);

        retval = io_channel_write_blk64(fs->io, blk, 1, buf);
        if (retval) {
            break;
        }
    }

    ext2fs_free_mem(&buf);
    if (retval == 0) {
        fs->flags &= ~EXT2_FLAG_BB_DIRTY;
    }
    return retval;
}

errcode_t ext2fs_write_inode_bitmap(ext2_filsys fs) {
    errcode_t retval = 0;
    char *buf = NULL;
    unsigned char *raw;
    size_t raw_bytes = 0;
    dgrp_t i;
    int inode_nbytes;

    if (!fs || !fs->inode_map) {
        return 0;
    }
    if (!(fs->flags & EXT2_FLAG_RW)) {
        return EXT2_ET_RO_FILSYS;
    }
    raw = ext2fs_get_bitmap_raw_buffer(fs->inode_map, &raw_bytes);
    inode_nbytes = (int)((fs->super->s_inodes_per_group + 7) / 8);

    retval = ext2fs_get_mem(fs->blocksize, &buf);
    if (retval) {
        return retval;
    }

    for (i = 0; i < fs->group_desc_count; i++) {
        blk64_t blk = ext2fs_inode_bitmap_loc(fs, i);
        size_t byte_off = ((size_t)i * fs->super->s_inodes_per_group) / 8;

        memset(buf, 0xFF, fs->blocksize);
        if (byte_off + inode_nbytes <= raw_bytes) {
            memcpy(buf, raw + byte_off, (size_t)inode_nbytes);
        }
        ext2fs_bg_flags_clear(fs, i, EXT2_BG_INODE_UNINIT);
        ext2fs_inode_bitmap_csum_set(fs, i, buf, inode_nbytes);
        ext2fs_group_desc_csum_set(fs, i);

        retval = io_channel_write_blk64(fs->io, blk, 1, buf);
        if (retval) {
            break;
        }
    }

    ext2fs_free_mem(&buf);
    if (retval == 0) {
        fs->flags &= ~EXT2_FLAG_IB_DIRTY;
    }
    return retval;
}

errcode_t ext2fs_write_bitmaps(ext2_filsys fs) {
    errcode_t retval = 0;
    if (fs->flags & EXT2_FLAG_BB_DIRTY) {
        retval = ext2fs_write_block_bitmap(fs);
        if (retval) {
            return retval;
        }
    }
    if (fs->flags & EXT2_FLAG_IB_DIRTY) {
        retval = ext2fs_write_inode_bitmap(fs);
        if (retval) {
            return retval;
        }
    }
    return 0;
}
