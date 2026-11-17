#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "ext2fs/ext2fs.h"

errcode_t ext2fs_get_mem(unsigned long size, void *ptr) {
    void **pp = (void **)ptr;
    void *p;
    if (!pp) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    p = malloc(size ? size : 1);
    if (!p) {
        return EXT2_ET_NO_MEMORY;
    }
    *pp = p;
    return 0;
}

errcode_t ext2fs_get_memzero(unsigned long size, void *ptr) {
    void **pp = (void **)ptr;
    void *p;
    if (!pp) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    p = calloc(1, size ? size : 1);
    if (!p) {
        return EXT2_ET_NO_MEMORY;
    }
    *pp = p;
    return 0;
}

errcode_t ext2fs_free_mem(void *ptr) {
    void **pp = (void **)ptr;
    if (pp && *pp) {
        free(*pp);
        *pp = NULL;
    }
    return 0;
}

errcode_t ext2fs_resize_mem(unsigned long old_size, unsigned long size, void *ptr) {
    void **pp = (void **)ptr;
    void *p;
    (void)old_size;
    if (!pp) {
        return EXT2_ET_INVALID_ARGUMENT;
    }
    p = realloc(*pp, size ? size : 1);
    if (!p) {
        return EXT2_ET_NO_MEMORY;
    }
    *pp = p;
    return 0;
}

void ext2fs_mark_super_dirty(ext2_filsys fs) {
    if (fs) {
        fs->flags |= EXT2_FLAG_DIRTY | EXT2_FLAG_CHANGED;
    }
}

void ext2fs_mark_changed(ext2_filsys fs) {
    if (fs) {
        fs->flags |= EXT2_FLAG_CHANGED;
    }
}

void ext2fs_mark_ib_dirty(ext2_filsys fs) {
    if (fs) {
        fs->flags |= EXT2_FLAG_IB_DIRTY | EXT2_FLAG_CHANGED;
    }
}

void ext2fs_mark_bb_dirty(ext2_filsys fs) {
    if (fs) {
        fs->flags |= EXT2_FLAG_BB_DIRTY | EXT2_FLAG_CHANGED;
    }
}

int ext2fs_test_valid(ext2_filsys fs) {
    return (fs && (fs->flags & EXT2_FLAG_VALID)) ? 1 : 0;
}

void ext2fs_mark_valid(ext2_filsys fs) {
    if (fs) {
        fs->flags |= EXT2_FLAG_VALID;
    }
}

void ext2fs_unmark_valid(ext2_filsys fs) {
    if (fs) {
        fs->flags &= ~EXT2_FLAG_VALID;
    }
}

void ext2fs_free(ext2_filsys fs) {
    if (!fs || fs->magic != EXT2_ET_MAGIC_EXT2FS_FILSYS) {
        return;
    }
    if (fs->io) {
        io_channel_close(fs->io);
        fs->io = NULL;
    }
    ext2fs_free_mem(&fs->device_name);
    ext2fs_free_mem(&fs->super);
    ext2fs_free_mem(&fs->group_desc);
    if (fs->block_map) {
        ext2fs_free_block_bitmap(fs->block_map);
        fs->block_map = NULL;
    }
    if (fs->inode_map) {
        ext2fs_free_inode_bitmap(fs->inode_map);
        fs->inode_map = NULL;
    }
    fs->magic = 0;
    free(fs);
}
