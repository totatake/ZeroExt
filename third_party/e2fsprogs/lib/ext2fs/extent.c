#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "ext2fs/ext2fs.h"

#define EXT4_EXT_MAX_LEN 32768U

struct extent_path {
    char *buf;
    int entries;
    int max_entries;
    int depth;
    int curr; /* index of current entry */
    blk64_t end_blk;
    blk64_t pblk; /* 0 for root (inside inode) */
};

struct ext2_extent_handle {
    errcode_t magic;
    ext2_filsys fs;
    ext2_ino_t ino;
    struct ext2_inode *inode;
    struct ext2_inode inode_storage;
    int max_depth;
    int level;
    struct extent_path *path;
};

static struct ext4_extent_header *path_hdr(struct extent_path *p) {
    return (struct ext4_extent_header *)p->buf;
}

static struct ext4_extent *path_ext(struct extent_path *p, int idx) {
    return (struct ext4_extent *)(p->buf + sizeof(struct ext4_extent_header) + idx * sizeof(struct ext4_extent));
}

static struct ext4_extent_idx *path_idx(struct extent_path *p, int idx) {
    return (struct ext4_extent_idx *)(p->buf + sizeof(struct ext4_extent_header) + idx * sizeof(struct ext4_extent_idx));
}

static blk64_t idx_pblk(const struct ext4_extent_idx *ix) {
    return ((blk64_t)ix->ei_leaf_lo) | (((blk64_t)ix->ei_leaf_hi) << 32);
}

static void idx_set_pblk(struct ext4_extent_idx *ix, blk64_t blk) {
    ix->ei_leaf_lo = (__u32)(blk & 0xFFFFFFFFULL);
    ix->ei_leaf_hi = (__u16)(blk >> 32);
}

static blk64_t ext_pblk(const struct ext4_extent *ex) {
    return ((blk64_t)ex->ee_start_lo) | (((blk64_t)ex->ee_start_hi) << 32);
}

static void ext_set_pblk(struct ext4_extent *ex, blk64_t blk) {
    ex->ee_start_lo = (__u32)(blk & 0xFFFFFFFFULL);
    ex->ee_start_hi = (__u16)(blk >> 32);
}

static __u32 ext_len(const struct ext4_extent *ex) {
    return (ex->ee_len <= EXT4_EXT_MAX_LEN) ? ex->ee_len : (ex->ee_len - EXT4_EXT_MAX_LEN);
}

static int ext_is_uninit(const struct ext4_extent *ex) {
    return ex->ee_len > EXT4_EXT_MAX_LEN;
}

static errcode_t update_path_node(ext2_extent_handle_t handle, int level) {
    struct extent_path *p = &handle->path[level];
    struct ext4_extent_header *eh = path_hdr(p);

    eh->eh_entries = (__u16)p->entries;
    if (level == 0) {
        memcpy(handle->inode->i_block, p->buf, sizeof(handle->inode->i_block));
        return ext2fs_write_inode(handle->fs, handle->ino, handle->inode);
    }
    ext2fs_extent_block_csum_set(handle->fs, handle->ino, eh);
    return io_channel_write_blk64(handle->fs->io, p->pblk, 1, p->buf);
}

static errcode_t load_extent_block(ext2_extent_handle_t handle, int level, blk64_t blk) {
    struct extent_path *p = &handle->path[level];
    struct ext4_extent_header *eh;
    errcode_t retval;

    if (!p->buf) {
        retval = ext2fs_get_mem(handle->fs->blocksize, &p->buf);
        if (retval) return retval;
    }
    retval = io_channel_read_blk64(handle->fs->io, blk, 1, p->buf);
    if (retval) return retval;

    eh = path_hdr(p);
    if (eh->eh_magic != EXT4_EXT_MAGIC) {
        return EXT2_ET_EXTENT_HEADER_BAD;
    }
    if (!(handle->fs->flags & EXT2_FLAG_IGNORE_CSUM_ERRORS)) {
        retval = ext2fs_extent_block_csum_verify(handle->fs, handle->ino, eh);
        if (retval) return retval;
    }
    p->pblk = blk;
    p->depth = eh->eh_depth;
    p->entries = eh->eh_entries;
    p->max_entries = eh->eh_max;
    p->curr = 0;
    return 0;
}

