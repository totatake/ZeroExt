/*
 * ext2fs.h --- header file for the ext2fs library
 */
#ifndef _EXT2FS_EXT2FS_H
#define _EXT2FS_EXT2FS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <et/com_err.h>
#include <ext2fs/ext2_types.h>
#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2_io.h>
#include <ext2fs/ext2_err.h>

typedef struct struct_ext2_filsys *ext2_filsys;
typedef struct ext2fs_struct_generic_bitmap *ext2fs_generic_bitmap;
typedef ext2fs_generic_bitmap ext2fs_inode_bitmap;
typedef ext2fs_generic_bitmap ext2fs_block_bitmap;
typedef struct ext2_file *ext2_file_t;
typedef struct ext2_extent_handle *ext2_extent_handle_t;
typedef __s64 e2_blkcnt_t;

/* Filesystem open flags */
#define EXT2_FLAG_RW                 0x00001
#define EXT2_FLAG_CHANGED            0x00002
#define EXT2_FLAG_DIRTY              0x00004
#define EXT2_FLAG_VALID              0x00008
#define EXT2_FLAG_IB_DIRTY           0x00010
#define EXT2_FLAG_BB_DIRTY           0x00020
#define EXT2_FLAG_SWAP_BYTES         0x00040
#define EXT2_FLAG_SUPER_ONLY         0x00400
#define EXT2_FLAG_FORCE              0x01000
#define EXT2_FLAG_EXCLUSIVE          0x04000
#define EXT2_FLAG_IGNORE_CSUM_ERRORS 0x200000
#define EXT2_FLAG_64BITS             0x20000
#define EXT2_FLAG_THREADS            0x80000

/* File open flags */
#define EXT2_FILE_WRITE      0x0001
#define EXT2_FILE_CREATE     0x0002

/* Seek whence constants */
#define EXT2_SEEK_SET 0
#define EXT2_SEEK_CUR 1
#define EXT2_SEEK_END 2

/* Directory iteration flags */
#define DIRENT_FLAG_INCLUDE_EMPTY 1
#define DIRENT_FLAG_INCLUDE_REMOVED 2

#define DIRENT_CHANGED 1
#define DIRENT_ABORT   2
#define DIRENT_ERROR   3

/* Block iteration flags */
#define BLOCK_FLAG_APPEND 1
#define BLOCK_FLAG_HOLE   2
#define BLOCK_FLAG_DEPTH_TRAVERSE 4
#define BLOCK_FLAG_DATA_ONLY 8
#define BLOCK_FLAG_READ_ONLY 16

#define BLOCK_CHANGED 1
#define BLOCK_ABORT   2
#define BLOCK_ERROR   4

/* Extent iteration / manipulation constants */
#define EXT2_EXTENT_FLAGS_LEAF 0x0001
#define EXT2_EXTENT_FLAGS_UNINIT 0x0002

#define EXT2_EXTENT_CURRENT 0x0000
#define EXT2_EXTENT_ROOT    0x0001
#define EXT2_EXTENT_LAST_LEAF 0x0002
#define EXT2_EXTENT_FIRST_SIB 0x0003
#define EXT2_EXTENT_LAST_SIB  0x0004
#define EXT2_EXTENT_NEXT_SIB  0x0005
#define EXT2_EXTENT_PREV_SIB  0x0006
#define EXT2_EXTENT_NEXT_LEAF 0x0007
#define EXT2_EXTENT_PREV_LEAF 0x0008
#define EXT2_EXTENT_NEXT      0x0009
#define EXT2_EXTENT_PREV      0x000A
#define EXT2_EXTENT_UP        0x000B
#define EXT2_EXTENT_DOWN      0x000C
#define EXT2_EXTENT_DOWN_AND_LAST 0x000D

#define EXT2_EXTENT_INSERT_AFTER 0x0001
#define EXT2_EXTENT_INSERT_NOSPLIT 0x0002

/* BMAP flags */
#define BMAP_ALLOC 0x0001
#define BMAP_SET   0x0002
#define BMAP_ZERO  0x0008
#define BMAP_UNINIT 0x0010

#define BMAP_RET_UNINIT 0x0001

/* Feature check helpers */
#define EXT2_HAS_COMPAT_FEATURE(sb, mask) (((sb)->s_feature_compat & (mask)) != 0)
#define EXT2_HAS_RO_COMPAT_FEATURE(sb, mask) (((sb)->s_feature_ro_compat & (mask)) != 0)
#define EXT2_HAS_INCOMPAT_FEATURE(sb, mask) (((sb)->s_feature_incompat & (mask)) != 0)
#define EXT2_SET_COMPAT_FEATURE(sb, mask) ((sb)->s_feature_compat |= (mask))
#define EXT2_SET_RO_COMPAT_FEATURE(sb, mask) ((sb)->s_feature_ro_compat |= (mask))
#define EXT2_SET_INCOMPAT_FEATURE(sb, mask) ((sb)->s_feature_incompat |= (mask))

