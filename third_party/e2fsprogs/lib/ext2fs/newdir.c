#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "ext2fs/ext2fs.h"

errcode_t ext2fs_new_dir_block(ext2_filsys fs, ext2_ino_t dir_ino, ext2_ino_t parent_ino, char **block) {
    char *buf = NULL;
    struct ext2_dir_entry_2 *de;
    unsigned int usable_size;
    errcode_t retval;

    if (!fs || !block) return EXT2_ET_INVALID_ARGUMENT;

    retval = ext2fs_get_memzero(fs->blocksize, &buf);
    if (retval) return retval;

    usable_size = fs->blocksize;
    if (ext2fs_has_feature_metadata_csum(fs->super)) {
        usable_size -= sizeof(struct ext2_dir_entry_tail);
    }

    if (dir_ino == 0) {
        de = (struct ext2_dir_entry_2 *)buf;
        de->inode = 0;
        de->rec_len = (__u16)usable_size;
    } else {
        int rec1 = EXT2_DIR_REC_LEN(1);
        de = (struct ext2_dir_entry_2 *)buf;
        de->inode = dir_ino;
        de->rec_len = (__u16)rec1;
        de->name_len = 1;
        de->file_type = EXT2_HAS_INCOMPAT_FEATURE(fs->super, EXT2_FEATURE_INCOMPAT_FILETYPE) ? EXT2_FT_DIR : 0;
        de->name[0] = '.';

        de = (struct ext2_dir_entry_2 *)(buf + rec1);
        de->inode = parent_ino;
        de->rec_len = (__u16)(usable_size - rec1);
        de->name_len = 2;
        de->file_type = EXT2_HAS_INCOMPAT_FEATURE(fs->super, EXT2_FEATURE_INCOMPAT_FILETYPE) ? EXT2_FT_DIR : 0;
        de->name[0] = '.';
        de->name[1] = '.';
    }

    if (ext2fs_has_feature_metadata_csum(fs->super)) {
        struct ext2_dir_entry_tail *t = (struct ext2_dir_entry_tail *)(buf + usable_size);
        t->det_reserved_zero1 = 0;
        t->det_rec_len = sizeof(struct ext2_dir_entry_tail);
        t->det_reserved_zero2 = 0;
        t->det_reserved_ft = EXT2_FT_DIR_CSUM;
    }

    *block = buf;
    return 0;
}
