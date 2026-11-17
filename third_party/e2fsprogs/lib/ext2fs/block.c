#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "ext2fs/ext2fs.h"

static errcode_t iterate_ind(ext2_filsys fs, blk64_t *ind_blk_ptr, int level,
                             e2_blkcnt_t *logical_cnt, char *buf,
                             int (*func)(ext2_filsys, blk64_t *, e2_blkcnt_t, blk64_t, int, void *),
                             void *priv_data, int flags) {
    blk64_t ind_blk = *ind_blk_ptr;
    unsigned int addr_per_block = fs->blocksize / sizeof(__u32);
    __u32 *entries;
    unsigned int i;
    int changed = 0;
    errcode_t retval;

    if (!ind_blk) {
        blk64_t skip = 1;
        int l;
        for (l = 0; l < level; l++) skip *= addr_per_block;
        *logical_cnt += (e2_blkcnt_t)skip;
        return 0;
    }

    retval = ext2fs_get_mem(fs->blocksize, &entries);
    if (retval) return retval;

    retval = io_channel_read_blk64(fs->io, ind_blk, 1, entries);
    if (retval) {
        ext2fs_free_mem(&entries);
        return retval;
    }

    for (i = 0; i < addr_per_block; i++) {
        blk64_t child = entries[i];
        if (level == 1) {
            if (child) {
                int r = func(fs, &child, *logical_cnt, ind_blk, (int)(i * sizeof(__u32)), priv_data);
                if (r & BLOCK_CHANGED) {
                    entries[i] = (__u32)child;
                    changed = 1;
                }
                if (r & BLOCK_ABORT) {
                    if (changed && !(flags & BLOCK_FLAG_READ_ONLY)) {
                        io_channel_write_blk64(fs->io, ind_blk, 1, entries);
                    }
                    ext2fs_free_mem(&entries);
                    return 0;
                }
            }
            (*logical_cnt)++;
        } else {
            retval = iterate_ind(fs, &child, level - 1, logical_cnt, buf, func, priv_data, flags);
            if (retval) {
                ext2fs_free_mem(&entries);
                return retval;
            }
            if (child != entries[i]) {
                entries[i] = (__u32)child;
                changed = 1;
            }
        }
    }

    if (changed && !(flags & BLOCK_FLAG_READ_ONLY)) {
        retval = io_channel_write_blk64(fs->io, ind_blk, 1, entries);
    }
    if (flags & BLOCK_FLAG_DEPTH_TRAVERSE) {
        int r = func(fs, ind_blk_ptr, -1, 0, 0, priv_data);
        (void)r;
    }
    ext2fs_free_mem(&entries);
    return retval;
}

errcode_t ext2fs_block_iterate3(ext2_filsys fs, ext2_ino_t ino, int flags, char *block_buf,
                                int (*func)(ext2_filsys fs, blk64_t *blocknr, e2_blkcnt_t blockcnt,
                                            blk64_t ref_blk, int ref_offset, void *priv_data),
                                void *priv_data) {
    struct ext2_inode inode;
    errcode_t retval;
    int i, changed = 0;
    e2_blkcnt_t logical = 0;
    (void)block_buf;

    if (!fs || !func) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    retval = ext2fs_read_inode(fs, ino, &inode);
    if (retval) return retval;

    if (inode.i_flags & EXT4_EXTENTS_FL) {
        ext2_extent_handle_t handle = NULL;
        struct ext2fs_extent extent;
        retval = ext2fs_extent_open2(fs, ino, &inode, &handle);
        if (retval) return retval;

        retval = ext2fs_extent_get(handle, EXT2_EXTENT_ROOT, &extent);
        while (retval == 0) {
            if (extent.e_flags & EXT2_EXTENT_FLAGS_LEAF) {
                __u32 k;
                for (k = 0; k < extent.e_len; k++) {
                    blk64_t b = extent.e_pblk + k;
                    int r = func(fs, &b, (e2_blkcnt_t)(extent.e_lblk + k), 0, 0, priv_data);
                    if (r & BLOCK_ABORT) {
                        ext2fs_extent_free(handle);
                        return 0;
                    }
                }
            }
            retval = ext2fs_extent_get(handle, EXT2_EXTENT_NEXT, &extent);
        }
        ext2fs_extent_free(handle);
        return (retval == EXT2_ET_EXTENT_NOT_FOUND) ? 0 : retval;
    }

    for (i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        blk64_t b = inode.i_block[i];
        if (b) {
            int r = func(fs, &b, logical, 0, 0, priv_data);
            if (r & BLOCK_CHANGED) {
                inode.i_block[i] = (__u32)b;
                changed = 1;
            }
            if (r & BLOCK_ABORT) {
                goto done;
            }
        }
        logical++;
    }
    if (inode.i_block[EXT2_IND_BLOCK]) {
        blk64_t b = inode.i_block[EXT2_IND_BLOCK];
        retval = iterate_ind(fs, &b, 1, &logical, NULL, func, priv_data, flags);
        if (b != inode.i_block[EXT2_IND_BLOCK]) {
            inode.i_block[EXT2_IND_BLOCK] = (__u32)b;
            changed = 1;
        }
        if (retval) return retval;
    }
    if (inode.i_block[EXT2_DIND_BLOCK]) {
        blk64_t b = inode.i_block[EXT2_DIND_BLOCK];
        retval = iterate_ind(fs, &b, 2, &logical, NULL, func, priv_data, flags);
        if (b != inode.i_block[EXT2_DIND_BLOCK]) {
            inode.i_block[EXT2_DIND_BLOCK] = (__u32)b;
            changed = 1;
        }
        if (retval) return retval;
    }

done:
    if (changed && !(flags & BLOCK_FLAG_READ_ONLY)) {
        retval = ext2fs_write_inode(fs, ino, &inode);
    }
    return retval;
}

