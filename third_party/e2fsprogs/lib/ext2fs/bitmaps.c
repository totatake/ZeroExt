#include "config.h"
#include "ext2fs/ext2fs.h"

/* Bitmap helper functions beyond gen_bitmap64.c */
errcode_t ext2fs_copy_bitmap(ext2fs_generic_bitmap src, ext2fs_generic_bitmap *dest) {
    (void)src;
    (void)dest;
    return EXT2_ET_UNIMPLEMENTED;
}