#define EXT2_I_SIZE(inode) (((__u64)(inode)->i_size) | (((__u64)(inode)->i_size_high) << 32))

struct ext2fs_extent {
    blk64_t e_pblk;
    blk64_t e_lblk;
    __u32   e_len;
    __u32   e_flags;
};

struct struct_ext2_filsys {
    errcode_t magic;
    io_channel io;
    int flags;
    char *device_name;
    struct ext2_super_block *super;
    unsigned int blocksize;
    int fragsize;
    dgrp_t group_desc_count;
    unsigned long desc_blocks;
    struct opaque_ext2_group_desc *group_desc;
    unsigned int inode_blocks_per_group;
    ext2fs_inode_bitmap inode_map;
    ext2fs_block_bitmap block_map;
    int stride;
    blk64_t super_blk;
    blk64_t blocks_count;
    __u32 csum_seed;
    int (*get_blocks)(ext2_filsys fs, ext2_ino_t ino, blk_t *blocks);
    void *priv_data;
};

/* Inline helper for 64-bit block counts */
static __inline blk64_t ext2fs_blocks_count(const struct ext2_super_block *sb) {
    blk64_t ret = sb->s_blocks_count;
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        ret |= ((blk64_t)sb->s_blocks_count_hi) << 32;
    }
    return ret;
}

static __inline void ext2fs_blocks_count_set(struct ext2_super_block *sb, blk64_t blk) {
    sb->s_blocks_count = (__u32)(blk & 0xFFFFFFFFULL);
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        sb->s_blocks_count_hi = (__u32)(blk >> 32);
    }
}

static __inline blk64_t ext2fs_r_blocks_count(const struct ext2_super_block *sb) {
    blk64_t ret = sb->s_r_blocks_count;
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        ret |= ((blk64_t)sb->s_r_blocks_count_hi) << 32;
    }
    return ret;
}

static __inline blk64_t ext2fs_free_blocks_count(const struct ext2_super_block *sb) {
    blk64_t ret = sb->s_free_blocks_count;
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        ret |= ((blk64_t)sb->s_free_blocks_hi) << 32;
    }
    return ret;
}

static __inline void ext2fs_free_blocks_count_set(struct ext2_super_block *sb, blk64_t blk) {
    sb->s_free_blocks_count = (__u32)(blk & 0xFFFFFFFFULL);
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        sb->s_free_blocks_hi = (__u32)(blk >> 32);
    }
}

/* Memory helpers */
errcode_t ext2fs_get_mem(unsigned long size, void *ptr);
errcode_t ext2fs_get_memzero(unsigned long size, void *ptr);
errcode_t ext2fs_free_mem(void *ptr);
errcode_t ext2fs_resize_mem(unsigned long old_size, unsigned long size, void *ptr);

/* Filesystem lifecycle */
errcode_t ext2fs_open2(const char *name, const char *io_options, int flags,
                       int superblock, unsigned int block_size,
                       io_manager manager, ext2_filsys *ret_fs);
errcode_t ext2fs_open(const char *name, int flags, int superblock,
                      unsigned int block_size, io_manager manager,
                      ext2_filsys *ret_fs);
errcode_t ext2fs_initialize(const char *name, int flags,
                            struct ext2_super_block *param,
                            io_manager manager, ext2_filsys *ret_fs);
errcode_t ext2fs_allocate_tables(ext2_filsys fs);
errcode_t ext2fs_flush(ext2_filsys fs);
errcode_t ext2fs_flush2(ext2_filsys fs, int flags);
errcode_t ext2fs_close(ext2_filsys fs);
errcode_t ext2fs_close_free(ext2_filsys *fs);
void ext2fs_free(ext2_filsys fs);
void ext2fs_mark_super_dirty(ext2_filsys fs);
void ext2fs_mark_changed(ext2_filsys fs);
void ext2fs_mark_ib_dirty(ext2_filsys fs);
void ext2fs_mark_bb_dirty(ext2_filsys fs);
int ext2fs_test_valid(ext2_filsys fs);
void ext2fs_mark_valid(ext2_filsys fs);
void ext2fs_unmark_valid(ext2_filsys fs);