static void fill_ext_from_path(struct extent_path *p, struct ext2fs_extent *extent) {
    if (p->depth == 0) {
        struct ext4_extent *ex = path_ext(p, p->curr);
        extent->e_lblk = ex->ee_block;
        extent->e_pblk = ext_pblk(ex);
        extent->e_len = ext_len(ex);
        extent->e_flags = EXT2_EXTENT_FLAGS_LEAF;
        if (ext_is_uninit(ex)) {
            extent->e_flags |= EXT2_EXTENT_FLAGS_UNINIT;
        }
    } else {
        struct ext4_extent_idx *ix = path_idx(p, p->curr);
        extent->e_lblk = ix->ei_block;
        extent->e_pblk = idx_pblk(ix);
        extent->e_len = 0;
        extent->e_flags = 0;
    }
}

errcode_t ext2fs_extent_open2(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode,
                              ext2_extent_handle_t *ret_handle) {
    ext2_extent_handle_t handle = NULL;
    struct ext4_extent_header *eh;
    errcode_t retval;
    int i;

    if (!fs || !ret_handle) return EXT2_ET_INVALID_ARGUMENT;

    retval = ext2fs_get_memzero(sizeof(*handle), &handle);
    if (retval) return retval;

    handle->magic = EXT2_ET_MAGIC_EXTENT_HANDLE;
    handle->fs = fs;
    handle->ino = ino;

    if (inode) {
        handle->inode = inode;
    } else {
        retval = ext2fs_read_inode(fs, ino, &handle->inode_storage);
        if (retval) goto err;
        handle->inode = &handle->inode_storage;
    }

    if (!(handle->inode->i_flags & EXT4_EXTENTS_FL)) {
        retval = EXT2_ET_INODE_NOT_EXTENT;
        goto err;
    }

    eh = (struct ext4_extent_header *)handle->inode->i_block;
    if (eh->eh_magic != EXT4_EXT_MAGIC) {
        retval = EXT2_ET_EXTENT_HEADER_BAD;
        goto err;
    }

    handle->max_depth = eh->eh_depth;
    retval = ext2fs_get_memzero((unsigned long)((handle->max_depth + 2) * sizeof(struct extent_path)), &handle->path);
    if (retval) goto err;

    handle->path[0].buf = (char *)malloc(sizeof(handle->inode->i_block));
    if (!handle->path[0].buf) {
        retval = EXT2_ET_NO_MEMORY;
        goto err;
    }
    memcpy(handle->path[0].buf, handle->inode->i_block, sizeof(handle->inode->i_block));
    handle->path[0].pblk = 0;
    handle->path[0].depth = eh->eh_depth;
    handle->path[0].entries = eh->eh_entries;
    handle->path[0].max_entries = eh->eh_max;
    handle->path[0].curr = 0;
    handle->level = 0;

    *ret_handle = handle;
    return 0;

err:
    if (handle) {
        if (handle->path) {
            for (i = 0; i <= handle->max_depth + 1; i++) {
                if (handle->path[i].buf) free(handle->path[i].buf);
            }
            ext2fs_free_mem(&handle->path);
        }
        ext2fs_free_mem(&handle);
    }
    return retval;
}

errcode_t ext2fs_extent_open(ext2_filsys fs, ext2_ino_t ino, ext2_extent_handle_t *ret_handle) {
    return ext2fs_extent_open2(fs, ino, NULL, ret_handle);
}

void ext2fs_extent_free(ext2_extent_handle_t handle) {
    int i;
    if (!handle || handle->magic != EXT2_ET_MAGIC_EXTENT_HANDLE) return;
    if (handle->path) {
        for (i = 0; i <= handle->max_depth + 1; i++) {
            if (handle->path[i].buf) {
                free(handle->path[i].buf);
                handle->path[i].buf = NULL;
            }
        }
        ext2fs_free_mem(&handle->path);
    }
    handle->magic = 0;
    free(handle);
}

