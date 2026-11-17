#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "ext2fs/ext2fs.h"

struct ext2_file {
    errcode_t magic;
    ext2_filsys fs;
    ext2_ino_t ino;
    struct ext2_inode inode;
    int flags;
    __u64 pos;
    blk64_t blockno;
    blk64_t physblock;
    char *buf;
    int buf_valid;
    int buf_dirty;
    int inode_dirty;
};

errcode_t ext2fs_file_open2(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode,
                            int flags, ext2_file_t *ret) {
    ext2_file_t file = NULL;
    errcode_t retval;

    if (!fs || !ret) return EXT2_ET_INVALID_ARGUMENT;
    if ((flags & EXT2_FILE_WRITE) && !(fs->flags & EXT2_FLAG_RW)) {
        return EXT2_ET_RO_FILSYS;
    }

    retval = ext2fs_get_memzero(sizeof(*file), &file);
    if (retval) return retval;

    file->magic = EXT2_ET_MAGIC_EXT2_FILE;
    file->fs = fs;
    file->ino = ino;
    file->flags = flags;

    if (inode) {
        file->inode = *inode;
    } else {
        retval = ext2fs_read_inode(fs, ino, &file->inode);
        if (retval) {
            ext2fs_free_mem(&file);
            return retval;
        }
    }

    retval = ext2fs_get_mem(fs->blocksize, &file->buf);
    if (retval) {
        ext2fs_free_mem(&file);
        return retval;
    }

    *ret = file;
    return 0;
}

errcode_t ext2fs_file_open(ext2_filsys fs, ext2_ino_t ino, int flags, ext2_file_t *ret) {
    return ext2fs_file_open2(fs, ino, NULL, flags, ret);
}

static errcode_t sync_file_buffer(ext2_file_t file) {
    errcode_t retval = 0;
    if (file->buf_valid && file->buf_dirty && file->physblock) {
        retval = io_channel_write_blk64(file->fs->io, file->physblock, 1, file->buf);
        if (retval == 0) {
            file->buf_dirty = 0;
        }
    }
    return retval;
}

errcode_t ext2fs_file_flush(ext2_file_t file) {
    errcode_t retval;
    if (!file || file->magic != EXT2_ET_MAGIC_EXT2_FILE) return EXT2_ET_MAGIC_EXT2_FILE;
    retval = sync_file_buffer(file);
    if (retval) return retval;
    if (file->inode_dirty && (file->flags & EXT2_FILE_WRITE)) {
        retval = ext2fs_write_inode(file->fs, file->ino, &file->inode);
        if (retval == 0) {
            file->inode_dirty = 0;
        }
    }
    return retval;
}

errcode_t ext2fs_file_close(ext2_file_t file) {
    errcode_t retval = 0;
    if (!file) return 0;
    if (file->magic != EXT2_ET_MAGIC_EXT2_FILE) return EXT2_ET_MAGIC_EXT2_FILE;
    retval = ext2fs_file_flush(file);
    ext2fs_free_mem(&file->buf);
    file->magic = 0;
    free(file);
    return retval;
}

errcode_t ext2fs_file_llseek(ext2_file_t file, __u64 offset, int whence, __u64 *ret_pos) {
    __u64 size;
    if (!file || file->magic != EXT2_ET_MAGIC_EXT2_FILE) return EXT2_ET_MAGIC_EXT2_FILE;
    size = EXT2_I_SIZE(&file->inode);

    switch (whence) {
    case EXT2_SEEK_SET:
        file->pos = offset;
        break;
    case EXT2_SEEK_CUR:
        file->pos += offset;
        break;
    case EXT2_SEEK_END:
        file->pos = size + offset;
        break;
    default:
        return EXT2_ET_INVALID_ARGUMENT;
    }
    if (ret_pos) *ret_pos = file->pos;
    return 0;
}

static errcode_t load_file_block(ext2_file_t file, blk64_t lblock, int alloc) {
    int ret_flags = 0;
    blk64_t pblock = 0;
    errcode_t retval;

    if (file->buf_valid && file->blockno == lblock) {
        if (!alloc || file->physblock != 0) {
            return 0;
        }
    }

    retval = sync_file_buffer(file);
    if (retval) return retval;

    retval = ext2fs_bmap2(file->fs, file->ino, &file->inode, NULL,
                          alloc ? (BMAP_ALLOC | BMAP_ZERO) : 0,
                          lblock, &ret_flags, &pblock);
    if (retval) return retval;

    if (pblock == 0 || (ret_flags & BMAP_RET_UNINIT)) {
        memset(file->buf, 0, file->fs->blocksize);
    } else {
        retval = io_channel_read_blk64(file->fs->io, pblock, 1, file->buf);
        if (retval) return retval;
    }

    file->blockno = lblock;
    file->physblock = pblock;
    file->buf_valid = 1;
    file->buf_dirty = 0;
    return 0;
}

