#include "config.h"
#include <stddef.h>
#include <string.h>
#include "ext2fs/ext2fs.h"

int ext2fs_has_feature_metadata_csum(struct ext2_super_block *sb) {
    return (sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0;
}

void ext2fs_init_csum_seed(ext2_filsys fs) {
    if (!fs || !fs->super) {
        return;
    }
    if (EXT2_HAS_INCOMPAT_FEATURE(fs->super, EXT4_FEATURE_INCOMPAT_CSUM_SEED)) {
        fs->csum_seed = fs->super->s_checksum_seed;
    } else if (EXT2_HAS_RO_COMPAT_FEATURE(fs->super, EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) {
        fs->csum_seed = ext2fs_crc32c_le(~0U, fs->super->s_uuid, sizeof(fs->super->s_uuid));
    } else {
        fs->csum_seed = ~0U;
    }
}

static __u32 calc_superblock_csum(struct ext2_super_block *sb) {
    size_t offset = offsetof(struct ext2_super_block, s_checksum);
    return ext2fs_crc32c_le(~0U, (const unsigned char *)sb, offset);
}

int ext2fs_superblock_csum_verify(ext2_filsys fs, struct ext2_super_block *sb) {
    __u32 calculated;
    (void)fs;
    if (!ext2fs_has_feature_metadata_csum(sb)) {
        return 1;
    }
    calculated = calc_superblock_csum(sb);
    return calculated == sb->s_checksum;
}

errcode_t ext2fs_superblock_csum_set(ext2_filsys fs, struct ext2_super_block *sb) {
    (void)fs;
    if (!ext2fs_has_feature_metadata_csum(sb)) {
        return 0;
    }
    sb->s_checksum_type = 1; /* EXT4_CRC32C_CHKSUM */
    sb->s_checksum = calc_superblock_csum(sb);
    return 0;
}

static __u16 calc_group_desc_csum(ext2_filsys fs, dgrp_t group, const struct ext4_group_desc *gdp) {
    size_t desc_size = EXT2_DESC_SIZE(fs->super);
    __u32 le_group = group;
    if (ext2fs_has_feature_metadata_csum(fs->super)) {
        unsigned char buf[64];
        __u32 crc;
        size_t copy_sz = desc_size > sizeof(buf) ? sizeof(buf) : desc_size;
        memcpy(buf, gdp, copy_sz);
        /* Zero out bg_checksum at offset 0x1E (2 bytes) */
        buf[0x1E] = 0;
        buf[0x1F] = 0;
        crc = ext2fs_crc32c_le(fs->csum_seed, (const unsigned char *)&le_group, sizeof(le_group));
        crc = ext2fs_crc32c_le(crc, buf, copy_sz);
        return (__u16)(crc & 0xFFFFU);
    }
    if (EXT2_HAS_RO_COMPAT_FEATURE(fs->super, EXT4_FEATURE_RO_COMPAT_GDT_CSUM)) {
        __u16 crc = ext2fs_crc16(~0U, fs->super->s_uuid, sizeof(fs->super->s_uuid));
        crc = ext2fs_crc16(crc, &le_group, sizeof(le_group));
        crc = ext2fs_crc16(crc, gdp, offsetof(struct ext2_group_desc, bg_checksum));
        if (desc_size > EXT2_MIN_DESC_SIZE) {
            const unsigned char *ext = ((const unsigned char *)gdp) + EXT2_MIN_DESC_SIZE;
            crc = ext2fs_crc16(crc, ext, desc_size - EXT2_MIN_DESC_SIZE);
        }
        return crc;
    }
    return gdp->bg_checksum;
}

int ext2fs_group_desc_csum_verify(ext2_filsys fs, dgrp_t group) {
    struct ext4_group_desc *gdp;
    if (!ext2fs_has_feature_metadata_csum(fs->super) &&
        !EXT2_HAS_RO_COMPAT_FEATURE(fs->super, EXT4_FEATURE_RO_COMPAT_GDT_CSUM)) {
        return 1;
    }
    gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    return gdp->bg_checksum == calc_group_desc_csum(fs, group, gdp);
}

void ext2fs_group_desc_csum_set(ext2_filsys fs, dgrp_t group) {
    struct ext4_group_desc *gdp;
    if (!ext2fs_has_feature_metadata_csum(fs->super) &&
        !EXT2_HAS_RO_COMPAT_FEATURE(fs->super, EXT4_FEATURE_RO_COMPAT_GDT_CSUM)) {
        return;
    }
    gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    gdp->bg_checksum = calc_group_desc_csum(fs, group, gdp);
}

static __u32 calc_inode_csum(ext2_filsys fs, ext2_ino_t inum, const struct ext2_inode_large *inode) {
    unsigned int inode_size = EXT2_INODE_SIZE(fs->super);
    unsigned char buf[512];
    __u32 crc;
    __u32 ino_le = inum;
    __u32 gen_le = inode->i_generation;
    size_t copy_sz = inode_size > sizeof(buf) ? sizeof(buf) : inode_size;

    memcpy(buf, inode, copy_sz);
    /* Zero i_checksum_lo at 0x7C */
    buf[0x7C] = 0;
    buf[0x7D] = 0;
    if (copy_sz >= offsetof(struct ext2_inode_large, i_extra_isize) + 4) {
        /* Zero i_checksum_hi at 0x82 */
        buf[0x82] = 0;
        buf[0x83] = 0;
    }

    crc = ext2fs_crc32c_le(fs->csum_seed, (const unsigned char *)&ino_le, sizeof(ino_le));
    crc = ext2fs_crc32c_le(crc, (const unsigned char *)&gen_le, sizeof(gen_le));
    crc = ext2fs_crc32c_le(crc, buf, copy_sz);
    return crc;
}

errcode_t ext2fs_inode_csum_verify(ext2_filsys fs, ext2_ino_t inum, struct ext2_inode_large *inode) {
    __u32 provided, calculated;
    unsigned int inode_size = EXT2_INODE_SIZE(fs->super);

    if (!ext2fs_has_feature_metadata_csum(fs->super)) {
        return 0;
    }
    provided = inode->osd2.linux2.l_i_checksum_lo;
    if (inode_size > EXT2_GOOD_OLD_INODE_SIZE && inode->i_extra_isize >= 4) {
        provided |= ((__u32)inode->i_checksum_hi) << 16;
    }
    calculated = calc_inode_csum(fs, inum, inode);
    if (inode_size <= EXT2_GOOD_OLD_INODE_SIZE || inode->i_extra_isize < 4) {
        calculated &= 0xFFFFU;
    }
    return (provided == calculated) ? 0 : EXT2_ET_INODE_CSUM_INVALID;
}

errcode_t ext2fs_inode_csum_set(ext2_filsys fs, ext2_ino_t inum, struct ext2_inode_large *inode) {
    __u32 calculated;
    unsigned int inode_size = EXT2_INODE_SIZE(fs->super);

    if (!ext2fs_has_feature_metadata_csum(fs->super)) {
        return 0;
    }
    calculated = calc_inode_csum(fs, inum, inode);
    inode->osd2.linux2.l_i_checksum_lo = (__u16)(calculated & 0xFFFFU);
    if (inode_size > EXT2_GOOD_OLD_INODE_SIZE && inode->i_extra_isize >= 4) {
        inode->i_checksum_hi = (__u16)(calculated >> 16);
    }
    return 0;
}

static __u32 calc_extent_block_csum(ext2_filsys fs, ext2_ino_t inum, const struct ext4_extent_header *eh) {
    struct ext2_inode inode;
    __u32 gen_le = 0;
    __u32 ino_le = inum;
    __u32 crc;
    size_t len = fs->blocksize - sizeof(struct ext4_extent_tail);

    if (ext2fs_read_inode(fs, inum, &inode) == 0) {
        gen_le = inode.i_generation;
    }
    crc = ext2fs_crc32c_le(fs->csum_seed, (const unsigned char *)&ino_le, sizeof(ino_le));
    crc = ext2fs_crc32c_le(crc, (const unsigned char *)&gen_le, sizeof(gen_le));
    crc = ext2fs_crc32c_le(crc, (const unsigned char *)eh, len);
    return crc;
}

errcode_t ext2fs_extent_block_csum_verify(ext2_filsys fs, ext2_ino_t inum, struct ext4_extent_header *eh) {
    const struct ext4_extent_tail *tail;
    __u32 crc;
    if (!ext2fs_has_feature_metadata_csum(fs->super)) {
        return 0;
    }
    tail = (const struct ext4_extent_tail *)(((const unsigned char *)eh) + fs->blocksize - sizeof(struct ext4_extent_tail));
    crc = calc_extent_block_csum(fs, inum, eh);
    return (tail->et_checksum == crc) ? 0 : EXT2_ET_EXTENT_CSUM_INVALID;
}

errcode_t ext2fs_extent_block_csum_set(ext2_filsys fs, ext2_ino_t inum, struct ext4_extent_header *eh) {
    struct ext4_extent_tail *tail;
    if (!ext2fs_has_feature_metadata_csum(fs->super)) {
        return 0;
    }
    tail = (struct ext4_extent_tail *)(((unsigned char *)eh) + fs->blocksize - sizeof(struct ext4_extent_tail));
    tail->et_checksum = calc_extent_block_csum(fs, inum, eh);
    return 0;
}

static __u32 calc_dir_block_csum(ext2_filsys fs, ext2_ino_t inum, const struct ext2_dir_entry *dirent) {
    struct ext2_inode inode;
    __u32 gen_le = 0;
    __u32 ino_le = inum;
    __u32 crc;
    size_t len = fs->blocksize - sizeof(struct ext2_dir_entry_tail);

    if (ext2fs_read_inode(fs, inum, &inode) == 0) {
        gen_le = inode.i_generation;
    }
    crc = ext2fs_crc32c_le(fs->csum_seed, (const unsigned char *)&ino_le, sizeof(ino_le));
    crc = ext2fs_crc32c_le(crc, (const unsigned char *)&gen_le, sizeof(gen_le));
    crc = ext2fs_crc32c_le(crc, (const unsigned char *)dirent, len);
    return crc;
}

int ext2fs_dir_block_csum_verify(ext2_filsys fs, ext2_ino_t inum, struct ext2_dir_entry *dirent) {
    const struct ext2_dir_entry_tail *t;
    if (!ext2fs_has_feature_metadata_csum(fs->super)) {
        return 1;
    }
    t = (const struct ext2_dir_entry_tail *)(((const unsigned char *)dirent) + fs->blocksize - sizeof(struct ext2_dir_entry_tail));
    if (t->det_reserved_zero1 != 0 || t->det_rec_len != sizeof(struct ext2_dir_entry_tail) ||
        t->det_reserved_zero2 != 0 || t->det_reserved_ft != EXT2_FT_DIR_CSUM) {
        return 0;
    }
    return t->det_checksum == calc_dir_block_csum(fs, inum, dirent);
}

errcode_t ext2fs_dir_block_csum_set(ext2_filsys fs, ext2_ino_t inum, struct ext2_dir_entry *dirent) {
    struct ext2_dir_entry_tail *t;
    if (!ext2fs_has_feature_metadata_csum(fs->super)) {
        return 0;
    }
    t = (struct ext2_dir_entry_tail *)(((unsigned char *)dirent) + fs->blocksize - sizeof(struct ext2_dir_entry_tail));
    t->det_reserved_zero1 = 0;
    t->det_rec_len = sizeof(struct ext2_dir_entry_tail);
    t->det_reserved_zero2 = 0;
    t->det_reserved_ft = EXT2_FT_DIR_CSUM;
    t->det_checksum = calc_dir_block_csum(fs, inum, dirent);
    return 0;
}

int ext2fs_block_bitmap_csum_verify(ext2_filsys fs, dgrp_t group, char *bitmap, int size) {
    struct ext4_group_desc *gdp;
    __u32 provided, calculated;
    if (!ext2fs_has_feature_metadata_csum(fs->super)) {
        return 1;
    }
    gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    provided = gdp->bg_block_bitmap_csum_lo;
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        provided |= ((__u32)gdp->bg_block_bitmap_csum_hi) << 16;
    }
    calculated = ext2fs_crc32c_le(fs->csum_seed, (const unsigned char *)bitmap, (size_t)size);
    if (EXT2_DESC_SIZE(fs->super) < EXT2_MIN_DESC_SIZE_64BIT) {
        calculated &= 0xFFFFU;
    }
    return provided == calculated;
}

void ext2fs_block_bitmap_csum_set(ext2_filsys fs, dgrp_t group, char *bitmap, int size) {
    struct ext4_group_desc *gdp;
    __u32 calculated;
    if (!ext2fs_has_feature_metadata_csum(fs->super)) {
        return;
    }
    gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    calculated = ext2fs_crc32c_le(fs->csum_seed, (const unsigned char *)bitmap, (size_t)size);
    gdp->bg_block_bitmap_csum_lo = (__u16)(calculated & 0xFFFFU);
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        gdp->bg_block_bitmap_csum_hi = (__u16)(calculated >> 16);
    }
}

