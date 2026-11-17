#include "config.h"
#include "ext2fs/ext2fs.h"

int ext2fs_file_type(int mode) {
    if (LINUX_S_ISREG(mode)) return EXT2_FT_REG_FILE;
    if (LINUX_S_ISDIR(mode)) return EXT2_FT_DIR;
    if (LINUX_S_ISLNK(mode)) return EXT2_FT_SYMLINK;
    if ((mode & LINUX_S_IFMT) == LINUX_S_IFCHR) return EXT2_FT_CHRDEV;
    if ((mode & LINUX_S_IFMT) == LINUX_S_IFBLK) return EXT2_FT_BLKDEV;
    if ((mode & LINUX_S_IFMT) == LINUX_S_IFIFO) return EXT2_FT_FIFO;
    if ((mode & LINUX_S_IFMT) == LINUX_S_IFSOCK) return EXT2_FT_SOCK;
    return EXT2_FT_UNKNOWN;
}