errcode_t ext2fs_extent_get(ext2_extent_handle_t handle, int flags, struct ext2fs_extent *extent) {
    struct extent_path *p;
    errcode_t retval;

    if (!handle || handle->magic != EXT2_ET_MAGIC_EXTENT_HANDLE || !extent) {
        return EXT2_ET_INVALID_ARGUMENT;
    }

    switch (flags) {
    case EXT2_EXTENT_ROOT:
        handle->level = 0;
        handle->path[0].curr = 0;
        if (handle->path[0].entries == 0) return EXT2_ET_EXTENT_NOT_FOUND;
        fill_ext_from_path(&handle->path[0], extent);
        return 0;

    case EXT2_EXTENT_CURRENT:
        p = &handle->path[handle->level];
        if (p->entries == 0 || p->curr < 0 || p->curr >= p->entries) return EXT2_ET_EXTENT_NOT_FOUND;
        fill_ext_from_path(p, extent);
        return 0;

    case EXT2_EXTENT_DOWN:
        p = &handle->path[handle->level];
        if (p->depth == 0 || p->curr < 0 || p->curr >= p->entries) return EXT2_ET_EXTENT_NOT_FOUND;
        {
            blk64_t child_blk = idx_pblk(path_idx(p, p->curr));
            retval = load_extent_block(handle, handle->level + 1, child_blk);
            if (retval) return retval;
            handle->level++;
            p = &handle->path[handle->level];
            if (p->entries == 0) return EXT2_ET_EXTENT_NOT_FOUND;
            fill_ext_from_path(p, extent);
            return 0;
        }

    case EXT2_EXTENT_DOWN_AND_LAST:
        p = &handle->path[handle->level];
        if (p->depth == 0 || p->curr < 0 || p->curr >= p->entries) return EXT2_ET_EXTENT_NOT_FOUND;
        {
            blk64_t child_blk = idx_pblk(path_idx(p, p->curr));
            retval = load_extent_block(handle, handle->level + 1, child_blk);
            if (retval) return retval;
            handle->level++;
            p = &handle->path[handle->level];
            if (p->entries == 0) return EXT2_ET_EXTENT_NOT_FOUND;
            p->curr = p->entries - 1;
            fill_ext_from_path(p, extent);
            return 0;
        }

    case EXT2_EXTENT_UP:
        if (handle->level == 0) return EXT2_ET_EXTENT_NOT_FOUND;
        handle->level--;
        p = &handle->path[handle->level];
        fill_ext_from_path(p, extent);
        return 0;

    case EXT2_EXTENT_LAST_LEAF:
        handle->level = 0;
        if (handle->path[0].entries == 0) return EXT2_ET_EXTENT_NOT_FOUND;
        handle->path[0].curr = handle->path[0].entries - 1;
        while (handle->path[handle->level].depth > 0) {
            retval = ext2fs_extent_get(handle, EXT2_EXTENT_DOWN_AND_LAST, extent);
            if (retval) return retval;
        }
        fill_ext_from_path(&handle->path[handle->level], extent);
        return 0;

    case EXT2_EXTENT_NEXT_SIB:
        p = &handle->path[handle->level];
        if (p->curr + 1 < p->entries) {
            p->curr++;
            fill_ext_from_path(p, extent);
            return 0;
        }
        return EXT2_ET_EXTENT_NOT_FOUND;

    case EXT2_EXTENT_PREV_SIB:
        p = &handle->path[handle->level];
        if (p->curr > 0) {
            p->curr--;
            fill_ext_from_path(p, extent);
            return 0;
        }
        return EXT2_ET_EXTENT_NOT_FOUND;

    case EXT2_EXTENT_NEXT:
        p = &handle->path[handle->level];
        if (p->depth > 0) {
            return ext2fs_extent_get(handle, EXT2_EXTENT_DOWN, extent);
        }
        /* Try next sibling, else climb up */
        while (1) {
            if (ext2fs_extent_get(handle, EXT2_EXTENT_NEXT_SIB, extent) == 0) {
                return 0;
            }
            if (handle->level == 0) {
                return EXT2_ET_EXTENT_NOT_FOUND;
            }
            handle->level--;
        }

    case EXT2_EXTENT_NEXT_LEAF:
        /* Advance to next leaf */
        while (1) {
            p = &handle->path[handle->level];
            if (p->curr + 1 < p->entries) {
                p->curr++;
                while (handle->path[handle->level].depth > 0) {
                    retval = ext2fs_extent_get(handle, EXT2_EXTENT_DOWN, extent);
                    if (retval) return retval;
                }
                fill_ext_from_path(&handle->path[handle->level], extent);
                return 0;
            }
            if (handle->level == 0) return EXT2_ET_EXTENT_NOT_FOUND;
            handle->level--;
        }

    case EXT2_EXTENT_PREV_LEAF:
        while (1) {
            p = &handle->path[handle->level];
            if (p->curr > 0) {
                p->curr--;
                while (handle->path[handle->level].depth > 0) {
                    retval = ext2fs_extent_get(handle, EXT2_EXTENT_DOWN_AND_LAST, extent);
                    if (retval) return retval;
                }
                fill_ext_from_path(&handle->path[handle->level], extent);
                return 0;
            }
            if (handle->level == 0) return EXT2_ET_EXTENT_NOT_FOUND;
            handle->level--;
        }

    default:
        return EXT2_ET_INVALID_ARGUMENT;
    }
}

