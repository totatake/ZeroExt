#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "ext2fs/ext2fs.h"

struct ext2fs_struct_generic_bitmap {
    errcode_t magic;
    ext2_filsys fs;
    __u64 start;
    __u64 end;
    __u64 real_end;
    char *description;
    unsigned char *bitmap;
    size_t nbytes;
};

static errcode_t alloc_bmap(ext2_filsys fs, errcode_t magic, __u64 start, __u64 end, __u64 real_end,
                            const char *descr, ext2fs_generic_bitmap *ret) {
    ext2fs_generic_bitmap bmap = NULL;
    __u64 bits;
    size_t nbytes;
    errcode_t retval;

    if (real_end < end) {
        real_end = end;
    }
    bits = real_end - start + 1;
    nbytes = (size_t)((bits + 7) / 8);
    if (nbytes == 0) {
        nbytes = 1;
    }

    retval = ext2fs_get_memzero(sizeof(*bmap), &bmap);
    if (retval) {
        return retval;
    }
    bmap->magic = magic;
    bmap->fs = fs;
    bmap->start = start;
    bmap->end = end;
    bmap->real_end = real_end;
    bmap->nbytes = nbytes;

    if (descr) {
        retval = ext2fs_get_mem((unsigned long)strlen(descr) + 1, &bmap->description);
        if (retval) {
            free(bmap);
            return retval;
        }
        strcpy(bmap->description, descr);
    }

    retval = ext2fs_get_memzero((unsigned long)nbytes, &bmap->bitmap);
    if (retval) {
        ext2fs_free_mem(&bmap->description);
        free(bmap);
        return retval;
    }

    *ret = bmap;
    return 0;
}

errcode_t ext2fs_allocate_block_bitmap(ext2_filsys fs, const char *descr, ext2fs_block_bitmap *ret) {
    __u64 start = fs->super->s_first_data_block;
    __u64 end = ext2fs_blocks_count(fs->super) - 1;
    __u64 real_end = ((__u64)fs->group_desc_count * fs->super->s_blocks_per_group) + start - 1;
    return alloc_bmap(fs, EXT2_ET_MAGIC_BLOCK_BITMAP, start, end, real_end, descr, ret);
}

errcode_t ext2fs_allocate_inode_bitmap(ext2_filsys fs, const char *descr, ext2fs_inode_bitmap *ret) {
    __u64 start = 1;
    __u64 end = fs->super->s_inodes_count;
    __u64 real_end = (__u64)fs->group_desc_count * fs->super->s_inodes_per_group;
    return alloc_bmap(fs, EXT2_ET_MAGIC_INODE_BITMAP, start, end, real_end, descr, ret);
}

void ext2fs_free_block_bitmap(ext2fs_block_bitmap bitmap) {
    if (!bitmap) {
        return;
    }
    ext2fs_free_mem(&bitmap->description);
    ext2fs_free_mem(&bitmap->bitmap);
    bitmap->magic = 0;
    free(bitmap);
}

void ext2fs_free_inode_bitmap(ext2fs_inode_bitmap bitmap) {
    ext2fs_free_block_bitmap(bitmap);
}

int ext2fs_test_block_bitmap2(ext2fs_block_bitmap bitmap, blk64_t block) {
    __u64 idx;
    if (!bitmap || block < bitmap->start || block > bitmap->real_end) {
        return 0;
    }
    idx = block - bitmap->start;
    return (bitmap->bitmap[idx >> 3] & (1U << (idx & 7U))) != 0;
}

void ext2fs_mark_block_bitmap2(ext2fs_block_bitmap bitmap, blk64_t block) {
    __u64 idx;
    if (!bitmap || block < bitmap->start || block > bitmap->real_end) {
        return;
    }
    idx = block - bitmap->start;
    bitmap->bitmap[idx >> 3] |= (unsigned char)(1U << (idx & 7U));
}

void ext2fs_unmark_block_bitmap2(ext2fs_block_bitmap bitmap, blk64_t block) {
    __u64 idx;
    if (!bitmap || block < bitmap->start || block > bitmap->real_end) {
        return;
    }
    idx = block - bitmap->start;
    bitmap->bitmap[idx >> 3] &= (unsigned char)(~(1U << (idx & 7U)));
}

int ext2fs_test_inode_bitmap2(ext2fs_inode_bitmap bitmap, ext2_ino_t inode) {
    return ext2fs_test_block_bitmap2(bitmap, (blk64_t)inode);
}

void ext2fs_mark_inode_bitmap2(ext2fs_inode_bitmap bitmap, ext2_ino_t inode) {
    ext2fs_mark_block_bitmap2(bitmap, (blk64_t)inode);
}

void ext2fs_unmark_inode_bitmap2(ext2fs_inode_bitmap bitmap, ext2_ino_t inode) {
    ext2fs_unmark_block_bitmap2(bitmap, (blk64_t)inode);
}

errcode_t ext2fs_find_first_zero_block_bitmap2(ext2fs_block_bitmap bitmap, blk64_t start, blk64_t end, blk64_t *out) {
    blk64_t cur;
    if (!bitmap || !out) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    if (start < bitmap->start) {
        start = bitmap->start;
    }
    if (end > bitmap->end) {
        end = bitmap->end;
    }
    for (cur = start; cur <= end; ) {
        __u64 idx = cur - bitmap->start;
        if ((idx & 7ULL) == 0 && (cur + 7ULL) <= end && bitmap->bitmap[idx >> 3] == 0xFF) {
            cur += 8;
            continue;
        }
        if ((bitmap->bitmap[idx >> 3] & (1U << (idx & 7U))) == 0) {
            *out = cur;
            return 0;
        }
        cur++;
    }
    return ENOENT;
}

errcode_t ext2fs_find_first_zero_inode_bitmap2(ext2fs_inode_bitmap bitmap, ext2_ino_t start, ext2_ino_t end, ext2_ino_t *out) {
    blk64_t found = 0;
    errcode_t retval = ext2fs_find_first_zero_block_bitmap2(bitmap, (blk64_t)start, (blk64_t)end, &found);
    if (retval == 0 && out) {
        *out = (ext2_ino_t)found;
    }
    return retval;
}

unsigned char *ext2fs_get_bitmap_raw_buffer(ext2fs_generic_bitmap bitmap, size_t *out_nbytes) {
    if (!bitmap) {
        return NULL;
    }
    if (out_nbytes) {
        *out_nbytes = bitmap->nbytes;
    }
    return bitmap->bitmap;
}
