/*
 * ext2_fs.h --- on-disk ext2/ext3/ext4 structures (e2fsprogs compatible)
 */
#ifndef _LINUX_EXT2_FS_H
#define _LINUX_EXT2_FS_H

#include <ext2fs/ext2_types.h>

#define EXT2_SUPER_MAGIC      0xEF53
#define EXT2_OLD_REV          0
#define EXT2_DYNAMIC_REV      1
#define EXT2_GOOD_OLD_INODE_SIZE 128
#define EXT2_GOOD_OLD_FIRST_INO  11

#define EXT2_MIN_BLOCK_SIZE   1024
#define EXT2_MAX_BLOCK_SIZE   65536
#define EXT2_MIN_BLOCK_LOG_SIZE 10
#define EXT2_MAX_BLOCK_LOG_SIZE 16

#define EXT2_ROOT_INO         2
#define EXT2_BAD_INO          1
#define EXT2_JOURNAL_INO      8

#define EXT2_NAME_LEN         255

/* File types for directory entries */
#define EXT2_FT_UNKNOWN       0
#define EXT2_FT_REG_FILE      1
#define EXT2_FT_DIR           2
#define EXT2_FT_CHRDEV        3
#define EXT2_FT_BLKDEV        4
#define EXT2_FT_FIFO          5
#define EXT2_FT_SOCK          6
#define EXT2_FT_SYMLINK       7
#define EXT2_FT_MAX           8
#define EXT2_FT_DIR_CSUM      0xDE

/* Linux file mode bits */
#define LINUX_S_IFMT          00170000
#define LINUX_S_IFSOCK        0140000
#define LINUX_S_IFLNK         0120000
#define LINUX_S_IFREG         0100000
#define LINUX_S_IFBLK         0060000
#define LINUX_S_IFDIR         0040000
#define LINUX_S_IFCHR         0020000
#define LINUX_S_IFIFO         0010000

#define LINUX_S_ISREG(m)      (((m) & LINUX_S_IFMT) == LINUX_S_IFREG)
#define LINUX_S_ISDIR(m)      (((m) & LINUX_S_IFMT) == LINUX_S_IFDIR)
#define LINUX_S_ISLNK(m)      (((m) & LINUX_S_IFMT) == LINUX_S_IFLNK)

/* Inode flags */
#define EXT2_SECRM_FL         0x00000001
#define EXT2_UNRM_FL          0x00000002
#define EXT2_COMPR_FL         0x00000004
#define EXT2_SYNC_FL          0x00000008
#define EXT2_IMMUTABLE_FL     0x00000010
#define EXT2_APPEND_FL        0x00000020
#define EXT2_NODUMP_FL        0x00000040
#define EXT2_NOATIME_FL       0x00000080
#define EXT2_INDEX_FL         0x00001000
#define EXT4_EXTENTS_FL       0x00080000
#define EXT4_HUGE_FILE_FL     0x00040000
#define EXT4_INLINE_DATA_FL   0x10000000

/* Superblock state flags */
#define EXT2_VALID_FS         0x0001
#define EXT2_ERROR_FS         0x0002
#define EXT3_ORPHAN_FS        0x0004

/* Superblock feature flags */
#define EXT2_FEATURE_COMPAT_DIR_PREALLOC  0x0001
#define EXT2_FEATURE_COMPAT_IMAGIC_INODES 0x0002
#define EXT3_FEATURE_COMPAT_HAS_JOURNAL   0x0004
#define EXT2_FEATURE_COMPAT_EXT_ATTR      0x0008
#define EXT2_FEATURE_COMPAT_RESIZE_INODE  0x0010
#define EXT2_FEATURE_COMPAT_DIR_INDEX     0x0020
#define EXT4_FEATURE_COMPAT_SPARSE_SUPER2 0x0200

#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE   0x0002
#define EXT2_FEATURE_RO_COMPAT_BTREE_DIR    0x0004
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE    0x0008
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM     0x0010
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK    0x0020
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE  0x0040
#define EXT4_FEATURE_RO_COMPAT_QUOTA        0x0100
#define EXT4_FEATURE_RO_COMPAT_BIGALLOC     0x0200
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400
#define EXT4_FEATURE_RO_COMPAT_READONLY     0x1000
#define EXT4_FEATURE_RO_COMPAT_PROJECT      0x2000

