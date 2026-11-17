#include "config.h"
#include <string.h>
#include <time.h>
#include "ext2fs/ext2fs.h"

int ext2fs_is_fast_symlink(struct ext2_inode *inode) {
    if (!inode) return 0;
    return LINUX_S_ISLNK(inode->i_mode) && (EXT2_I_SIZE(inode) < sizeof(inode->i_block)) &&
           (inode->i_blocks == 0);
}

errcode_t ext2fs_symlink(ext2_filsys fs, ext2_ino_t parent, ext2_ino_t ino, const char *name, const char *target) {
    struct ext2_inode_large inode;
    size_t target_len;
    __u32 now = (__u32)time(NULL);
    errcode_t retval;
    int fast_link;

    if (!fs || !name || !target) return EXT2_ET_INVALID_ARGUMENT;
    if (!(fs->flags & EXT2_FLAG_RW)) return EXT2_ET_RO_FILSYS;

    target_len = strlen(target);
    if (target_len >= fs->blocksize) return EXT2_ET_INVALID_ARGUMENT;

    if (ino == 0) {
        retval = ext2fs_new_inode(fs, parent, LINUX_S_IFLNK | 0777, fs->inode_map, &ino);
        if (retval) return retval;
    }

    memset(&inode, 0, sizeof(inode));
    inode.i_mode = LINUX_S_IFLNK | 0777;
    inode.i_links_count = 1;
    inode.i_size = (__u32)target_len;
    inode.i_atime = now;
    inode.i_ctime = now;
    inode.i_mtime = now;
    inode.i_crtime = now;
    inode.i_generation = 1;
    inode.i_extra_isize = 32;

    fast_link = (target_len < sizeof(inode.i_block));
    if (fast_link) {
        memcpy(inode.i_block, target, target_len);
        retval = ext2fs_write_inode_full(fs, ino, (struct ext2_inode *)&inode, sizeof(inode));
        if (retval) return retval;
    } else {
        ext2_file_t f = NULL;
        unsigned int written = 0;
        if (EXT2_HAS_INCOMPAT_FEATURE(fs->super, EXT4_FEATURE_INCOMPAT_EXTENTS)) {
            struct ext4_extent_header *eh = (struct ext4_extent_header *)inode.i_block;
            inode.i_flags |= EXT4_EXTENTS_FL;
            eh->eh_magic = EXT4_EXT_MAGIC;
            eh->eh_entries = 0;
            eh->eh_max = 4;
            eh->eh_depth = 0;
        }
        retval = ext2fs_write_inode_full(fs, ino, (struct ext2_inode *)&inode, sizeof(inode));
        if (retval) return retval;
        retval = ext2fs_file_open2(fs, ino, (struct ext2_inode *)&inode, EXT2_FILE_WRITE, &f);
        if (retval) return retval;
        retval = ext2fs_file_write(f, target, (unsigned int)target_len, &written);
        ext2fs_file_close(f);
        if (retval) return retval;
    }

    ext2fs_inode_alloc_stats2(fs, ino, 1, 0);
    retval = ext2fs_link(fs, parent, name, ino, EXT2_FT_SYMLINK);
    if (retval) {
        ext2fs_inode_alloc_stats2(fs, ino, -1, 0);
        return retval;
    }
    return 0;
}