/* Block group / numbering helpers (blknum.c) */
dgrp_t ext2fs_group_of_blk2(ext2_filsys fs, blk64_t blk);
blk64_t ext2fs_group_first_block2(ext2_filsys fs, dgrp_t group);
blk64_t ext2fs_group_last_block2(ext2_filsys fs, dgrp_t group);
int ext2fs_group_blocks_count(ext2_filsys fs, dgrp_t group);
dgrp_t ext2fs_group_of_ino(ext2_filsys fs, ext2_ino_t ino);
struct ext2_group_desc *ext2fs_group_desc(ext2_filsys fs, struct opaque_ext2_group_desc *gdp, dgrp_t group);
struct ext4_group_desc *ext4fs_group_desc(ext2_filsys fs, struct opaque_ext2_group_desc *gdp, dgrp_t group);
blk64_t ext2fs_block_bitmap_loc(ext2_filsys fs, dgrp_t group);
void ext2fs_block_bitmap_loc_set(ext2_filsys fs, dgrp_t group, blk64_t blk);
blk64_t ext2fs_inode_bitmap_loc(ext2_filsys fs, dgrp_t group);
void ext2fs_inode_bitmap_loc_set(ext2_filsys fs, dgrp_t group, blk64_t blk);
blk64_t ext2fs_inode_table_loc(ext2_filsys fs, dgrp_t group);
void ext2fs_inode_table_loc_set(ext2_filsys fs, dgrp_t group, blk64_t blk);
__u32 ext2fs_bg_free_blocks_count(ext2_filsys fs, dgrp_t group);
void ext2fs_bg_free_blocks_count_set(ext2_filsys fs, dgrp_t group, __u32 n);
__u32 ext2fs_bg_free_inodes_count(ext2_filsys fs, dgrp_t group);
void ext2fs_bg_free_inodes_count_set(ext2_filsys fs, dgrp_t group, __u32 n);
__u32 ext2fs_bg_used_dirs_count(ext2_filsys fs, dgrp_t group);
void ext2fs_bg_used_dirs_count_set(ext2_filsys fs, dgrp_t group, __u32 n);
__u16 ext2fs_bg_flags(ext2_filsys fs, dgrp_t group);
void ext2fs_bg_flags_set(ext2_filsys fs, dgrp_t group, __u16 flags);
void ext2fs_bg_flags_clear(ext2_filsys fs, dgrp_t group, __u16 flags);
int ext2fs_bg_has_super(ext2_filsys fs, dgrp_t group);

/* Bitmaps & allocation */
errcode_t ext2fs_allocate_block_bitmap(ext2_filsys fs, const char *descr, ext2fs_block_bitmap *ret);
errcode_t ext2fs_allocate_inode_bitmap(ext2_filsys fs, const char *descr, ext2fs_inode_bitmap *ret);
void ext2fs_free_block_bitmap(ext2fs_block_bitmap bitmap);
void ext2fs_free_inode_bitmap(ext2fs_inode_bitmap bitmap);
errcode_t ext2fs_read_bitmaps(ext2_filsys fs);
errcode_t ext2fs_read_inode_bitmap(ext2_filsys fs);
errcode_t ext2fs_read_block_bitmap(ext2_filsys fs);
errcode_t ext2fs_write_bitmaps(ext2_filsys fs);
errcode_t ext2fs_write_inode_bitmap(ext2_filsys fs);
errcode_t ext2fs_write_block_bitmap(ext2_filsys fs);

int ext2fs_test_block_bitmap2(ext2fs_block_bitmap bitmap, blk64_t block);
void ext2fs_mark_block_bitmap2(ext2fs_block_bitmap bitmap, blk64_t block);
void ext2fs_unmark_block_bitmap2(ext2fs_block_bitmap bitmap, blk64_t block);
int ext2fs_test_inode_bitmap2(ext2fs_inode_bitmap bitmap, ext2_ino_t inode);
void ext2fs_mark_inode_bitmap2(ext2fs_inode_bitmap bitmap, ext2_ino_t inode);
void ext2fs_unmark_inode_bitmap2(ext2fs_inode_bitmap bitmap, ext2_ino_t inode);
errcode_t ext2fs_find_first_zero_block_bitmap2(ext2fs_block_bitmap bitmap, blk64_t start, blk64_t end, blk64_t *out);
errcode_t ext2fs_find_first_zero_inode_bitmap2(ext2fs_inode_bitmap bitmap, ext2_ino_t start, ext2_ino_t end, ext2_ino_t *out);