#define EXT2_FEATURE_INCOMPAT_COMPRESSION 0x0001
#define EXT2_FEATURE_INCOMPAT_FILETYPE    0x0002
#define EXT3_FEATURE_INCOMPAT_RECOVER     0x0004
#define EXT3_FEATURE_INCOMPAT_JOURNAL_DEV 0x0008
#define EXT2_FEATURE_INCOMPAT_META_BG     0x0010
#define EXT4_FEATURE_INCOMPAT_EXTENTS     0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT       0x0080
#define EXT4_FEATURE_INCOMPAT_MMP         0x0100
#define EXT4_FEATURE_INCOMPAT_FLEX_BG     0x0200
#define EXT4_FEATURE_INCOMPAT_EA_INODE    0x0400
#define EXT4_FEATURE_INCOMPAT_DIRDATA     0x1000
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED   0x2000
#define EXT4_FEATURE_INCOMPAT_LARGEDIR    0x4000
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x8000
#define EXT4_FEATURE_INCOMPAT_ENCRYPT     0x10000

/* Group descriptor flags */
#define EXT2_BG_INODE_UNINIT  0x0001
#define EXT2_BG_BLOCK_UNINIT  0x0002
#define EXT2_BG_INODE_ZEROED  0x0004

/* Direct/indirect block constants */
#define EXT2_NDIR_BLOCKS      12
#define EXT2_IND_BLOCK        EXT2_NDIR_BLOCKS
#define EXT2_DIND_BLOCK       (EXT2_IND_BLOCK + 1)
#define EXT2_TIND_BLOCK       (EXT2_DIND_BLOCK + 1)
#define EXT2_N_BLOCKS         (EXT2_TIND_BLOCK + 1)

/* JBD2 journal constants */
#define JBD2_MAGIC_NUMBER     0xC03B3998U
#define JBD2_SUPERBLOCK_V1    3
#define JBD2_SUPERBLOCK_V2    4

#pragma pack(push, 1)

struct ext2_super_block {
    __u32 s_inodes_count;
    __u32 s_blocks_count;
    __u32 s_r_blocks_count;
    __u32 s_free_blocks_count;
    __u32 s_free_inodes_count;
    __u32 s_first_data_block;
    __u32 s_log_block_size;
    __u32 s_log_cluster_size;
    __u32 s_blocks_per_group;
    __u32 s_clusters_per_group;
    __u32 s_inodes_per_group;
    __u32 s_mtime;
    __u32 s_wtime;
    __u16 s_mnt_count;
    __s16 s_max_mnt_count;
    __u16 s_magic;
    __u16 s_state;
    __u16 s_errors;
    __u16 s_minor_rev_level;
    __u32 s_lastcheck;
    __u32 s_checkinterval;
    __u32 s_creator_os;
    __u32 s_rev_level;
    __u16 s_def_resuid;
    __u16 s_def_resgid;
    __u32 s_first_ino;
    __u16 s_inode_size;
    __u16 s_block_group_nr;
    __u32 s_feature_compat;
    __u32 s_feature_incompat;
    __u32 s_feature_ro_compat;
    __u8  s_uuid[16];
    __u8  s_volume_name[16];
    __u8  s_last_mounted[64];
    __u32 s_algorithm_usage_bitmap;
    __u8  s_prealloc_blocks;
    __u8  s_prealloc_dir_blocks;
    __u16 s_reserved_gdt_blocks;
    __u8  s_journal_uuid[16];
    __u32 s_journal_inum;
    __u32 s_journal_dev;
    __u32 s_last_orphan;
    __u32 s_hash_seed[4];
    __u8  s_def_hash_version;
    __u8  s_jnl_backup_type;
    __u16 s_desc_size;
    __u32 s_default_mount_opts;
    __u32 s_first_meta_bg;
    __u32 s_mkfs_time;
    __u32 s_jnl_blocks[17];
    __u32 s_blocks_count_hi;
    __u32 s_r_blocks_count_hi;
    __u32 s_free_blocks_hi;
    __u16 s_min_extra_isize;
    __u16 s_want_extra_isize;
    __u32 s_flags;
    __u16 s_raid_stride;
    __u16 s_mmp_update_interval;
    __u64 s_mmp_block;
    __u32 s_raid_stripe_width;
    __u8  s_log_groups_per_flex;
    __u8  s_checksum_type;
    __u8  s_encryption_level;
    __u8  s_reserved_pad;
    __u64 s_kbytes_written;
    __u32 s_snapshot_inum;
    __u32 s_snapshot_id;
    __u64 s_snapshot_r_blocks_count;
    __u32 s_snapshot_list;
    __u32 s_error_count;
    __u32 s_first_error_time;
    __u32 s_first_error_ino;
    __u64 s_first_error_block;
    __u8  s_first_error_func[32];
    __u32 s_first_error_line;
    __u32 s_last_error_time;
    __u32 s_last_error_ino;
    __u32 s_last_error_line;
    __u64 s_last_error_block;
    __u8  s_last_error_func[32];
    __u8  s_mount_opts[64];
    __u32 s_usr_quota_inum;
    __u32 s_grp_quota_inum;
    __u32 s_overhead_clusters;
    __u32 s_backup_bgs[2];
    __u8  s_encrypt_algos[4];
    __u8  s_encrypt_pw_salt[16];
    __u32 s_lpf_ino;
    __u32 s_prj_quota_inum;
    __u32 s_checksum_seed;
    __u8  s_wtime_hi;
    __u8  s_mtime_hi;
    __u8  s_mkfs_time_hi;
    __u8  s_lastcheck_hi;
    __u8  s_first_error_time_hi;
    __u8  s_last_error_time_hi;
    __u8  s_first_error_errcode;
    __u8  s_last_error_errcode;
    __u16 s_encoding;
    __u16 s_encoding_flags;
    __u32 s_orphan_file_inum;
    __u32 s_reserved[94];
    __u32 s_checksum;
};