static int punch_indirect_cb(ext2_filsys fs, blk64_t *blocknr, e2_blkcnt_t blockcnt,
                             blk64_t ref_blk, int ref_offset, void *priv_data) {
    blk64_t *range = (blk64_t *)priv_data;
    blk64_t start = range[0];
    blk64_t end = range[1];
    (void)ref_blk;
    (void)ref_offset;

    if (*blocknr == 0) {
        return 0;
    }
    if (blockcnt < 0 && start == 0 && end == (blk64_t)-1) {
        ext2fs_block_alloc_stats2(fs, *blocknr, -1);
        *blocknr = 0;
        return BLOCK_CHANGED;
    }
    if (blockcnt >= 0 && (blk64_t)blockcnt >= start && (blk64_t)blockcnt <= end) {
        ext2fs_block_alloc_stats2(fs, *blocknr, -1);
        *blocknr = 0;
        return BLOCK_CHANGED;
    }
    return 0;
}

errcode_t ext2fs_punch(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode,
                       char *block_buf, blk64_t start, blk64_t end) {
    struct ext2_inode inode_local;
    errcode_t retval;
    (void)block_buf;

    if (!fs) return EXT2_ET_INVALID_ARGUMENT;
    if (!inode) {
        retval = ext2fs_read_inode(fs, ino, &inode_local);
        if (retval) return retval;
        inode = &inode_local;
    }

    if (inode->i_flags & EXT4_EXTENTS_FL) {
        if (start == 0 && end == (blk64_t)-1) {
            return ext2fs_extent_free_all(fs, ino, inode);
        }
        {
            ext2_extent_handle_t handle = NULL;
            struct ext2fs_extent extent;
            retval = ext2fs_extent_open2(fs, ino, inode, &handle);
            if (retval) return retval;

            retval = ext2fs_extent_get(handle, EXT2_EXTENT_LAST_LEAF, &extent);
            while (retval == 0) {
                blk64_t ext_start = extent.e_lblk;
                blk64_t ext_end = extent.e_lblk + extent.e_len - 1;
                if (ext_end < start) {
                    break;
                }
                if (ext_start >= start && ext_end <= end) {
                    __u32 k;
                    for (k = 0; k < extent.e_len; k++) {
                        ext2fs_block_alloc_stats2(fs, extent.e_pblk + k, -1);
                    }
                    ext2fs_iblk_sub_blocks(fs, inode, extent.e_len);
                    retval = ext2fs_extent_delete(handle, 0);
                    if (retval) break;
                    retval = ext2fs_extent_get(handle, EXT2_EXTENT_LAST_LEAF, &extent);
                    continue;
                } else if (ext_start < start && ext_end >= start) {
                    __u32 keep_len = (__u32)(start - ext_start);
                    __u32 drop_end = (ext_end <= end) ? (__u32)(ext_end - start + 1) : (__u32)(end - start + 1);
                    __u32 k;
                    for (k = 0; k < drop_end; k++) {
                        ext2fs_block_alloc_stats2(fs, extent.e_pblk + keep_len + k, -1);
                    }
                    ext2fs_iblk_sub_blocks(fs, inode, drop_end);
                    extent.e_len = keep_len;
                    retval = ext2fs_extent_replace(handle, 0, &extent);
                    if (retval) break;
                }
                retval = ext2fs_extent_get(handle, EXT2_EXTENT_PREV_LEAF, &extent);
            }
            ext2fs_extent_free(handle);
            if (retval == EXT2_ET_EXTENT_NOT_FOUND) retval = 0;
            if (retval == 0) {
                retval = ext2fs_write_inode(fs, ino, inode);
            }
            return retval;
        }
    }

    {
        blk64_t range[2] = { start, end };
        retval = ext2fs_block_iterate3(fs, ino, BLOCK_FLAG_DEPTH_TRAVERSE, NULL, punch_indirect_cb, range);
        if (retval == 0) {
            retval = ext2fs_read_inode(fs, ino, inode);
        }
        return retval;
    }
}