errcode_t ext2fs_file_read(ext2_file_t file, void *buf, unsigned int wanted, unsigned int *got) {
    __u64 file_size;
    unsigned int left;
    unsigned int copied = 0;
    char *dst = (char *)buf;
    errcode_t retval = 0;

    if (!file || file->magic != EXT2_ET_MAGIC_EXT2_FILE) return EXT2_ET_MAGIC_EXT2_FILE;
    if (got) *got = 0;
    if (!buf && wanted > 0) return EXT2_ET_INVALID_ARGUMENT;

    file_size = EXT2_I_SIZE(&file->inode);
    if (file->pos >= file_size || wanted == 0) {
        return 0;
    }

    left = wanted;
    if (file->pos + left > file_size) {
        left = (unsigned int)(file_size - file->pos);
    }

    while (left > 0) {
        blk64_t lblock = file->pos / file->fs->blocksize;
        unsigned int off = (unsigned int)(file->pos % file->fs->blocksize);
        unsigned int chunk = file->fs->blocksize - off;
        if (chunk > left) chunk = left;

        /* Fast path for full-block unbuffered aligned reads */
        if (off == 0 && chunk == file->fs->blocksize && !(file->buf_valid && file->blockno == lblock && file->buf_dirty)) {
            int ret_flags = 0;
            blk64_t pblock = 0;
            retval = ext2fs_bmap2(file->fs, file->ino, &file->inode, NULL, 0, lblock, &ret_flags, &pblock);
            if (retval) break;
            if (pblock == 0 || (ret_flags & BMAP_RET_UNINIT)) {
                memset(dst, 0, chunk);
            } else {
                retval = io_channel_read_blk64(file->fs->io, pblock, 1, dst);
                if (retval) break;
            }
        } else {
            retval = load_file_block(file, lblock, 0);
            if (retval) break;
            memcpy(dst, file->buf + off, chunk);
        }

        dst += chunk;
        file->pos += chunk;
        copied += chunk;
        left -= chunk;
    }

    if (got) *got = copied;
    return retval;
}

errcode_t ext2fs_file_write(ext2_file_t file, const void *buf, unsigned int nbytes, unsigned int *written) {
    unsigned int left = nbytes;
    unsigned int done = 0;
    const char *src = (const char *)buf;
    __u64 cur_size;
    errcode_t retval = 0;

    if (!file || file->magic != EXT2_ET_MAGIC_EXT2_FILE) return EXT2_ET_MAGIC_EXT2_FILE;
    if (!(file->flags & EXT2_FILE_WRITE)) return EXT2_ET_FILE_RO;
    if (written) *written = 0;
    if (!buf && nbytes > 0) return EXT2_ET_INVALID_ARGUMENT;

    while (left > 0) {
        blk64_t lblock = file->pos / file->fs->blocksize;
        unsigned int off = (unsigned int)(file->pos % file->fs->blocksize);
        unsigned int chunk = file->fs->blocksize - off;
        if (chunk > left) chunk = left;

        retval = load_file_block(file, lblock, 1);
        if (retval) break;

        memcpy(file->buf + off, src, chunk);
        file->buf_dirty = 1;
        file->inode_dirty = 1;

        src += chunk;
        file->pos += chunk;
        done += chunk;
        left -= chunk;
    }

    cur_size = EXT2_I_SIZE(&file->inode);
    if (file->pos > cur_size) {
        ext2fs_inode_size_set(file->fs, &file->inode, file->pos);
        file->inode_dirty = 1;
    }

    if (retval == 0) {
        retval = ext2fs_file_flush(file);
    }
    if (written) *written = done;
    return retval;
}

errcode_t ext2fs_file_get_size(ext2_file_t file, ext2_off64_t *ret_size) {
    if (!file || file->magic != EXT2_ET_MAGIC_EXT2_FILE || !ret_size) return EXT2_ET_INVALID_ARGUMENT;
    *ret_size = EXT2_I_SIZE(&file->inode);
    return 0;
}

errcode_t ext2fs_file_set_size2(ext2_file_t file, ext2_off64_t size) {
    ext2_off64_t old_size;
    errcode_t retval = 0;

    if (!file || file->magic != EXT2_ET_MAGIC_EXT2_FILE) return EXT2_ET_MAGIC_EXT2_FILE;
    if (!(file->flags & EXT2_FILE_WRITE)) return EXT2_ET_FILE_RO;

    retval = ext2fs_file_flush(file);
    if (retval) return retval;

    old_size = EXT2_I_SIZE(&file->inode);
    if (size == old_size) return 0;

    if (size < old_size) {
        blk64_t first_drop = (size + file->fs->blocksize - 1) / file->fs->blocksize;
        retval = ext2fs_punch(file->fs, file->ino, &file->inode, NULL, first_drop, (blk64_t)-1);
        if (retval) return retval;
        if (size % file->fs->blocksize != 0) {
            blk64_t tail_blk = size / file->fs->blocksize;
            unsigned int off = (unsigned int)(size % file->fs->blocksize);
            if (load_file_block(file, tail_blk, 0) == 0 && file->physblock != 0) {
                memset(file->buf + off, 0, file->fs->blocksize - off);
                file->buf_dirty = 1;
                sync_file_buffer(file);
            }
        }
        file->buf_valid = 0;
    }

    ext2fs_inode_size_set(file->fs, &file->inode, size);
    file->inode_dirty = 1;
    return ext2fs_file_flush(file);
}

struct ext2_inode *ext2fs_file_get_inode(ext2_file_t file) {
    return file ? &file->inode : NULL;
}

ext2_filsys ext2fs_file_get_fs(ext2_file_t file) {
    return file ? file->fs : NULL;
}