struct ext2_group_desc {
    __u32 bg_block_bitmap;
    __u32 bg_inode_bitmap;
    __u32 bg_inode_table;
    __u16 bg_free_blocks_count;
    __u16 bg_free_inodes_count;
    __u16 bg_used_dirs_count;
    __u16 bg_flags;
    __u32 bg_exclude_bitmap_lo;
    __u16 bg_block_bitmap_csum_lo;
    __u16 bg_inode_bitmap_csum_lo;
    __u16 bg_itable_unused;
    __u16 bg_checksum;
};

struct ext4_group_desc {
    __u32 bg_block_bitmap;
    __u32 bg_inode_bitmap;
    __u32 bg_inode_table;
    __u16 bg_free_blocks_count;
    __u16 bg_free_inodes_count;
    __u16 bg_used_dirs_count;
    __u16 bg_flags;
    __u32 bg_exclude_bitmap_lo;
    __u16 bg_block_bitmap_csum_lo;
    __u16 bg_inode_bitmap_csum_lo;
    __u16 bg_itable_unused;
    __u16 bg_checksum;
    __u32 bg_block_bitmap_hi;
    __u32 bg_inode_bitmap_hi;
    __u32 bg_inode_table_hi;
    __u16 bg_free_blocks_count_hi;
    __u16 bg_free_inodes_count_hi;
    __u16 bg_used_dirs_count_hi;
    __u16 bg_itable_unused_hi;
    __u32 bg_exclude_bitmap_hi;
    __u16 bg_block_bitmap_csum_hi;
    __u16 bg_inode_bitmap_csum_hi;
    __u32 bg_reserved;
};

struct ext2_inode {
    __u16 i_mode;
    __u16 i_uid;
    __u32 i_size;
    __u32 i_atime;
    __u32 i_ctime;
    __u32 i_mtime;
    __u32 i_dtime;
    __u16 i_gid;
    __u16 i_links_count;
    __u32 i_blocks;
    __u32 i_flags;
    union {
        struct {
            __u32 l_i_version;
        } linux1;
        __u32 osd1_val;
    } osd1;
    __u32 i_block[EXT2_N_BLOCKS];
    __u32 i_generation;
    __u32 i_file_acl;
    __u32 i_size_high;
    __u32 i_faddr;
    union {
        struct {
            __u16 l_i_blocks_hi;
            __u16 l_i_file_acl_high;
            __u16 l_i_uid_high;
            __u16 l_i_gid_high;
            __u16 l_i_checksum_lo;
            __u16 l_i_reserved;
        } linux2;
    } osd2;
};

struct ext2_inode_large {
    __u16 i_mode;
    __u16 i_uid;
    __u32 i_size;
    __u32 i_atime;
    __u32 i_ctime;
    __u32 i_mtime;
    __u32 i_dtime;
    __u16 i_gid;
    __u16 i_links_count;
    __u32 i_blocks;
    __u32 i_flags;
    union {
        struct {
            __u32 l_i_version;
        } linux1;
        __u32 osd1_val;
    } osd1;
    __u32 i_block[EXT2_N_BLOCKS];
    __u32 i_generation;
    __u32 i_file_acl;
    __u32 i_size_high;
    __u32 i_faddr;
    union {
        struct {
            __u16 l_i_blocks_hi;
            __u16 l_i_file_acl_high;
            __u16 l_i_uid_high;
            __u16 l_i_gid_high;
            __u16 l_i_checksum_lo;
            __u16 l_i_reserved;
        } linux2;
    } osd2;
    __u16 i_extra_isize;
    __u16 i_checksum_hi;
    __u32 i_ctime_extra;
    __u32 i_mtime_extra;
    __u32 i_atime_extra;
    __u32 i_crtime;
    __u32 i_crtime_extra;
    __u32 i_version_hi;
    __u32 i_projid;
};