errcode_t ext2fs_new_inode(ext2_filsys fs, ext2_ino_t dir, int mode, ext2fs_inode_bitmap map, ext2_ino_t *ret);
errcode_t ext2fs_new_block2(ext2_filsys fs, blk64_t goal, ext2fs_block_bitmap map, blk64_t *ret);
errcode_t ext2fs_alloc_block2(ext2_filsys fs, blk64_t goal, char *block_buf, blk64_t *ret);
void ext2fs_block_alloc_stats2(ext2_filsys fs, blk64_t blk, int inuse);
void ext2fs_inode_alloc_stats2(ext2_filsys fs, ext2_ino_t ino, int inuse, int isdir);
errcode_t ext2fs_reserve_super_and_bgd(ext2_filsys fs, dgrp_t group, ext2fs_block_bitmap bmap);
blk64_t ext2fs_find_inode_goal(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode, blk64_t lblk);

/* Inode read/write */
errcode_t ext2fs_read_inode_full(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode, int bufsize);
errcode_t ext2fs_read_inode(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode);
errcode_t ext2fs_write_inode_full(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode, int bufsize);
errcode_t ext2fs_write_inode(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode);
errcode_t ext2fs_write_new_inode(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode);
errcode_t ext2fs_inode_size_set(ext2_filsys fs, struct ext2_inode *inode, ext2_off64_t size);
__u64 ext2fs_get_inode_blocks(ext2_filsys fs, const struct ext2_inode *inode);
errcode_t ext2fs_iblk_add_blocks(ext2_filsys fs, struct ext2_inode *inode, blk64_t num_blocks);
errcode_t ext2fs_iblk_sub_blocks(ext2_filsys fs, struct ext2_inode *inode, blk64_t num_blocks);

/* Extent operations */
errcode_t ext2fs_extent_open(ext2_filsys fs, ext2_ino_t ino, ext2_extent_handle_t *ret_handle);
errcode_t ext2fs_extent_open2(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode, ext2_extent_handle_t *ret_handle);
void ext2fs_extent_free(ext2_extent_handle_t handle);
errcode_t ext2fs_extent_get(ext2_extent_handle_t handle, int flags, struct ext2fs_extent *extent);
errcode_t ext2fs_extent_goto(ext2_extent_handle_t handle, blk64_t blk);
errcode_t ext2fs_extent_insert(ext2_extent_handle_t handle, int flags, struct ext2fs_extent *extent);
errcode_t ext2fs_extent_replace(ext2_extent_handle_t handle, int flags, struct ext2fs_extent *extent);
errcode_t ext2fs_extent_delete(ext2_extent_handle_t handle, int flags);
errcode_t ext2fs_extent_set_bmap(ext2_extent_handle_t handle, blk64_t logical, blk64_t physical, int flags);
errcode_t ext2fs_extent_free_all(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode);

/* Block mapping & iteration */
errcode_t ext2fs_bmap2(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode,
                       char *block_buf, int bmap_flags, blk64_t block,
                       int *ret_flags, blk64_t *phys_blk);
errcode_t ext2fs_block_iterate3(ext2_filsys fs, ext2_ino_t ino, int flags, char *block_buf,
                                int (*func)(ext2_filsys fs, blk64_t *blocknr, e2_blkcnt_t blockcnt,
                                            blk64_t ref_blk, int ref_offset, void *priv_data)
                                , void *priv_data);
/* Use __s64 for e2_blkcnt_t (defined below) */
errcode_t ext2fs_punch(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode,
                       char *block_buf, blk64_t start, blk64_t end);
errcode_t ext2fs_read_ind_block(ext2_filsys fs, blk_t blk, void *buf);
errcode_t ext2fs_write_ind_block(ext2_filsys fs, blk_t blk, void *buf);
errcode_t ext2fs_zero_blocks2(ext2_filsys fs, blk64_t blk, int num, blk64_t *ret_blk, int *ret_count);

/* File I/O */
errcode_t ext2fs_file_open2(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode, int flags, ext2_file_t *ret);
errcode_t ext2fs_file_open(ext2_filsys fs, ext2_ino_t ino, int flags, ext2_file_t *ret);
errcode_t ext2fs_file_close(ext2_file_t file);
errcode_t ext2fs_file_flush(ext2_file_t file);
errcode_t ext2fs_file_llseek(ext2_file_t file, __u64 offset, int whence, __u64 *ret_pos);
errcode_t ext2fs_file_read(ext2_file_t file, void *buf, unsigned int wanted, unsigned int *got);
errcode_t ext2fs_file_write(ext2_file_t file, const void *buf, unsigned int nbytes, unsigned int *written);
errcode_t ext2fs_file_get_size(ext2_file_t file, ext2_off64_t *ret_size);
errcode_t ext2fs_file_set_size2(ext2_file_t file, ext2_off64_t size);
struct ext2_inode *ext2fs_file_get_inode(ext2_file_t file);
ext2_filsys ext2fs_file_get_fs(ext2_file_t file);