errcode_t ext2fs_extent_goto(ext2_extent_handle_t handle, blk64_t blk) {
    errcode_t retval;
    int i;

    if (!handle || handle->magic != EXT2_ET_MAGIC_EXTENT_HANDLE) return EXT2_ET_INVALID_ARGUMENT;

    handle->level = 0;
    if (handle->path[0].entries == 0) {
        return EXT2_ET_EXTENT_NOT_FOUND;
    }

    while (1) {
        struct extent_path *p = &handle->path[handle->level];
        int chosen = 0;
        if (p->depth > 0) {
            for (i = 0; i < p->entries; i++) {
                struct ext4_extent_idx *ix = path_idx(p, i);
                if (ix->ei_block <= blk) {
                    chosen = i;
                } else {
                    break;
                }
            }
            p->curr = chosen;
            retval = load_extent_block(handle, handle->level + 1, idx_pblk(path_idx(p, chosen)));
            if (retval) return retval;
            handle->level++;
        } else {
            chosen = -1;
            for (i = 0; i < p->entries; i++) {
                struct ext4_extent *ex = path_ext(p, i);
                if (ex->ee_block <= blk) {
                    chosen = i;
                } else {
                    break;
                }
            }
            if (chosen < 0) {
                p->curr = 0;
                return EXT2_ET_EXTENT_NOT_FOUND;
            }
            p->curr = chosen;
            {
                struct ext4_extent *ex = path_ext(p, chosen);
                if (blk < ex->ee_block + ext_len(ex)) {
                    return 0;
                }
            }
            return EXT2_ET_EXTENT_NOT_FOUND;
        }
    }
}

static errcode_t grow_tree_depth(ext2_extent_handle_t handle) {
    struct extent_path *root = &handle->path[0];
    blk64_t new_blk = 0;
    char *blk_buf = NULL;
    struct ext4_extent_header *new_eh;
    struct ext4_extent_header *root_eh;
    struct ext4_extent_idx *ix;
    int max_per_block;
    int new_max_depth;
    struct extent_path *new_path;
    errcode_t retval;
    int i;

    retval = ext2fs_get_memzero(handle->fs->blocksize, &blk_buf);
    if (retval) return retval;

    retval = ext2fs_alloc_block2(handle->fs, ext2fs_find_inode_goal(handle->fs, handle->ino, handle->inode, 0),
                                 NULL, &new_blk);
    if (retval) {
        ext2fs_free_mem(&blk_buf);
        return retval;
    }
    ext2fs_iblk_add_blocks(handle->fs, handle->inode, 1);

    max_per_block = (int)((handle->fs->blocksize - sizeof(struct ext4_extent_header) -
                           sizeof(struct ext4_extent_tail)) / sizeof(struct ext4_extent));

    new_eh = (struct ext4_extent_header *)blk_buf;
    new_eh->eh_magic = EXT4_EXT_MAGIC;
    new_eh->eh_entries = (__u16)root->entries;
    new_eh->eh_max = (__u16)max_per_block;
    new_eh->eh_depth = (__u16)root->depth;
    new_eh->eh_generation = 0;

    if (root->entries > 0) {
        memcpy(blk_buf + sizeof(struct ext4_extent_header),
               root->buf + sizeof(struct ext4_extent_header),
               (size_t)root->entries * sizeof(struct ext4_extent));
    }
    ext2fs_extent_block_csum_set(handle->fs, handle->ino, new_eh);
    retval = io_channel_write_blk64(handle->fs->io, new_blk, 1, blk_buf);
    ext2fs_free_mem(&blk_buf);
    if (retval) return retval;

    /* Update root to point to new_blk */
    root_eh = path_hdr(root);
    root_eh->eh_depth++;
    root_eh->eh_entries = 1;
    ix = path_idx(root, 0);
    if (root->depth == 0) {
        ix->ei_block = path_ext(root, 0)->ee_block;
    } else {
        ix->ei_block = path_idx(root, 0)->ei_block;
    }
    idx_set_pblk(ix, new_blk);
    ix->ei_unused = 0;

    root->depth = root_eh->eh_depth;
    root->entries = 1;
    root->curr = 0;

    new_max_depth = root->depth;
    new_path = (struct extent_path *)calloc((size_t)(new_max_depth + 2), sizeof(struct extent_path));
    if (!new_path) return EXT2_ET_NO_MEMORY;
    new_path[0] = handle->path[0];
    for (i = 1; i <= handle->max_depth + 1; i++) {
        if (handle->path[i].buf) free(handle->path[i].buf);
    }
    ext2fs_free_mem(&handle->path);
    handle->path = new_path;
    handle->max_depth = new_max_depth;
    handle->level = 0;

    return update_path_node(handle, 0);
}

