#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "ext2fs/ext2fs.h"

struct dir_iter_ctx {
    ext2_ino_t dir;
    int flags;
    int (*func)(ext2_ino_t, int, struct ext2_dir_entry *, int, int, char *, void *);
    void *priv_data;
    errcode_t err;
    int aborted;
    int is_dx;
};

static int is_dir_tail(ext2_filsys fs, struct ext2_dir_entry_2 *de, int offset) {
    if (!ext2fs_has_feature_metadata_csum(fs->super)) {
        return 0;
    }
    if (offset == (int)(fs->blocksize - sizeof(struct ext2_dir_entry_tail)) &&
        de->inode == 0 &&
        de->rec_len == sizeof(struct ext2_dir_entry_tail) &&
        de->name_len == 0 &&
        de->file_type == EXT2_FT_DIR_CSUM) {
        return 1;
    }
    return 0;
}

static int dir_block_cb(ext2_filsys fs, blk64_t *blocknr, e2_blkcnt_t blockcnt,
                        blk64_t ref_blk, int ref_offset, void *priv_data) {
    struct dir_iter_ctx *ctx = (struct dir_iter_ctx *)priv_data;
    char *buf = NULL;
    int offset = 0;
    int changed = 0;
    int ret = 0;
    errcode_t retval;
    (void)ref_blk;
    (void)ref_offset;

    if (blockcnt < 0 || *blocknr == 0) {
        return 0;
    }

    retval = ext2fs_get_mem(fs->blocksize, &buf);
    if (retval) {
        ctx->err = retval;
        return BLOCK_ABORT;
    }

    retval = ext2fs_read_dir_block4(fs, *blocknr, buf, 0, ctx->dir);
    if (retval) {
        ext2fs_free_mem(&buf);
        ctx->err = retval;
        return BLOCK_ABORT;
    }

    /* Check if this is an internal HTree dx_node (blockcnt > 0 && inode == 0 && rec_len == blocksize) */
    if (ctx->is_dx && blockcnt > 0) {
        struct ext2_dir_entry_2 *first = (struct ext2_dir_entry_2 *)buf;
        if (first->inode == 0 && first->rec_len == fs->blocksize) {
            ext2fs_free_mem(&buf);
            return 0;
        }
    }

    while (offset < (int)fs->blocksize) {
        struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)(buf + offset);
        int rlen = de->rec_len;
        if (rlen < 8 || (rlen & 3) != 0 || offset + rlen > (int)fs->blocksize) {
            ctx->err = EXT2_ET_DIR_CORRUPTED;
            ret = BLOCK_ABORT;
            break;
        }
        if (is_dir_tail(fs, de, offset)) {
            break;
        }
        if (de->inode != 0 || (ctx->flags & DIRENT_FLAG_INCLUDE_EMPTY)) {
            int cb_res = ctx->func(ctx->dir, 0, (struct ext2_dir_entry *)de,
                                   offset, (int)fs->blocksize, buf, ctx->priv_data);
            if (cb_res & DIRENT_CHANGED) {
                changed = 1;
            }
            if (cb_res & DIRENT_ABORT) {
                ctx->aborted = 1;
                ret = BLOCK_ABORT;
                break;
            }
            if (cb_res & DIRENT_ERROR) {
                ctx->err = EXT2_ET_DIR_CORRUPTED;
                ret = BLOCK_ABORT;
                break;
            }
        }
        offset += de->rec_len;
    }

    if (changed && (fs->flags & EXT2_FLAG_RW)) {
        retval = ext2fs_write_dir_block4(fs, *blocknr, buf, 0, ctx->dir);
        if (retval && !ctx->err) {
            ctx->err = retval;
        }
    }

    ext2fs_free_mem(&buf);
    return ret;
}

int ext2fs_dir_iterate2(ext2_filsys fs, ext2_ino_t dir, int flags, char *block_buf,
                        int (*func)(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                                    int offset, int blocksize, char *buf, void *priv_data),
                        void *priv_data) {
    struct ext2_inode inode;
    struct dir_iter_ctx ctx;
    errcode_t retval;
    (void)block_buf;

    if (!fs || !func) return EXT2_ET_INVALID_ARGUMENT;

    retval = ext2fs_read_inode(fs, dir, &inode);
    if (retval) return (int)retval;
    if (!LINUX_S_ISDIR(inode.i_mode)) return EXT2_ET_NO_DIRECTORY;

    memset(&ctx, 0, sizeof(ctx));
    ctx.dir = dir;
    ctx.flags = flags;
    ctx.func = func;
    ctx.priv_data = priv_data;
    ctx.is_dx = (inode.i_flags & EXT2_INDEX_FL) != 0;

    retval = ext2fs_block_iterate3(fs, dir, BLOCK_FLAG_DATA_ONLY | BLOCK_FLAG_READ_ONLY, NULL, dir_block_cb, &ctx);
    if (retval) return (int)retval;
    if (ctx.err) return (int)ctx.err;
    return 0;
}
