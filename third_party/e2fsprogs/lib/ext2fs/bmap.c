#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "ext2fs/ext2fs.h"

static errcode_t bmap_indirect(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode,
                               char *block_buf, int bmap_flags, blk64_t block, blk64_t *phys_blk) {
    unsigned int addr_per_block = fs->blocksize / sizeof(__u32);
    __u32 *buf = (__u32 *)block_buf;
    errcode_t retval;
    blk64_t goal = ext2fs_find_inode_goal(fs, ino, inode, block);

    *phys_blk = 0;
    if (block < EXT2_NDIR_BLOCKS) {
        if (inode->i_block[block] == 0 && (bmap_flags & BMAP_ALLOC)) {
            blk64_t nb = 0;
            retval = ext2fs_alloc_block2(fs, goal, NULL, &nb);
            if (retval) return retval;
            if (bmap_flags & BMAP_ZERO) {
                ext2fs_zero_blocks2(fs, nb, 1, NULL, NULL);
            }
            inode->i_block[block] = (__u32)nb;
            ext2fs_iblk_add_blocks(fs, inode, 1);
        }
        *phys_blk = inode->i_block[block];
        return 0;
    }

    block -= EXT2_NDIR_BLOCKS;
    if (block < addr_per_block) {
        blk_t ind_blk = inode->i_block[EXT2_IND_BLOCK];
        if (!ind_blk) {
            if (!(bmap_flags & BMAP_ALLOC)) return 0;
            blk64_t nb = 0;
            retval = ext2fs_alloc_block2(fs, goal, block_buf, &nb);
            if (retval) return retval;
            ind_blk = (blk_t)nb;
            inode->i_block[EXT2_IND_BLOCK] = ind_blk;
            ext2fs_iblk_add_blocks(fs, inode, 1);
        }
        retval = ext2fs_read_ind_block(fs, ind_blk, buf);
        if (retval) return retval;
        if (buf[block] == 0 && (bmap_flags & BMAP_ALLOC)) {
            blk64_t nb = 0;
            retval = ext2fs_alloc_block2(fs, goal, NULL, &nb);
            if (retval) return retval;
            if (bmap_flags & BMAP_ZERO) {
                ext2fs_zero_blocks2(fs, nb, 1, NULL, NULL);
            }
            buf[block] = (__u32)nb;
            retval = ext2fs_write_ind_block(fs, ind_blk, buf);
            if (retval) return retval;
            ext2fs_iblk_add_blocks(fs, inode, 1);
        }
        *phys_blk = buf[block];
        return 0;
    }

    block -= addr_per_block;
    if (block < (__u64)addr_per_block * addr_per_block) {
        blk_t dind_blk = inode->i_block[EXT2_DIND_BLOCK];
        __u32 idx1 = (__u32)(block / addr_per_block);
        __u32 idx2 = (__u32)(block % addr_per_block);
        blk_t ind_blk;

        if (!dind_blk) {
            if (!(bmap_flags & BMAP_ALLOC)) return 0;
            blk64_t nb = 0;
            retval = ext2fs_alloc_block2(fs, goal, block_buf, &nb);
            if (retval) return retval;
            dind_blk = (blk_t)nb;
            inode->i_block[EXT2_DIND_BLOCK] = dind_blk;
            ext2fs_iblk_add_blocks(fs, inode, 1);
        }
        retval = ext2fs_read_ind_block(fs, dind_blk, buf);
        if (retval) return retval;
        ind_blk = buf[idx1];
        if (!ind_blk) {
            blk64_t nb = 0;
            char *tmp = NULL;
            if (!(bmap_flags & BMAP_ALLOC)) return 0;
            retval = ext2fs_get_mem(fs->blocksize, &tmp);
            if (retval) return retval;
            retval = ext2fs_alloc_block2(fs, goal, tmp, &nb);
            ext2fs_free_mem(&tmp);
            if (retval) return retval;
            ind_blk = (blk_t)nb;
            buf[idx1] = ind_blk;
            retval = ext2fs_write_ind_block(fs, dind_blk, buf);
            if (retval) return retval;
            ext2fs_iblk_add_blocks(fs, inode, 1);
        }
        retval = ext2fs_read_ind_block(fs, ind_blk, buf);
        if (retval) return retval;
        if (buf[idx2] == 0 && (bmap_flags & BMAP_ALLOC)) {
            blk64_t nb = 0;
            retval = ext2fs_alloc_block2(fs, goal, NULL, &nb);
            if (retval) return retval;
            if (bmap_flags & BMAP_ZERO) {
                ext2fs_zero_blocks2(fs, nb, 1, NULL, NULL);
            }
            buf[idx2] = (__u32)nb;
            retval = ext2fs_write_ind_block(fs, ind_blk, buf);
            if (retval) return retval;
            ext2fs_iblk_add_blocks(fs, inode, 1);
        }
        *phys_blk = buf[idx2];
        return 0;
    }

    return EXT2_ET_FILE_TOO_BIG;
}