static errcode_t split_node_and_insert(ext2_extent_handle_t handle, int level, blk64_t lblk,
                                       struct ext2fs_extent *new_ext);

static errcode_t insert_index_at(ext2_extent_handle_t handle, int level, blk64_t lblk, blk64_t child_blk) {
    struct extent_path *p = &handle->path[level];
    int pos = 0;
    int i;
    errcode_t retval;

    if (p->entries >= p->max_entries) {
        if (level == 0) {
            retval = grow_tree_depth(handle);
            if (retval) return retval;
            retval = load_extent_block(handle, 1, idx_pblk(path_idx(&handle->path[0], 0)));
            if (retval) return retval;
            level = 1;
            p = &handle->path[1];
        } else {
            struct ext2fs_extent dummy;
            memset(&dummy, 0, sizeof(dummy));
            dummy.e_lblk = lblk;
            dummy.e_pblk = child_blk;
            dummy.e_len = 0;
            dummy.e_flags = 0;
            return split_node_and_insert(handle, level, lblk, &dummy);
        }
    }

    for (i = 0; i < p->entries; i++) {
        if (path_idx(p, i)->ei_block > lblk) break;
        pos = i + 1;
    }
    if (pos < p->entries) {
        memmove(path_idx(p, pos + 1), path_idx(p, pos),
                (size_t)(p->entries - pos) * sizeof(struct ext4_extent_idx));
    }
    {
        struct ext4_extent_idx *ix = path_idx(p, pos);
        ix->ei_block = (__u32)lblk;
        idx_set_pblk(ix, child_blk);
        ix->ei_unused = 0;
    }
    p->entries++;
    p->curr = pos;
    return update_path_node(handle, level);
}

static errcode_t split_node_and_insert(ext2_extent_handle_t handle, int level, blk64_t lblk,
                                       struct ext2fs_extent *new_ext) {
    struct extent_path *p;
    blk64_t right_blk = 0;
    char *right_buf = NULL;
    struct ext4_extent_header *right_eh;
    int split_idx;
    int move_cnt;
    blk64_t split_lblk;
    int max_per_block;
    int old_max_depth;
    errcode_t retval;

    if (level == 0) {
        retval = grow_tree_depth(handle);
        if (retval) return retval;
        retval = load_extent_block(handle, 1, idx_pblk(path_idx(&handle->path[0], 0)));
        if (retval) return retval;
        handle->level = 1;
        return ext2fs_extent_insert(handle, EXT2_EXTENT_INSERT_AFTER, new_ext);
    }

    p = &handle->path[level];
    retval = ext2fs_get_memzero(handle->fs->blocksize, &right_buf);
    if (retval) return retval;

    retval = ext2fs_alloc_block2(handle->fs, ext2fs_find_inode_goal(handle->fs, handle->ino, handle->inode, lblk),
                                 NULL, &right_blk);
    if (retval) {
        ext2fs_free_mem(&right_buf);
        return retval;
    }
    ext2fs_iblk_add_blocks(handle->fs, handle->inode, 1);

    max_per_block = (int)((handle->fs->blocksize - sizeof(struct ext4_extent_header) -
                           sizeof(struct ext4_extent_tail)) / sizeof(struct ext4_extent));

    split_idx = p->entries / 2;
    move_cnt = p->entries - split_idx;

    right_eh = (struct ext4_extent_header *)right_buf;
    right_eh->eh_magic = EXT4_EXT_MAGIC;
    right_eh->eh_entries = (__u16)move_cnt;
    right_eh->eh_max = (__u16)max_per_block;
    right_eh->eh_depth = (__u16)p->depth;
    right_eh->eh_generation = 0;

    memcpy(right_buf + sizeof(struct ext4_extent_header),
           p->buf + sizeof(struct ext4_extent_header) + (size_t)split_idx * sizeof(struct ext4_extent),
           (size_t)move_cnt * sizeof(struct ext4_extent));

    if (p->depth == 0) {
        split_lblk = ((struct ext4_extent *)(right_buf + sizeof(struct ext4_extent_header)))->ee_block;
    } else {
        split_lblk = ((struct ext4_extent_idx *)(right_buf + sizeof(struct ext4_extent_header)))->ei_block;
    }

    p->entries = split_idx;
    retval = update_path_node(handle, level);
    if (retval) {
        ext2fs_free_mem(&right_buf);
        return retval;
    }
    ext2fs_extent_block_csum_set(handle->fs, handle->ino, right_eh);
    retval = io_channel_write_blk64(handle->fs->io, right_blk, 1, right_buf);
    ext2fs_free_mem(&right_buf);
    if (retval) return retval;

    old_max_depth = handle->max_depth;
    retval = insert_index_at(handle, level - 1, split_lblk, right_blk);
    if (retval) return retval;

    level += (handle->max_depth - old_max_depth);
    if (lblk >= split_lblk) {
        retval = load_extent_block(handle, level, right_blk);
        if (retval) return retval;
    }
    handle->level = level;
    if (new_ext->e_flags & EXT2_EXTENT_FLAGS_LEAF) {
        return ext2fs_extent_insert(handle, 0, new_ext);
    }
    return insert_index_at(handle, level, new_ext->e_lblk, new_ext->e_pblk);
}