int ext2fs_inode_bitmap_csum_verify(ext2_filsys fs, dgrp_t group, char *bitmap, int size) {
    struct ext4_group_desc *gdp;
    __u32 provided, calculated;
    if (!ext2fs_has_feature_metadata_csum(fs->super)) {
        return 1;
    }
    gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    provided = gdp->bg_inode_bitmap_csum_lo;
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        provided |= ((__u32)gdp->bg_inode_bitmap_csum_hi) << 16;
    }
    calculated = ext2fs_crc32c_le(fs->csum_seed, (const unsigned char *)bitmap, (size_t)size);
    if (EXT2_DESC_SIZE(fs->super) < EXT2_MIN_DESC_SIZE_64BIT) {
        calculated &= 0xFFFFU;
    }
    return provided == calculated;
}

void ext2fs_inode_bitmap_csum_set(ext2_filsys fs, dgrp_t group, char *bitmap, int size) {
    struct ext4_group_desc *gdp;
    __u32 calculated;
    if (!ext2fs_has_feature_metadata_csum(fs->super)) {
        return;
    }
    gdp = ext4fs_group_desc(fs, fs->group_desc, group);
    calculated = ext2fs_crc32c_le(fs->csum_seed, (const unsigned char *)bitmap, (size_t)size);
    gdp->bg_inode_bitmap_csum_lo = (__u16)(calculated & 0xFFFFU);
    if (EXT2_DESC_SIZE(fs->super) >= EXT2_MIN_DESC_SIZE_64BIT) {
        gdp->bg_inode_bitmap_csum_hi = (__u16)(calculated >> 16);
    }
}