errcode_t ext2fs_bmap2(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode,
                       char *block_buf, int bmap_flags, blk64_t block,
                       int *ret_flags, blk64_t *phys_blk) {
    struct ext2_inode inode_local;
    char *local_buf = NULL;
    errcode_t retval = 0;

    if (!fs || !phys_blk) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    *phys_blk = 0;
    if (ret_flags) *ret_flags = 0;

    if (!inode) {
        retval = ext2fs_read_inode(fs, ino, &inode_local);
        if (retval) return retval;
        inode = &inode_local;
    }

    if (inode->i_flags & EXT4_EXTENTS_FL) {
        ext2_extent_handle_t handle = NULL;
        struct ext2fs_extent extent;

        retval = ext2fs_extent_open2(fs, ino, inode, &handle);
        if (retval) return retval;

        retval = ext2fs_extent_goto(handle, block);
        if (retval == 0) {
            retval = ext2fs_extent_get(handle, EXT2_EXTENT_CURRENT, &extent);
            if (retval == 0 && block >= extent.e_lblk && block < extent.e_lblk + extent.e_len) {
                *phys_blk = extent.e_pblk + (block - extent.e_lblk);
                if (ret_flags && (extent.e_flags & EXT2_EXTENT_FLAGS_UNINIT)) {
                    *ret_flags |= BMAP_RET_UNINIT;
                }
                ext2fs_extent_free(handle);
                return 0;
            }
        }
        if (!(bmap_flags & BMAP_ALLOC)) {
            ext2fs_extent_free(handle);
            *phys_blk = 0;
            return 0;
        }
        {
            blk64_t goal = ext2fs_find_inode_goal(fs, ino, inode, block);
            blk64_t nb = 0;
            retval = ext2fs_alloc_block2(fs, goal, NULL, &nb);
            if (retval == 0) {
                if (bmap_flags & BMAP_ZERO) {
                    ext2fs_zero_blocks2(fs, nb, 1, NULL, NULL);
                }
                retval = ext2fs_extent_set_bmap(handle, block, nb, 0);
                if (retval == 0) {
                    ext2fs_iblk_add_blocks(fs, inode, 1);
                    *phys_blk = nb;
                } else {
                    ext2fs_block_alloc_stats2(fs, nb, -1);
                }
            }
        }
        ext2fs_extent_free(handle);
        if (retval == 0 && (bmap_flags & BMAP_ALLOC)) {
            retval = ext2fs_write_inode(fs, ino, inode);
        }
        return retval;
    }

    if (!block_buf) {
        retval = ext2fs_get_mem(fs->blocksize, &local_buf);
        if (retval) return retval;
        block_buf = local_buf;
    }
    retval = bmap_indirect(fs, ino, inode, block_buf, bmap_flags, block, phys_blk);
    if (retval == 0 && (bmap_flags & BMAP_ALLOC)) {
        retval = ext2fs_write_inode(fs, ino, inode);
    }
    if (local_buf) {
        ext2fs_free_mem(&local_buf);
    }
    return retval;
}
