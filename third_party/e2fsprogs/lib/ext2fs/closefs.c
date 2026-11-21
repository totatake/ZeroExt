#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ext2fs/ext2fs.h"

static errcode_t write_primary_super(ext2_filsys fs, struct ext2_super_block *sb) {
    if (fs->blocksize == 1024) {
        return io_channel_write_blk64(fs->io, 1, 1, sb);
    } else {
        char *buf = NULL;
        errcode_t retval = ext2fs_get_mem(fs->blocksize, &buf);
        if (retval) {
            return retval;
        }
        retval = io_channel_read_blk64(fs->io, 0, 1, buf);
        if (retval == 0) {
            memcpy(buf + 1024, sb, sizeof(*sb));
            retval = io_channel_write_blk64(fs->io, 0, 1, buf);
        }
        ext2fs_free_mem(&buf);
        return retval;
    }
}

static errcode_t write_backup_super_and_bgd(ext2_filsys fs) {
    char *blk_buf = NULL;
    struct ext2_super_block *sb_copy = NULL;
    errcode_t retval;
    unsigned int alloc_sz = fs->blocksize >= sizeof(*sb_copy) ? fs->blocksize : (unsigned int)sizeof(*sb_copy);
    dgrp_t g, i;
    dgrp_t groups_per_block = (dgrp_t)(fs->blocksize / EXT2_DESC_SIZE(fs->super));
    unsigned int first_meta_bg = fs->super->s_first_meta_bg;

    retval = ext2fs_get_memzero(alloc_sz, &blk_buf);
    if (retval) {
        return retval;
    }
    sb_copy = (struct ext2_super_block *)blk_buf;

    for (g = 1; g < fs->group_desc_count; g++) {
        blk64_t group_blk;
        if (!ext2fs_bg_has_super(fs, g)) {
            continue;
        }
        group_blk = ext2fs_group_first_block2(fs, g);
        memcpy(sb_copy, fs->super, sizeof(*sb_copy));
        sb_copy->s_block_group_nr = (__u16)g;
        ext2fs_superblock_csum_set(fs, sb_copy);
        retval = io_channel_write_blk64(fs->io, group_blk, 1, blk_buf);
        if (retval) {
            break;
        }
        if (!EXT2_HAS_INCOMPAT_FEATURE(fs->super, EXT2_FEATURE_INCOMPAT_META_BG)) {
            for (i = 0; i < fs->desc_blocks; i++) {
                retval = io_channel_write_blk64(fs->io, group_blk + 1 + i, 1,
                    ((char *)fs->group_desc) + ((size_t)i * fs->blocksize));
                if (retval) {
                    goto out;
                }
            }
        } else {
            for (i = 0; i < fs->desc_blocks; i++) {
                if (i >= first_meta_bg && (i * groups_per_block == g)) {
                    retval = io_channel_write_blk64(fs->io, group_blk + 1, 1,
                        ((char *)fs->group_desc) + ((size_t)i * fs->blocksize));
                    if (retval) {
                        goto out;
                    }
                }
            }
        }
    }

out:
    ext2fs_free_mem(&blk_buf);
    return retval;
}

errcode_t ext2fs_flush2(ext2_filsys fs, int flags) {
    errcode_t retval = 0;
    dgrp_t i;
    dgrp_t groups_per_block;
    unsigned int first_meta_bg;
    blk64_t group_block;
    (void)flags;

    if (!fs || fs->magic != EXT2_ET_MAGIC_EXT2FS_FILSYS) {
        return EXT2_ET_MAGIC_EXT2FS_FILSYS;
    }
    if (!(fs->flags & EXT2_FLAG_RW)) {
        return 0;
    }

    if (fs->flags & (EXT2_FLAG_IB_DIRTY | EXT2_FLAG_BB_DIRTY)) {
        retval = ext2fs_write_bitmaps(fs);
        if (retval) {
            return retval;
        }
    }

    if (fs->flags & (EXT2_FLAG_DIRTY | EXT2_FLAG_CHANGED)) {
        fs->super->s_wtime = (__u32)time(NULL);
        fs->super->s_block_group_nr = 0;
        ext2fs_superblock_csum_set(fs, fs->super);

        retval = write_primary_super(fs, fs->super);
        if (retval) {
            return retval;
        }

        if (fs->group_desc) {
            for (i = 0; i < fs->group_desc_count; i++) {
                ext2fs_group_desc_csum_set(fs, i);
            }
            group_block = (blk64_t)fs->super->s_first_data_block;
            groups_per_block = (dgrp_t)(fs->blocksize / EXT2_DESC_SIZE(fs->super));
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
                retval = io_channel_write_blk64(fs->io, blk, 1,
                    ((char *)fs->group_desc) + ((size_t)i * fs->blocksize));
                if (retval) {
                    return retval;
                }
            }
            retval = write_backup_super_and_bgd(fs);
            if (retval) {
                return retval;
            }
        }
        fs->flags &= ~(EXT2_FLAG_DIRTY | EXT2_FLAG_CHANGED);
    }

    if (fs->io) {
        retval = io_channel_flush(fs->io);
    }
    return retval;
}

errcode_t ext2fs_flush(ext2_filsys fs) {
    return ext2fs_flush2(fs, 0);
}

errcode_t ext2fs_close(ext2_filsys fs) {
    return ext2fs_close_free(&fs);
}

errcode_t ext2fs_close_free(ext2_filsys *fs_ptr) {
    errcode_t retval = 0;
    ext2_filsys fs;

    if (!fs_ptr || !*fs_ptr) {
        return 0;
    }
    fs = *fs_ptr;
    if (fs->magic != EXT2_ET_MAGIC_EXT2FS_FILSYS) {
        return EXT2_ET_MAGIC_EXT2FS_FILSYS;
    }
    if (fs->flags & EXT2_FLAG_RW) {
        retval = ext2fs_flush(fs);
    }
    ext2fs_free(fs);
    *fs_ptr = NULL;
    return retval;
}