/* Directory operations */
errcode_t ext2fs_read_dir_block4(ext2_filsys fs, blk64_t block, void *buf, int flags, ext2_ino_t ino);
errcode_t ext2fs_write_dir_block4(ext2_filsys fs, blk64_t block, void *buf, int flags, ext2_ino_t ino);
errcode_t ext2fs_new_dir_block(ext2_filsys fs, ext2_ino_t dir_ino, ext2_ino_t parent_ino, char **block);
int ext2fs_dir_iterate2(ext2_filsys fs, ext2_ino_t dir, int flags, char *block_buf,
                        int (*func)(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                                    int offset, int blocksize, char *buf, void *priv_data),
                        void *priv_data);
errcode_t ext2fs_lookup(ext2_filsys fs, ext2_ino_t dir, const char *name, int namelen, char *buf, ext2_ino_t *inode);
errcode_t ext2fs_namei(ext2_filsys fs, ext2_ino_t root, ext2_ino_t cwd, const char *name, ext2_ino_t *inode);
errcode_t ext2fs_namei_follow(ext2_filsys fs, ext2_ino_t root, ext2_ino_t cwd, const char *name, ext2_ino_t *inode);
errcode_t ext2fs_mkdir(ext2_filsys fs, ext2_ino_t parent, ext2_ino_t inum, const char *name);
errcode_t ext2fs_link(ext2_filsys fs, ext2_ino_t dir, const char *name, ext2_ino_t ino, int flags);
errcode_t ext2fs_unlink(ext2_filsys fs, ext2_ino_t dir, const char *name, ext2_ino_t ino, int flags);
errcode_t ext2fs_expand_dir(ext2_filsys fs, ext2_ino_t dir);
errcode_t ext2fs_symlink(ext2_filsys fs, ext2_ino_t parent, ext2_ino_t ino, const char *name, const char *target);
int ext2fs_is_fast_symlink(struct ext2_inode *inode);
int ext2fs_file_type(int mode);

/* Inline data helpers */
errcode_t ext2fs_inline_data_get(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode, void *buf, size_t *size);
errcode_t ext2fs_inline_data_size(ext2_filsys fs, ext2_ino_t ino, size_t *size);

/* Checksums */
__u16 ext2fs_crc16(__u16 crc, void const *buffer, size_t len);
__u32 ext2fs_crc32c_le(__u32 crc, unsigned char const *p, size_t len);
int ext2fs_has_feature_metadata_csum(struct ext2_super_block *sb);
void ext2fs_init_csum_seed(ext2_filsys fs);
int ext2fs_superblock_csum_verify(ext2_filsys fs, struct ext2_super_block *sb);
errcode_t ext2fs_superblock_csum_set(ext2_filsys fs, struct ext2_super_block *sb);
int ext2fs_group_desc_csum_verify(ext2_filsys fs, dgrp_t group);
void ext2fs_group_desc_csum_set(ext2_filsys fs, dgrp_t group);
errcode_t ext2fs_inode_csum_verify(ext2_filsys fs, ext2_ino_t inum, struct ext2_inode_large *inode);
errcode_t ext2fs_inode_csum_set(ext2_filsys fs, ext2_ino_t inum, struct ext2_inode_large *inode);
errcode_t ext2fs_extent_block_csum_verify(ext2_filsys fs, ext2_ino_t inum, struct ext4_extent_header *eh);
errcode_t ext2fs_extent_block_csum_set(ext2_filsys fs, ext2_ino_t inum, struct ext4_extent_header *eh);
int ext2fs_dir_block_csum_verify(ext2_filsys fs, ext2_ino_t inum, struct ext2_dir_entry *dirent);
errcode_t ext2fs_dir_block_csum_set(ext2_filsys fs, ext2_ino_t inum, struct ext2_dir_entry *dirent);
int ext2fs_block_bitmap_csum_verify(ext2_filsys fs, dgrp_t group, char *bitmap, int size);
void ext2fs_block_bitmap_csum_set(ext2_filsys fs, dgrp_t group, char *bitmap, int size);
int ext2fs_inode_bitmap_csum_verify(ext2_filsys fs, dgrp_t group, char *bitmap, int size);
void ext2fs_inode_bitmap_csum_set(ext2_filsys fs, dgrp_t group, char *bitmap, int size);

#endif /* _EXT2FS_EXT2FS_H */