errcode_t ext2fs_extent_insert(ext2_extent_handle_t handle, int flags, struct ext2fs_extent *extent) {
    struct extent_path *p;
    int pos = 0;
    int i;
    (void)flags;

    if (!handle || !extent) return EXT2_ET_INVALID_ARGUMENT;
    p = &handle->path[handle->level];
    if (p->depth != 0) return EXT2_ET_INVALID_ARGUMENT;

    if (p->entries >= p->max_entries) {
        return split_node_and_insert(handle, handle->level, extent->e_lblk, extent);
    }

    for (i = 0; i < p->entries; i++) {
        if (path_ext(p, i)->ee_block > extent->e_lblk) break;
        pos = i + 1;
    }
    if (pos < p->entries) {
        memmove(path_ext(p, pos + 1), path_ext(p, pos),
                (size_t)(p->entries - pos) * sizeof(struct ext4_extent));
    }
    {
        struct ext4_extent *ex = path_ext(p, pos);
        ex->ee_block = (__u32)extent->e_lblk;
        ex->ee_len = (__u16)extent->e_len;
        if (extent->e_flags & EXT2_EXTENT_FLAGS_UNINIT) {
            ex->ee_len += EXT4_EXT_MAX_LEN;
        }
        ext_set_pblk(ex, extent->e_pblk);
    }
    p->entries++;
    p->curr = pos;
    return update_path_node(handle, handle->level);
}

errcode_t ext2fs_extent_replace(ext2_extent_handle_t handle, int flags, struct ext2fs_extent *extent) {
    struct extent_path *p;
    struct ext4_extent *ex;
    (void)flags;

    if (!handle || !extent) return EXT2_ET_INVALID_ARGUMENT;
    p = &handle->path[handle->level];
    if (p->depth != 0 || p->curr < 0 || p->curr >= p->entries) return EXT2_ET_INVALID_ARGUMENT;

    ex = path_ext(p, p->curr);
    ex->ee_block = (__u32)extent->e_lblk;
    ex->ee_len = (__u16)extent->e_len;
    if (extent->e_flags & EXT2_EXTENT_FLAGS_UNINIT) {
        ex->ee_len += EXT4_EXT_MAX_LEN;
    }
    ext_set_pblk(ex, extent->e_pblk);
    return update_path_node(handle, handle->level);
}