#define EXT4_EXT_MAGIC 0xF30A

struct ext4_extent_header {
    __u16 eh_magic;
    __u16 eh_entries;
    __u16 eh_max;
    __u16 eh_depth;
    __u32 eh_generation;
};

struct ext4_extent {
    __u32 ee_block;
    __u16 ee_len;
    __u16 ee_start_hi;
    __u32 ee_start_lo;
};

struct ext4_extent_idx {
    __u32 ei_block;
    __u32 ei_leaf_lo;
    __u16 ei_leaf_hi;
    __u16 ei_unused;
};

struct ext4_extent_tail {
    __u32 et_checksum;
};

struct ext2_dir_entry {
    __u32 inode;
    __u16 rec_len;
    __u16 name_len;
    char  name[EXT2_NAME_LEN];
};

struct ext2_dir_entry_2 {
    __u32 inode;
    __u16 rec_len;
    __u8  name_len;
    __u8  file_type;
    char  name[EXT2_NAME_LEN];
};

struct ext2_dir_entry_tail {
    __u32 det_reserved_zero1;
    __u16 det_rec_len;
    __u8  det_reserved_zero2;
    __u8  det_reserved_ft;
    __u32 det_checksum;
};

typedef struct journal_header_s {
    __u32 h_magic;
    __u32 h_blocktype;
    __u32 h_sequence;
} journal_header_t;

typedef struct journal_superblock_s {
    journal_header_t s_header;
    __u32 s_blocksize;
    __u32 s_maxlen;
    __u32 s_first;
    __u32 s_sequence;
    __u32 s_start;
    __s32 s_errno;
    __u32 s_feature_compat;
    __u32 s_feature_incompat;
    __u32 s_feature_ro_compat;
    __u8  s_uuid[16];
    __u32 s_nr_users;
    __u32 s_dynsuper;
    __u32 s_max_transaction;
    __u32 s_max_trans_data;
    __u8  s_checksum_type;
    __u8  s_padding2[3];
    __u32 s_padding[42];
    __u32 s_checksum;
} journal_superblock_t;

#pragma pack(pop)

#define EXT2_SB(sb) (sb)
#define EXT2_MIN_DESC_SIZE 32
#define EXT2_MIN_DESC_SIZE_64BIT 64
#define EXT2_DESC_SIZE(s) \
    (((s)->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && (s)->s_desc_size >= EXT2_MIN_DESC_SIZE_64BIT ? (s)->s_desc_size : EXT2_MIN_DESC_SIZE)
#define EXT2_INODE_SIZE(s) \
    (((s)->s_rev_level == EXT2_OLD_REV) ? EXT2_GOOD_OLD_INODE_SIZE : (s)->s_inode_size)
#define EXT2_BLOCK_SIZE(s) ((unsigned int)(EXT2_MIN_BLOCK_SIZE << (s)->s_log_block_size))
#define EXT2_BLOCKS_PER_GROUP(s) ((s)->s_blocks_per_group)
#define EXT2_INODES_PER_GROUP(s) ((s)->s_inodes_per_group)
#define EXT2_INODES_PER_BLOCK(s) (EXT2_BLOCK_SIZE(s) / EXT2_INODE_SIZE(s))
#define EXT2_DESC_PER_BLOCK(s) (EXT2_BLOCK_SIZE(s) / EXT2_DESC_SIZE(s))

#define EXT2_DIR_PAD 4
#define EXT2_DIR_ROUND (EXT2_DIR_PAD - 1)
#define EXT2_DIR_REC_LEN(name_len) (((name_len) + 8 + EXT2_DIR_ROUND) & ~EXT2_DIR_ROUND)

#define EXT4_EPOCH_BITS 2
#define EXT4_EPOCH_MASK ((1U << EXT4_EPOCH_BITS) - 1)
#define EXT4_NSEC_MASK (~0U << EXT4_EPOCH_BITS)

/* Compile-time size verification */
typedef char ext2_fs_check_sb_size[(sizeof(struct ext2_super_block) == 1024) ? 1 : -1];
typedef char ext2_fs_check_gd32_size[(sizeof(struct ext2_group_desc) == 32) ? 1 : -1];
typedef char ext2_fs_check_gd64_size[(sizeof(struct ext4_group_desc) == 64) ? 1 : -1];
typedef char ext2_fs_check_ino128_size[(sizeof(struct ext2_inode) == 128) ? 1 : -1];
typedef char ext2_fs_check_ino_large_size[(sizeof(struct ext2_inode_large) == 160) ? 1 : -1];

#endif /* _LINUX_EXT2_FS_H */