errcode_t ext2fs_extent_delete(ext2_extent_handle_t handle, int flags) {
    struct extent_path *p;
    (void)flags;

    if (!handle) return EXT2_ET_INVALID_ARGUMENT;
    p = &handle->path[handle->level];
    if (p->depth != 0 || p->curr < 0 || p->curr >= p->entries) return EXT2_ET_INVALID_ARGUMENT;

    if (p->curr + 1 < p->entries) {
        memmove(path_ext(p, p->curr), path_ext(p, p->curr + 1),
                (size_t)(p->entries - p->curr - 1) * sizeof(struct ext4_extent));
    }
    p->entries--;
    if (p->curr >= p->entries && p->entries > 0) {
        p->curr = p->entries - 1;
    }
    return update_path_node(handle, handle->level);
}

errcode_t ext2fs_extent_set_bmap(ext2_extent_handle_t handle, blk64_t logical, blk64_t physical, int flags) {
    struct ext2fs_extent cur;
    struct extent_path *p;
    errcode_t retval;

    if (!handle) return EXT2_ET_INVALID_ARGUMENT;

    /* Navigate to leaf position just before or containing logical */
    retval = ext2fs_extent_goto(handle, logical);
    p = &handle->path[handle->level];

    /* Try merge with current/previous extent in same leaf */
    if (p->entries > 0 && p->curr >= 0 && p->curr < p->entries) {
        fill_ext_from_path(p, &cur);
        if (cur.e_lblk + cur.e_len == logical &&
            cur.e_pblk + cur.e_len == physical &&
            cur.e_len < EXT4_EXT_MAX_LEN &&
            ((cur.e_flags & EXT2_EXTENT_FLAGS_UNINIT) == 0)) {
            cur.e_len++;
            return ext2fs_extent_replace(handle, 0, &cur);
        }
    }

    memset(&cur, 0, sizeof(cur));
    cur.e_lblk = logical;
    cur.e_pblk = physical;
    cur.e_len = 1;
    cur.e_flags = EXT2_EXTENT_FLAGS_LEAF | (flags & EXT2_EXTENT_FLAGS_UNINIT);
    return ext2fs_extent_insert(handle, 0, &cur);
}

static errcode_t free_extent_node_recursive(ext2_filsys fs, char *buf, int is_root) {
    struct ext4_extent_header *eh = (struct ext4_extent_header *)buf;
    int i;
    errcode_t retval = 0;

    if (eh->eh_magic != EXT4_EXT_MAGIC) {
        return EXT2_ET_EXTENT_HEADER_BAD;
    }
    if (eh->eh_depth == 0) {
        struct ext4_extent *ex = (struct ext4_extent *)(buf + sizeof(*eh));
        for (i = 0; i < eh->eh_entries; i++) {
            blk64_t pblk = ext_pblk(&ex[i]);
            __u32 len = ext_len(&ex[i]);
            __u32 k;
            for (k = 0; k < len; k++) {
                ext2fs_block_alloc_stats2(fs, pblk + k, -1);
            }
        }
    } else {
        struct ext4_extent_idx *ix = (struct ext4_extent_idx *)(buf + sizeof(*eh));
        char *child_buf = NULL;
        retval = ext2fs_get_mem(fs->blocksize, &child_buf);
        if (retval) return retval;
        for (i = 0; i < eh->eh_entries; i++) {
            blk64_t cblk = idx_pblk(&ix[i]);
            retval = io_channel_read_blk64(fs->io, cblk, 1, child_buf);
            if (retval == 0) {
                retval = free_extent_node_recursive(fs, child_buf, 0);
            }
            ext2fs_block_alloc_stats2(fs, cblk, -1);
            if (retval) break;
        }
        ext2fs_free_mem(&child_buf);
    }
    (void)is_root;
    return retval;
}

errcode_t ext2fs_extent_free_all(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode) {
    struct ext4_extent_header *eh;
    errcode_t retval;

    if (!fs || !inode) return EXT2_ET_INVALID_ARGUMENT;
    if (!(inode->i_flags & EXT4_EXTENTS_FL)) return 0;

    retval = free_extent_node_recursive(fs, (char *)inode->i_block, 1);
    if (retval) return retval;

    memset(inode->i_block, 0, sizeof(inode->i_block));
    eh = (struct ext4_extent_header *)inode->i_block;
    eh->eh_magic = EXT4_EXT_MAGIC;
    eh->eh_entries = 0;
    eh->eh_max = 4;
    eh->eh_depth = 0;
    eh->eh_generation = 0;
    inode->i_blocks = 0;
    inode->osd2.linux2.l_i_blocks_hi = 0;
    return ext2fs_write_inode(fs, ino, inode);
}
